/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <iosfwd>
#include <json/value.h>

#include "ApproximateShapeMerging.h"
#include "DexClass.h"
#include "MergerType.h"
#include "MergingStrategies.h"
#include "PassManager.h"
#include "Trace.h"
#include "TypeSystem.h"

struct ConfigFiles;
class PassManager;
class RefChecker;

using ConstTypeHashSet = std::unordered_set<const DexType*>;

namespace class_merging {

using TypeToTypeSet = std::unordered_map<const DexType*, TypeSet>;
using TypeGroupByDex = std::vector<std::pair<boost::optional<size_t>, TypeSet>>;

enum InterDexGroupingType {
  DISABLED = 0, // No interdex grouping.
  NON_HOT_SET = 1, // Exclude hot set.
  NON_ORDERED_SET = 2, // Exclude all ordered set.
  FULL = 3, // Apply interdex grouping on the entire input.
};

enum TypeTagConfig {
  // No type tags exist in the input hierarchy. No type tags need to be
  // generated by Redex.
  // We don't support operations that require the original type identity in this
  // option.
  NONE = 0,
  // No type tags in the input hierarchy. Redex generates the type tags and
  // fully handles the logic around type tags.
  GENERATE = 1,
  // The input hierarchy has type tags emitted. Redex handles the type tag value
  // passing for the merged ctors.
  INPUT_PASS_TYPE_TAG_TO_CTOR = 2,
  // The input hierarchy has type tags emitted. It also fully handles the type
  // tag logic including ctor value passing.
  INPUT_HANDLED = 3,
};

enum TypeLikeStringConfig {
  // Type like strings are safe to be replaced with the name of the new
  // shape class. The assumption is that the reflections against the type like
  // strings still work after merging. This usually means type tags exist in the
  // targeted input. Merging only changes class names not intiantiation pattern.
  REPLACE = 0,
  // Do not merge classes potentially reflected using the type like string. It's
  // more conservative. We do not have the full knowledge about the reflection
  // pattern. It's better to avoid merging altogether.
  EXCLUDE = 1,
};

/**
 * A class hierarchy specification to model for erasure.
 * This is normally specified via config entries:
 * // array of models
 * "models" : [
 *   {
 *     // this field is really not needed as we could remove the whole entry
 *     // but it's here for simplicity
 *     "enabled" : true,
 *     // this only makes sense when enabled is 'false' and it's intended
 *     // to perform the analysis without the optmization.
 *     // Look at the print comment in the .cpp file to see how to read the
 *     // analysis results
 *     "analysis" : true,
 *     // model name for printing/tracing/debugging purposes
 *     "name" : "Generated Code",
 *     // prefix to every generated class name for this model.
 *     // It's also used for metrics.
 *     // Makes it easy to see what is what
 *     "class_name_prefix" : "GenCode",
 *     // the generated model needs a type tag
 *     "needs_type_tag" : true;
 *     // the model has a type tag predefined and usable
 *     "has_type_tag" : true;
 *     "needs_type_tag" : true;
 *     // build MergerType only for groups that have more than min_count
 *     // classes, ignore others (default to 1)
 *     min_group_count: 100,
 *     // root to the model, the base type to identify all classes
 *     // that are candidate for erasure
 *     "root" : "Lcom/facebook/gencode/BaseType;",
 *     // exclude classes, can be classes or interfaces
 *     "exclude" : [
 *       "Lcom/facebook/gencode/ExcludedBase;"
 *     ],
 *     // a specification for the generated set that is treated specially
 *     // for reference analysis
 *     "generated" : {
 *       // Treat types under the same namespace specially.
 *       // Skip type exclusion check under the same namespace.
 *       // Assuming cross referencing under the same namespace are safe.
 *       "namespace" : true,
 *       // other roots from which identify types that have
 *       // to be treated specially
 *       "other_roots" : [
 *         "Lcom/facebook/gencode/OtherBase;"
 *       ]
 *     }
 *   },
 * ]
 */
struct ModelSpec {
  // whether the spec is to be used
  bool enabled{true};
  // name of the spec for debug/printing
  std::string name;
  // set of roots from which to find all model types
  TypeSet roots;
  // A set of types to be merged, they should be subtypes of the roots.
  ConstTypeHashSet merging_targets;
  // types to exclude from the model
  ConstTypeHashSet exclude_types;
  // prefixes of types to exclude from the model
  std::unordered_set<std::string> exclude_prefixes;
  // prefix for class generation
  std::string class_name_prefix;
  // type tag config
  TypeTagConfig type_tag_config{TypeTagConfig::GENERATE};
  // minimum nuber of mergeables to make it into a MergerType
  // (no optimization otherwise)
  size_t min_count{2};
  // set of generated types
  std::unordered_set<DexType*> gen_types;
  // set of annotations marking generated code
  std::unordered_set<DexType*> gen_annos;
  // set of types safe to consume the class obj of merged classes
  std::unordered_set<DexType*> const_class_safe_types;
  // The merging strategy of the model
  strategy::Strategy strategy{strategy::BY_CLASS_COUNT};
  // Group splitting. This is looser than the per dex split and takes into
  // account the interdex order (if any provided).
  InterDexGroupingType interdex_grouping{InterDexGroupingType::DISABLED};
  // whether to perform class merging on the primary dex.
  bool include_primary_dex{false};
  // Process @MethodMeta annotations
  bool process_method_meta{false};
  // Max mergeable count per merger type
  boost::optional<size_t> max_count{boost::none};
  // Approximate shaping
  Json::Value approximate_shape_merging;
  // Allows merging classes with non-primitive static fields. Enabling this will
  // change initialization order.
  bool merge_types_with_static_fields{false};
  // Preserve debug info like line numbers.
  bool keep_debug_info{false};
  // A flag for method deduplication. Deduplicating block that explicitly
  // capture stack traces for human-written code may make java stack trace
  // confusing.
  bool dedup_fill_in_stack_trace{true};
  // Replace type like string or exclude potentially referenced class.
  TypeLikeStringConfig type_like_string_confg{TypeLikeStringConfig::EXCLUDE};
  // Indicates if the merging should be performed per dex.
  bool per_dex_grouping{false};
  // The Model targets are generated code. If so, we consider merging_targets as
  // a part of the generated set.
  bool is_generated_code{false};

  enum class InterDexGroupingInferringMode {
    kAllTypeRefs,
    kClassLoads,
    kClassLoadsBasicBlockFiltering,
  };
  InterDexGroupingInferringMode interdex_grouping_inferring_mode{
      InterDexGroupingInferringMode::kAllTypeRefs};

  bool generate_type_tag() const {
    return type_tag_config == TypeTagConfig::GENERATE;
  }

  bool no_type_tag() const { return type_tag_config == TypeTagConfig::NONE; }

  bool has_type_tag() const { return type_tag_config != TypeTagConfig::NONE; }

  bool input_has_type_tag() const {
    return type_tag_config == TypeTagConfig::INPUT_PASS_TYPE_TAG_TO_CTOR ||
           type_tag_config == TypeTagConfig::INPUT_HANDLED;
  }

  bool pass_type_tag_to_ctor() const {
    return type_tag_config == TypeTagConfig::GENERATE ||
           type_tag_config == TypeTagConfig::INPUT_PASS_TYPE_TAG_TO_CTOR;
  }

  bool replace_type_like_strings() const {
    return type_like_string_confg == TypeLikeStringConfig::REPLACE;
  }

  bool exclude_type_like_strings() const {
    return type_like_string_confg == TypeLikeStringConfig::EXCLUDE;
  }

  boost::optional<size_t> max_num_dispatch_target{boost::none};
};

struct ModelStats {
  // Model level stats
  uint32_t m_all_types = 0;
  uint32_t m_non_mergeables = 0;
  uint32_t m_excluded = 0;
  uint32_t m_dropped = 0;
  // InterDex grouping stats
  std::map<InterdexSubgroupIdx, size_t> m_interdex_groups{};
  // Stats for approximate shape merging
  ApproximateStats m_approx_stats{};
  // Merging related stats
  uint32_t m_num_classes_merged = 0;
  uint32_t m_num_generated_classes = 0;
  uint32_t m_num_ctor_dedupped = 0;
  uint32_t m_num_static_non_virt_dedupped = 0;
  uint32_t m_num_vmethods_dedupped = 0;
  uint32_t m_num_const_lifted_methods = 0;

  ModelStats& operator+=(const ModelStats& stats);

  void update_redex_stats(const std::string& prefix, PassManager& mgr) const;
};

/**
 * A Model is a revised hierarchy for the class set under analysis.
 * The purpose is to define a small number of types that can be used to
 * merge a set of other types. The mergeables types will be erased.
 * The model takes into account interfaces and shapes of the types
 * to merge in order to define proper aggregation.
 * The Model retains all the class hierarchy and mergeable type information
 * that can be use to generated proper code.
 * Manipulation of the Model is done via calls to the Model public API.
 */
class Model {
 public:
  /**
   * Build a Model given a scope and a specification.
   */
  static Model build_model(const Scope& scope,
                           const DexStoresVector& stores,
                           const ConfigFiles& conf,
                           const ModelSpec& spec,
                           const TypeSystem& type_system,
                           const RefChecker& refchecker);

  const std::string& get_name() const { return m_spec.name; }
  std::vector<const DexType*> get_roots() const {
    std::vector<const DexType*> res;
    for (const auto root_merger : m_roots) {
      res.push_back(root_merger->type);
    }
    return res;
  }

  template <class HierarchyWalkerFn = void(const MergerType&)>
  void walk_hierarchy(HierarchyWalkerFn walker) {
    for (const auto root_merger : m_roots) {
      if (!root_merger->dummy) {
        walker(*root_merger);
      }
      walk_hierarchy_helper(walker, root_merger->type);
    }
  }

  const DexType* get_parent(const DexType* child) const {
    auto it = m_parents.find(child);
    if (it == m_parents.end()) {
      return nullptr;
    }
    return it->second;
  }

  const TypeSet& get_interfaces(const DexType* type) const {
    const auto& intfs = m_class_to_intfs.find(type);
    return intfs != m_class_to_intfs.end() ? intfs->second : empty_set;
  }

  const std::string& get_class_name_prefix() const {
    return m_spec.class_name_prefix;
  }

  bool is_interdex_grouping_enabled() const {
    return m_spec.interdex_grouping != InterDexGroupingType::DISABLED;
  }

  const ModelSpec& get_model_spec() const { return m_spec; }

  const ModelStats& get_model_stats() const { return m_stats; }

  bool process_method_meta() const { return m_spec.process_method_meta; }
  bool keep_debug_info() const { return m_spec.keep_debug_info; }

  void update_redex_stats(PassManager& mgr) const;

  static void build_interdex_groups(ConfigFiles& conf);

  /**
   * Print everything about the model.
   * The printing has a format to allow grep to isolate specific parts.
   * The format is the following:
   * + TypeName type_info
   * - ErasedTypeName type_info
   * -* MergedType fields
   * -# MergedType methods
   * type_info gives info on children, interfaces and method count.
   * '+' can be used to look at hierarchies of types
   * (i.e. grep -e "^+* L.*;")
   * + Base children(k), interfaces(n), Intf1, Intf2
   * ++ Derived1
   * +++ Derived11
   * ++ Derived2
   * +++ Derived21
   * adding '-' would give the hierarchy and the merged/erasable types
   * (i.e. grep -e "^+* L.*;\|^-* L.*;")
   * + Base
   * ++ Derived1
   * +++ Derived11
   * ++ Shape
   * -- Erasable1
   * -- Erasable2
   * -- Erasable3
   * you can view the hierarchy with the merged types and the fields
   * and methods in the merger
   * (i.e. grep -e "^+* L.*;\|^-.* L.*;")
   * + Base
   * ++ Derived1
   * +++ Derived11
   * ++ Shape
   * -- Erasable1
   * --* field
   * --# method
   */
  std::string print() const;

  const TypeSystem& get_type_system() const { return m_type_system; }

 private:
  static const TypeSet empty_set;

  // the spec for this model
  ModelSpec m_spec;
  // stats collection of this model
  ModelStats m_stats;
  // the roots (base types) for the model
  std::vector<MergerType*> m_roots;
  // the new generated class hierarchy during analysis.
  // Types are not changed during analysis and m_hierarchy represents
  // the class hierarchy as known to the analysis and what the final
  // hierarchy will be
  ClassHierarchy m_hierarchy;
  // child to parent relationship of types in the model.
  // Because nothing is changed during analysis DexClass::get_super_class()
  // may not have the correct relationship
  std::unordered_map<const DexType*, const DexType*> m_parents;
  // class to interfaces map as known to the analysis
  TypeToTypeSet m_class_to_intfs;
  // interface to class relationship as known to the analysis
  TypeToTypeSet m_intf_to_classes;
  // type to merger map
  std::unordered_map<const DexType*, MergerType> m_mergers;
  // Types excluded by the ModelSpec.exclude_types
  TypeSet m_excluded;
  // The set of non mergeables types. Those are types that are not
  // erasable for whatever reason
  TypeSet m_non_mergeables;

  const TypeSystem& m_type_system;
  const RefChecker& m_ref_checker;

  // Number of merger types created with the same shape per model.
  std::map<MergerType::Shape, size_t, MergerType::ShapeComp> m_shape_to_count;

  const Scope& m_scope;
  const ConfigFiles& m_conf;
  const XDexRefs m_x_dex;

  static std::unordered_map<DexType*, size_t> s_cls_to_interdex_group;
  static size_t s_num_interdex_groups;

  /**
   * Build a Model given a set of roots and a set of types deriving from the
   * roots.
   */
  Model(const Scope& scope,
        const DexStoresVector& stores,
        const ConfigFiles& conf,
        const ModelSpec& spec,
        const TypeSystem& type_system,
        const RefChecker& refchecker);

  void init(const Scope& scope,
            const ModelSpec& spec,
            const TypeSystem& type_system);

  void build_hierarchy(const TypeSet& roots);
  void build_interface_map(const DexType* type, TypeSet implemented);
  MergerType* build_mergers(const DexType* root);
  void exclude_types(const ConstTypeHashSet& exclude_types);
  bool is_excluded(const DexType* type) const;

  // MergerType creator helpers
  MergerType& create_dummy_merger(const DexType* type);
  void create_dummy_mergers_if_children(const DexType* type);
  MergerType& create_merger_shape(const DexType* shape_type,
                                  const MergerType::Shape& shape,
                                  const DexType* parent,
                                  const TypeSet& intfs,
                                  const std::vector<const DexType*>& classes);
  MergerType& create_merger_helper(
      const DexType* merger_type,
      const MergerType::Shape& shape,
      const TypeSet& intf_set,
      const boost::optional<size_t>& dex_id,
      const ConstTypeVector& group_values,
      const boost::optional<InterdexSubgroupIdx>& interdex_subgroup_idx,
      const InterdexSubgroupIdx subgroup_idx);
  void create_mergers_helper(
      const DexType* merger_type,
      const MergerType::Shape& shape,
      const TypeSet& intf_set,
      const boost::optional<size_t>& dex_id,
      const TypeSet& group_values,
      const strategy::Strategy strategy,
      const boost::optional<InterdexSubgroupIdx>& interdex_subgroup_idx,
      const boost::optional<size_t>& max_mergeables_count,
      size_t min_mergeables_count);

  // make shapes out of the model classes
  void shape_model();
  void shape_merger(const MergerType& root, MergerType::ShapeCollector& shapes);
  void approximate_shapes(MergerType::ShapeCollector& shapes);
  void break_by_interface(const MergerType& merger,
                          const MergerType::Shape& shape,
                          MergerType::ShapeHierarchy& hier);
  void flatten_shapes(const MergerType& merger,
                      MergerType::ShapeCollector& shapes);
  TypeGroupByDex group_per_dex(bool per_dex_grouping, const TypeSet& types);
  TypeSet get_types_in_current_interdex_group(
      const TypeSet& types, const ConstTypeHashSet& interdex_group_types);

  std::vector<ConstTypeHashSet> group_by_interdex_set(
      const ConstTypeHashSet& types);
  void map_fields(MergerType& merger,
                  const std::vector<const DexType*>& classes);

  // collect and distribute methods across MergerTypes
  void collect_methods();
  void add_virtual_scope(MergerType& merger, const VirtualScope& virt_scope);
  void add_interface_scope(MergerType& merger, const VirtualScope& intf_scope);
  void distribute_virtual_methods(const DexType* type,
                                  std::vector<const VirtualScope*> base_scopes);

  // Model internal type system helpers
  void set_parent_child(const DexType* parent, const DexType* child) {
    m_hierarchy[parent].insert(child);
    m_parents[child] = parent;
  }

  void remove_child(const DexType* child) {
    const auto& prev_parent_hier = m_hierarchy.find(m_parents[child]);
    always_assert(prev_parent_hier != m_hierarchy.end());
    auto erased = prev_parent_hier->second.erase(child);
    always_assert(erased > 0);
    if (prev_parent_hier->second.empty()) {
      m_hierarchy.erase(prev_parent_hier);
    }
  }

  void move_child_to_mergeables(MergerType& merger, const DexType* child) {
    TRACE(CLMG, 3, "Adding child %s to merger %s", show_type(child).c_str(),
          print(merger).c_str());
    remove_child(child);
    merger.mergeables.insert(child);
  }

  static std::string show_type(const DexType* type); // To avoid "Show.h" in the
                                                     // header.

  // printers
  std::string print(const MergerType& merger) const;
  std::string print(const DexType* type) const;
  std::string print(const DexType* type, int nest) const;

  // walker helper
  template <class HierarchyWalkerFn = void(const MergerType&)>
  void walk_hierarchy_helper(HierarchyWalkerFn walker, const DexType* type) {
    const auto& children = m_hierarchy.find(type);
    if (children == m_hierarchy.end()) return;
    for (const auto* child : children->second) {
      const auto& merger_it = m_mergers.find(child);
      if (merger_it != m_mergers.end()) {
        const auto& merger = merger_it->second;
        if (!merger.dummy) {
          walker(merger);
        }
      }
      walk_hierarchy_helper(walker, child);
    }
  }
};

InterDexGroupingType get_merge_per_interdex_type(
    const std::string& interdex_grouping);

std::ostream& operator<<(std::ostream& os,
                         ModelSpec::InterDexGroupingInferringMode mode);

} // namespace class_merging
