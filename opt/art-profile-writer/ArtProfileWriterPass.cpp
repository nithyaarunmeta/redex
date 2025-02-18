/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/* This pass optionally creates a baseline profile file in a superset of the
 * human-readable ART profile format (HRF) according to
 * https://developer.android.com/topic/performance/baselineprofiles/manually-create-measure#define-rules-manually
 * .
 */

#include "ArtProfileWriterPass.h"

#include <boost/algorithm/string.hpp>
#include <fstream>
#include <string>

#include "ConcurrentContainers.h"
#include "ConfigFiles.h"
#include "DexStructure.h"
#include "IRCode.h"
#include "MethodProfiles.h"
#include "PassManager.h"
#include "Show.h"
#include "Walkers.h"

namespace {
const std::string BASELINE_PROFILES_FILE = "additional-baseline-profiles.list";

struct ArtProfileEntryFlags {
  bool hot{false};
  bool startup{false};
  bool not_startup{false};
};

bool is_simple(DexMethod* method, IRInstruction** invoke_insn = nullptr) {
  auto* code = method->get_code();
  always_assert(code->editable_cfg_built());
  auto& cfg = code->cfg();
  if (cfg.blocks().size() != 1) {
    return false;
  }
  auto* b = cfg.entry_block();
  auto last_it = b->get_last_insn();
  if (last_it == b->end() || !opcode::is_a_return(last_it->insn->opcode())) {
    return false;
  }
  auto ii = InstructionIterable(b);
  auto it = ii.begin();
  always_assert(it != ii.end());
  while (opcode::is_a_load_param(it->insn->opcode())) {
    ++it;
    always_assert(it != ii.end());
  }
  if (opcode::is_a_const(it->insn->opcode())) {
    ++it;
    always_assert(it != ii.end());
  } else if ((opcode::is_an_iget(it->insn->opcode()) ||
              opcode::is_an_sget(it->insn->opcode()))) {
    ++it;
    always_assert(it != ii.end());
  } else if (opcode::is_an_invoke(it->insn->opcode())) {
    if (invoke_insn) {
      *invoke_insn = it->insn;
    }
    ++it;
    always_assert(it != ii.end());
  }
  if (opcode::is_move_result_any(it->insn->opcode())) {
    ++it;
    always_assert(it != ii.end());
  }
  always_assert(it != ii.end());
  return it->insn == last_it->insn;
}

void never_inline(bool attach_annotations,
                  const Scope& scope,
                  const std::unordered_map<const DexMethodRef*,
                                           ArtProfileEntryFlags>& method_flags,
                  PassManager& mgr) {
  DexAnnotationSet anno_set;
  anno_set.add_annotation(std::make_unique<DexAnnotation>(
      type::dalvik_annotation_optimization_NeverInline(),
      DexAnnotationVisibility::DAV_BUILD));

  // Only "hot" methods get compiled.
  auto is_hot = [&](DexMethod* method) {
    auto it = method_flags.find(method);
    return it != method_flags.end() && it->second.hot;
  };

  auto consider_callee = [&](DexMethod* callee) {
    if (callee == nullptr || !callee->get_code()) {
      return false;
    }
    auto* cls = type_class(callee->get_class());
    if (!cls || cls->is_external()) {
      return false;
    }
    if (callee->is_virtual() && (!is_final(callee) && !is_final(cls))) {
      return false;
    }
    return true;
  };

  auto get_callee = [&](DexMethod* caller,
                        IRInstruction* invoke_insn) -> DexMethod* {
    DexMethod* callee;
    do {
      callee = resolve_invoke_method(invoke_insn, caller);
      if (!consider_callee(callee)) {
        return nullptr;
      }
      caller = callee;
      invoke_insn = nullptr;
    } while (is_simple(callee, &invoke_insn) && invoke_insn != nullptr);
    return callee;
  };

  // Analyze caller/callee relationships
  std::atomic<size_t> callers_too_large{0};
  InsertOnlyConcurrentSet<DexMethod*> hot_cold_callees;
  InsertOnlyConcurrentSet<DexMethod*> hot_hot_callees;
  InsertOnlyConcurrentMap<DexMethod*, size_t> estimated_code_units;
  walk::parallel::code(scope, [&](DexMethod* caller, IRCode& code) {
    auto ecu = code.estimate_code_units();
    estimated_code_units.emplace(caller, ecu);
    if (!is_hot(caller)) {
      return;
    }
    if (ecu > 2048) {
      // Way over the 1024 threshold of the AOT compiler, to be conservative.
      callers_too_large.fetch_add(1);
      return;
    }
    for (auto* b : code.cfg().blocks()) {
      for (auto& mie : InstructionIterable(b)) {
        if (!opcode::is_an_invoke(mie.insn->opcode())) {
          continue;
        }

        DexMethod* callee = get_callee(caller, mie.insn);
        if (!callee) {
          continue;
        }

        if (is_hot(callee)) {
          hot_hot_callees.insert(callee);
        } else {
          hot_cold_callees.insert(callee);
        }
      }
    }
  });
  mgr.incr_metric("never_inline_callers_too_large", callers_too_large.load());
  mgr.incr_metric("never_inline_hot_cold_callees", hot_cold_callees.size());
  mgr.incr_metric("never_inline_hot_hot_callees", hot_hot_callees.size());

  // Attach annotation to callees where beneficial.
  std::atomic<size_t> callees_already_never_inline = 0;
  std::atomic<size_t> callees_too_hot = 0;
  std::atomic<size_t> callees_simple = 0;
  std::atomic<size_t> callees_too_small = 0;
  std::atomic<size_t> callees_too_large = 0;
  std::atomic<size_t> callees_annotation_attached = 0;
  walk::code(scope, [&](DexMethod* method, IRCode& code) {
    if (has_anno(method, type::dalvik_annotation_optimization_NeverInline())) {
      callees_already_never_inline.fetch_add(1);
      return;
    }

    if (!hot_cold_callees.count_unsafe(method)) {
      return;
    }

    if (hot_hot_callees.count(method)) {
      callees_too_hot.fetch_add(1);
      return;
    }

    auto ecu = code.estimate_code_units();
    if (ecu > 32) {
      // Way over the 14 threshold of the AOT compiler, to be conservative.
      callees_too_large.fetch_add(1);
      return;
    }

    if (ecu <= 3) {
      callees_too_small.fetch_add(1);
      return;
    }

    if (is_simple(method)) {
      callees_simple.fetch_add(1);
      return;
    }

    callees_annotation_attached.fetch_add(1);
    if (!attach_annotations) {
      return;
    }
    if (method->get_anno_set()) {
      method->get_anno_set()->combine_with(anno_set);
      return;
    }
    auto access = method->get_access();
    // attach_annotation_set requires the method to be synthetic.
    // A bit bizarre, and suggests that Redex' code to mutate annotations is
    // ripe for an overhaul. But I won't fight that here.
    method->set_access(access | ACC_SYNTHETIC);
    method->attach_annotation_set(std::make_unique<DexAnnotationSet>(anno_set));
    method->set_access(access);
  });
  mgr.incr_metric("never_inline_callees_already_never_inline",
                  callees_already_never_inline.load());
  mgr.incr_metric("never_inline_callees_too_hot", callees_too_hot.load());
  mgr.incr_metric("never_inline_callees_simple", callees_simple.load());
  mgr.incr_metric("never_inline_callees_too_small", callees_too_small.load());
  mgr.incr_metric("never_inline_callees_too_large", callees_too_large.load());
  mgr.incr_metric("never_inline_callees_annotation_attached",
                  callees_annotation_attached.load());
}

} // namespace

std::ostream& operator<<(std::ostream& os, const ArtProfileEntryFlags& flags) {
  if (flags.hot) {
    os << "H";
  }
  if (flags.startup) {
    os << "S";
  }
  if (flags.not_startup) {
    os << "P";
  }
  return os;
}

void ArtProfileWriterPass::bind_config() {
  bind("perf_appear100_threshold", m_perf_config.appear100_threshold,
       m_perf_config.appear100_threshold);
  bind("perf_call_count_threshold", m_perf_config.call_count_threshold,
       m_perf_config.call_count_threshold);
  bind("perf_coldstart_appear100_threshold",
       m_perf_config.coldstart_appear100_threshold,
       m_perf_config.coldstart_appear100_threshold);
  bind("perf_coldstart_appear100_nonhot_threshold",
       m_perf_config.coldstart_appear100_threshold,
       m_perf_config.coldstart_appear100_nonhot_threshold);
  bind("perf_interactions", m_perf_config.interactions,
       m_perf_config.interactions);
  bind("never_inline_estimate", false, m_never_inline_estimate);
  bind("never_inline_attach_annotations", false,
       m_never_inline_attach_annotations);
  after_configuration([this] {
    always_assert(m_perf_config.coldstart_appear100_nonhot_threshold <=
                  m_perf_config.coldstart_appear100_threshold);
  });
}

void ArtProfileWriterPass::eval_pass(DexStoresVector& stores,
                                     ConfigFiles& conf,
                                     PassManager& mgr) {
  if (m_never_inline_attach_annotations) {
    m_reserved_refs_handle = mgr.reserve_refs(name(),
                                              ReserveRefsInfo(/* frefs */ 0,
                                                              /* trefs */ 1,
                                                              /* mrefs */ 0));
  }
}

void ArtProfileWriterPass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& conf,
                                    PassManager& mgr) {
  if (m_never_inline_attach_annotations) {
    always_assert(m_reserved_refs_handle);
    mgr.release_reserved_refs(*m_reserved_refs_handle);
    m_reserved_refs_handle = std::nullopt;
  }

  const auto& method_profiles = conf.get_method_profiles();
  std::unordered_map<const DexMethodRef*, ArtProfileEntryFlags> method_flags;
  for (auto& interaction_id : m_perf_config.interactions) {
    bool startup = interaction_id == "ColdStart";
    const auto& method_stats = method_profiles.method_stats(interaction_id);
    for (auto&& [method, stat] : method_stats) {
      // for startup interaction, we can include it into baseline profile
      // as non hot method if the method appear100 is above nonhot_threshold
      if (stat.appear_percent >=
              (startup ? m_perf_config.coldstart_appear100_nonhot_threshold
                       : m_perf_config.appear100_threshold) &&
          stat.call_count >= m_perf_config.call_count_threshold) {
        auto& mf = method_flags[method];
        mf.hot = startup ? stat.appear_percent >
                               m_perf_config.coldstart_appear100_threshold
                         : true;
        if (startup) {
          // consistent with buck python config in the post-process baseline
          // profile generator, which is set both flags true for ColdStart
          // methods
          mf.startup = true;
          // if startup method is not hot, we do not set its not_startup flag
          // the method still has a change to get it set if it appears in other
          // interactions' hot list. Remember, ART only uses this flag to guide
          // dexlayout decision, so we don't have to be pedantic to assume it
          // never gets exectued post startup
          mf.not_startup = mf.hot;
        } else {
          mf.not_startup = true;
        }
      }
    }
  }

  std::ofstream ofs{conf.metafile(BASELINE_PROFILES_FILE)};

  always_assert(!stores.empty());
  auto& dexen = stores.front().get_dexen();
  int32_t min_sdk = mgr.get_redex_options().min_sdk;
  mgr.incr_metric("min_sdk", min_sdk);
  auto end = min_sdk >= 21 ? dexen.size() : 1;
  InsertOnlyConcurrentSet<DexMethod*> methods_with_baseline_profile;
  for (size_t dex_idx = 0; dex_idx < end; dex_idx++) {
    auto& dex = dexen.at(dex_idx);
    for (auto* cls : dex) {
      bool should_include_class = false;
      for (auto* method : cls->get_all_methods()) {
        auto it = method_flags.find(method);
        if (it == method_flags.end()) {
          continue;
        }
        // hot method's class should be included.
        // In addition, if we include non-hot startup method, we also need to
        // include its class.
        if (it->second.hot || (it->second.startup && !it->second.not_startup)) {
          should_include_class = true;
        }
        std::string descriptor = show_deobfuscated(method);
        // reformat it into manual profile pattern so baseline profile generator
        // in post-process can recognize the method
        boost::replace_all(descriptor, ".", "->");
        boost::replace_all(descriptor, ":(", "(");
        ofs << it->second << descriptor << std::endl;
        methods_with_baseline_profile.insert(method);
      }
      if (should_include_class) {
        ofs << show_deobfuscated(cls) << std::endl;
      }
    }
  }

  auto scope = build_class_scope(stores);
  std::atomic<size_t> methods_with_baseline_profile_code_units{0};
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    if (methods_with_baseline_profile.count(method)) {
      methods_with_baseline_profile_code_units += code.estimate_code_units();
    }
  });

  mgr.incr_metric("methods_with_baseline_profile",
                  methods_with_baseline_profile.size());

  mgr.incr_metric("methods_with_baseline_profile_code_units",
                  (size_t)methods_with_baseline_profile_code_units);

  if (!m_never_inline_estimate && !m_never_inline_attach_annotations) {
    return;
  }

  never_inline(m_never_inline_attach_annotations, scope, method_flags, mgr);
}

static ArtProfileWriterPass s_pass;
