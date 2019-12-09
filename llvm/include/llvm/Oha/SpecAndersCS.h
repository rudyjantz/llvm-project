/*
 * Copyright (C) 2015 David Devecsery
 */

#ifndef INCLUDE_SPECANDERSCS_H_
#define INCLUDE_SPECANDERSCS_H_

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "llvm/Oha/AndersGraph.h"
#include "llvm/Oha/Assumptions.h"
#include "llvm/Oha/ConstraintPass.h"
#include "llvm/Oha/lib/UnusedFunctions.h"
#include "llvm/Oha/lib/IndirFcnTarget.h"

#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"



// The actual SFS module, most of the work is done via the ObjectMap and Def-Use
// Graph (DUG), these methods mostly operate on them.

/*
 *
 * SpecAndersCSResult
 *
 */

class SpecAndersCSMixin;

using AliasResult = llvm::AliasResult;
using MemoryLocation = llvm::MemoryLocation;

class SpecAndersCSResult : public llvm::AAResultBase<SpecAndersCSResult> {

  friend AAResultBase<SpecAndersCSResult>;

 public:

  explicit SpecAndersCSResult(SpecAndersCSMixin &anders_mixin);
  SpecAndersCSResult(SpecAndersCSResult &&RHS);
  ~SpecAndersCSResult();

  virtual llvm::AliasResult alias(const llvm::MemoryLocation &L1,
      const llvm::MemoryLocation &L2);

  virtual llvm::ModRefInfo getModRefInfo(llvm::ImmutableCallSite CS,
                             const llvm::MemoryLocation &Loc);
  virtual llvm::ModRefInfo getModRefInfo(llvm::ImmutableCallSite CS1,
                                     llvm::ImmutableCallSite CS2);
  // Do not use it.
  bool pointsToConstantMemory(const llvm::MemoryLocation &Loc,
      bool OrLocal = false);


  /// Handle invalidation events from the new pass manager.
  /// By definition, this result is stateless and so remains valid.
  bool invalidate(llvm::Function &, const llvm::PreservedAnalyses &,
                  llvm::FunctionAnalysisManager::Invalidator &) {
    return false;
  }

 private:
  SpecAndersCSMixin &anders_mixin_;
};










class SpecAndersCSMixin : public llvm::AnalysisInfoMixin<SpecAndersCSMixin> {

  friend AnalysisInfoMixin<SpecAndersCSMixin>;
  static llvm::AnalysisKey Key;

public:
  using Result = SpecAndersCSResult;
  SpecAndersCSResult run(llvm::Function &F, llvm::FunctionAnalysisManager &FM);
};







/*
 *
 * SpecAndersCS
 *
 */

class SpecAndersCS : public llvm::ModulePass,
                     public llvm::AAResultBase<SpecAndersCS> {

public:
  static char ID;
  SpecAndersCS();
  explicit SpecAndersCS(char &id);

  using Result = SpecAndersCSResult;

  //SpecAndersCSResult run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
  SpecAndersCSResult run(llvm::Function &F, llvm::FunctionAnalysisManager &FM);

  virtual bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(llvm::AnalysisUsage &usage) const;

  virtual llvm::AliasResult alias(const llvm::MemoryLocation &L1,
                                  const llvm::MemoryLocation &L2);

  virtual llvm::ModRefInfo getModRefInfo(llvm::ImmutableCallSite CS,
                             const llvm::MemoryLocation &Loc);
  virtual llvm::ModRefInfo getModRefInfo(llvm::ImmutableCallSite CS1,
                                     llvm::ImmutableCallSite CS2);
  // Do not use it.
  bool pointsToConstantMemory(const llvm::MemoryLocation &Loc,
      bool OrLocal = false);

  /*
   * FIXME ? old stuff that might belong here still b/c of dependencies
   */
  const AssumptionSet &getSpecAssumptions() const {
    return specAssumptions_;
  }

  const std::set<std::vector<CsCFG::Id>> getInvalidStacks() const {
    return mainCg_->invalidStacks();
  }

  const CsCFG &getCsCFG() const {
    return mainCg_->csCFG();
  }

  ValueMap &getValueMap() {
    return mainCg_->vals();
  }

  ValueMap::Id getRep(ValueMap::Id id) {
    return mainCg_->vals().getRep(id);
  }

  const PtstoSet &getPointsTo(ValueMap::Id id) {
    // Convert input objID to rep ObjID:
    auto rep_id = getRep(id);
    return graph_.getNode(rep_id).ptsto();
  }

  ConstraintPass &getConstraintPass() {
    return *consPass_;
  }


 private:
  // Solves the remaining graph, providing full flow-sensitive inclusion-based
  // points-to analysis
  bool solve();

  void addIndirCall(const PtstoSet &fcn_pts,
      const CallInfo &caller_ci,
      CsFcnCFG::Id cur_graph_node,
      Worklist<AndersGraph::Id> &wl,
      std::vector<uint32_t> &priority);
  void addIndirEdges(const CallInfo &caller_ci,
      const CallInfo &callee_ci,
      Worklist<AndersGraph::Id> &wl,
      const std::vector<uint32_t> &priority);
  void handleGraphChange(size_t old_size,
      Worklist<AndersGraph::Id> &wl,
      std::vector<uint32_t> &priority);
  PtstoSet *ptsCacheGet(const llvm::Value *val);

 protected:
  AndersGraph graph_;

  std::unique_ptr<DynamicInfo> dynInfo_;
  std::unique_ptr<CgCache> cgCache_;
  std::unique_ptr<CgCache> callCgCache_;
  Cg *mainCg_;
  AssumptionSet specAssumptions_;

  ConstraintPass *consPass_;

  std::map<ValueMap::Id, ValueMap::Id> hcdPairs_;

  std::unordered_map<const llvm::Value *, PtstoSet> ptsCache_;

};










/*
 *
 * SpecAndersCSWrapperPass
 * Legacy wrapper pass to provide the SpecAndersCSResult object.
 *
 */


//class SpecAndersCSWrapperPass : public llvm::ModulePass {
//class SpecAndersCSWrapperPass : public llvm::FunctionPass {
class SpecAndersCSWrapperPass : public llvm::ImmutablePass {
  std::unique_ptr<SpecAndersCSResult> Result;

public:
  static char ID;

  SpecAndersCSWrapperPass();

  SpecAndersCSResult &getResult() { return *Result; }
  const SpecAndersCSResult &getResult() const { return *Result; }

  //bool runOnModule(llvm::Module &M) override;
  //bool runOnFunction(llvm::Function &F) override;
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;

  //void initializePass();
  bool doInitialization(llvm::Module &M) override;
  bool doFinalization(llvm::Module &M) override;

};

llvm::ImmutablePass *createSpecAndersCSWrapperPass();
//llvm::ModulePass *createSpecAndersCSWrapperPass();
//llvm::FunctionPass *createSpecAndersCSWrapperPass();


#endif  // INCLUDE_SPECANDERSCS_H_
