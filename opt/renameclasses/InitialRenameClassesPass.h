/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

/*
 * In some Redex opitmization passes (e.g. OrignialNamePass), we need to know if
 * a class is renamable before it is actually renamed. Therefore,
 * InitialRenameClassesPass is introduced to decided which classes will be
 * renamed before RenameClassesPass is executed. There could be new classes
 * generated by Redex after InitialRenameClassesPass and before
 * RenameClassesPass. For them, RenameClassesPassV2 will decide their renamble
 * status. Also, once a class' renamable status is decided at
 * InitialRenameClassesPass, it won't be changed after that.
 */
class InitialRenameClassesPass : public Pass {
 public:
  InitialRenameClassesPass() : Pass("InitialRenameClassesPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {HasSourceBlocks, Preserves},
    };
  }

  void bind_config() override { trait(Traits::Pass::unique, true); }

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;

  std::unordered_set<DexClass*> get_renamable_classes(Scope& scope,
                                                      ConfigFiles& conf,
                                                      PassManager& mgr);

 private:
  // Decide which classes should be actually renamed.
  void initial_rename_classes(Scope& scope, PassManager& mgr);
};
