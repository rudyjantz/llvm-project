/*
 * Copyright (C) 2015 David Devecsery
 */

#include "llvm/Oha/lib/BasicFcnCFG.h"

#include <string>

#include "llvm/Oha/lib/CallDests.h"
#include "llvm/Oha/Tarjans.h"
#include "llvm/Oha/LLVMHelper.h"

// Add Function CFG stuff here
// Get callsite info...
// Etc...

// From cg.cpp... ugh hacky
extern llvm::cl::opt<bool> no_spec;

BasicFcnCFG::BasicFcnCFG(llvm::Module &m, DynamicInfo &di) {
  auto &dyn_info = di.used_info;
  auto &indir_info = di.indir_info;
  // Populate our SEG to contain all of the functions as nodes
  // Step one, add a node for each function
  for (auto &fcn : m) {
    if (!dyn_info.isUsed(fcn) && !no_spec) {
      continue;
    }
    auto node_id = fcnGraph_.addNode<FcnNode>(&fcn);
    // llvm::dbgs() << "Adding fcn: " << fcn.getName() << ": " << &fcn << "\n";
    fcnMap_.emplace(&fcn, node_id);
  }

  // Add edges for all of the callsites
  for (auto &fcn : m) {
    if (!dyn_info.isUsed(fcn) && !no_spec) {
      continue;
    }

    // llvm::dbgs() << "Getting fcn: " << fcn.getName() << ": " << &fcn << "\n";
    auto fcn_id = fcnMap_.at(&fcn);
    for (auto &bb : fcn) {
      if (!dyn_info.isUsed(bb) && !no_spec) {
        continue;
      }

      for (auto &inst : bb) {
        auto pinst = &inst;

        if (auto ci = dyn_cast<llvm::CallInst>(pinst)) {
          llvm::ImmutableCallSite cs(ci);

          auto pdest_fcn = LLVMHelper::getFcnFromCall(cs);

          // Only consider direct calls...
          if (pdest_fcn != nullptr) {
            // assert(dyn_info.isUsed(pdest_fcn));
            // llvm::dbgs() << "    ci is: " << ValPrinter(ci) << "\n";
            // llvm::dbgs() << "    fcn is: " << ValPrinter(pdest_fcn) << "\n";
            // Its possible we didn't reach this point -- although we reached
            // points before it, because of interruptions in control flow --
            // e.g. signals.
            auto it = fcnMap_.find(pdest_fcn);
            if (it != std::end(fcnMap_)) {
              auto dest_id = it->second;
              fcnGraph_.addPred(dest_id, fcn_id);
            }
          // Unless we have indir info
          } else {
            if (indir_info.hasInfo()) {
              // Don't bother w/ inline asm...
              if (!llvm::isa<llvm::InlineAsm>(cs.getCalledValue())) {
                auto &dests = indir_info.getTargets(ci);

                for (auto dest : dests) {
                  auto dest_id = fcnMap_.at(cast<llvm::Function>(dest));
                  fcnGraph_.addPred(dest_id, fcn_id);
                }
              }
            }
          }
        } else if (auto ii = dyn_cast<llvm::InvokeInst>(pinst)) {
          llvm::dbgs() << "Unexpected invoke inst: " << *ii << "\n";
          llvm_unreachable("I don't support invokes!");
        }
      }
    }
  }

  // Calculate SCC
  {
    RunTarjans<> X(fcnGraph_);
  }
}

