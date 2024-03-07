/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "DexClass.h"
#include "IROpcode.h"
#include "WellKnownTypes.h"

namespace method {
/**
 * True if the method is a constructor (matches the "<init>" name)
 */
bool is_init(const DexMethodRef* method);

/**
 * True if the method is a static constructor (matches the "<clinit>" name)
 */
bool is_clinit(const DexMethodRef* method);

/**
 * True if the method is a constructor without arguments
 */
bool is_argless_init(const DexMethodRef* method);

/**
 * Whether the method is a ctor or static ctor.
 */
inline bool is_any_init(const DexMethodRef* method) {
  return is_init(method) || is_clinit(method);
}

/**
 * Return true if the clinit is Trivial.
 * A trivial clinit should only contain a return-void instruction.
 */
bool is_trivial_clinit(const IRCode& code);

bool is_clinit_invoked_method_benign(const DexMethodRef*);

/**
 * Checker whether the method has code that starts with an unreachable
 * instruction, indicating that earlier static analysis determined that this
 * particular method is never a possible target of an invocation.
 */
bool may_be_invoke_target(const DexMethod* method);

using ClInitHasNoSideEffectsPredicate = std::function<bool(const DexType*)>;
/**
 * Return true if change the exeution time of the <clinit> of the cls may change
 * the program behavior.
 *
 * TODO: We can assume no side effect for more cases, like if it only accesses
 * other classes whose <clinit> also has no side effect.
 *
 * Returns the first type along the chain of super types whose clinit actually
 * may have side effects.
 */
const DexClass* clinit_may_have_side_effects(
    const DexClass* cls,
    bool allow_benign_method_invocations,
    const ClInitHasNoSideEffectsPredicate* clinit_has_no_side_effects = nullptr,
    const std::unordered_set<DexMethod*>* non_true_virtuals = nullptr);

/**
 * Check that the method contains no invoke-super instruction; this is a
 * requirement to relocate a method outside of its original inheritance
 * hierarchy.
 */
bool no_invoke_super(const IRCode& code);

/**
 * Determine if the method is a constructor.
 *
 * Notes:
 * - Does NOT distinguish between <init> and <clinit>, will return true
 *   for static class initializers
 */

inline bool is_constructor(const DexMethod* meth) {
  return meth->get_access() & ACC_CONSTRUCTOR;
}

inline bool is_constructor(const DexMethodRef* meth) {
  return meth->is_def() &&
         method::is_constructor(static_cast<const DexMethod*>(meth));
}

/** Determine if the method takes no arguments. */
inline bool has_no_args(const DexMethodRef* meth) {
  return meth->get_proto()->get_args()->empty();
}

/** Determine if the method takes exactly n arguments. */
inline bool has_n_args(const DexMethodRef* meth, size_t n) {
  return meth->get_proto()->get_args()->size() == n;
}

/**
 * Determine if the method has code.
 *
 * Notes:
 * - Native methods are not considered to "have code"
 */
inline bool has_code(const DexMethodRef* meth) {
  return meth->is_def() &&
         static_cast<const DexMethod*>(meth)->get_code() != nullptr;
}

/**
 * Return true if method signatures (name and proto) match.
 */
inline bool signatures_match(const DexMethodRef* a, const DexMethodRef* b) {
  return a->get_name() == b->get_name() && a->get_proto() == b->get_proto();
}

#define DECLARE_METHOD(name, _) DexMethod* name();

#define FOR_EACH DECLARE_METHOD
WELL_KNOWN_METHODS
#undef FOR_EACH
#undef DECLARE_METHOD

DexMethod* kotlin_jvm_internal_Intrinsics_checkParameterIsNotNull();

DexMethod* kotlin_jvm_internal_Intrinsics_checkNotNullParameter();

DexMethod* kotlin_jvm_internal_Intrinsics_checExpressionValueIsNotNull();

DexMethod* kotlin_jvm_internal_Intrinsics_checkNotNullExpressionValue();

DexMethod* redex_internal_checkObjectNotNull();

inline unsigned count_opcode_of_types(
    const cfg::ControlFlowGraph& cfg,
    const std::unordered_set<IROpcode>& opcodes) {
  unsigned ret = 0;
  for (auto&& mie : cfg::ConstInstructionIterable(cfg)) {
    auto op = mie.insn->opcode();
    if (opcodes.count(op)) {
      ret++;
    }
  }
  return ret;
}

template <typename IRCodeContainer>
inline unsigned count_opcode_of_types(
    const IRCodeContainer& code, const std::unordered_set<IROpcode>& opcodes) {
  unsigned ret = 0;
  for (auto&& mie : ::InstructionIterable(code)) {
    auto op = mie.insn->opcode();
    if (opcodes.count(op)) {
      ret++;
    }
  }
  return ret;
}

}; // namespace method
