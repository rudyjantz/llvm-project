/*
 * Copyright (C) 2015 David Devecsery
 */

// Enable debugging prints for this file
// #define SPECSFS_DEBUG
// #define SPECSFS_LOGDEBUG
// #define SEPCSFS_PRINT_RESULTS

#include "llvm/Oha/SpecAnders.h"

#include <gperftools/profiler.h>

#include <execinfo.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "llvm/Oha/util.h"

#include "llvm/Pass.h"
#include "llvm/PassSupport.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/AliasAnalysis.h"
// #include "llvm/Analysis/ProfileSummaryInfo.h"

#include "llvm/Oha/Cg.h"
#include "llvm/Oha/ConstraintPass.h"
#include "llvm/Oha/Debug.h"
#include "llvm/Oha/ValueMap.h"
#include "llvm/Oha/lib/UnusedFunctions.h"
#include "llvm/Oha/lib/IndirFcnTarget.h"
// #include "llvm/Oha/lib/DynPtsto.h"

using std::swap;

// Error handling functions {{{
// Don't warn about this (if it is an) unused function... I'm being sloppy
[[ gnu::unused ]]
static void print_stack_trace(void) {
  void *array[10];
  size_t size;
  char **strings;
  size_t i;

  size = backtrace(array, 10);
  strings = backtrace_symbols(array, size);

  llvm::errs() << "BACKTRACE:\n";
  for (i = 0; i < size; i++) {
    llvm::errs() << "\t" << strings[i] << "\n";
  }

  free(strings);
}

static void error(const std::string &msg) {
  llvm::errs() << "ERROR: " << msg << "\n";
  print_stack_trace();
  assert(0);
}
//}}}

static llvm::cl::opt<bool>
  do_spec_diff("anders-do-spec-diff", llvm::cl::init(false),
      llvm::cl::value_desc("bool"),
      llvm::cl::desc("if set specanders will print the ptstos counts which "
        "would have been identified by a speculative sfs run (for reporting "
        "improvment numbers)"));

static llvm::cl::list<std::string>
  fcn_names("anders-debug-fcn",
      llvm::cl::desc("if set anders will print the ptsto set for this function"
        " at the end of execution"));

static llvm::cl::opt<std::string>
  glbl_name("anders-debug-glbl", llvm::cl::init(""),
      llvm::cl::value_desc("string"),
      llvm::cl::desc("if set anders will print the ptsto set for this global"
        " at the end of execution"));

static llvm::cl::list<int32_t> //  NOLINT
  id_debug("anders-debug-id",
      llvm::cl::desc("Specifies IDs to print the nodes of after andersens "
        "runs"));

static llvm::cl::list<int32_t> //  NOLINT
  id_print("anders-print-id",
      llvm::cl::desc("Specifies IDs to print the nodes of before andersens "
        "runs"));

static llvm::cl::opt<bool>
  anders_no_opt("anders-no-opt", llvm::cl::init(false),
      llvm::cl::value_desc("bool"),
      llvm::cl::desc(
        "Disables all optimization passes Andersens analysis uses"));


static llvm::cl::opt<bool>
  do_anders_print_result("anders-do-print-result", llvm::cl::init(false),
      llvm::cl::value_desc("bool"),
      llvm::cl::desc(
        "if set specanders will print the ptsto sets for each value"));

static llvm::cl::opt<bool>
  do_spec_dyn_debug("anders-do-check-dyn", llvm::cl::init(false),
      llvm::cl::value_desc("bool"),
      llvm::cl::desc(
        "Verifies the calculated points-to set is a superset of the dynamic "
        "points-to to"));


// AA Result
llvm::AliasResult SpecAndersAAResult::alias(const llvm::MemoryLocation &L1,
                                            const llvm::MemoryLocation &L2) {
  auto v1 = L1.Ptr;
  auto v2 = L2.Ptr;

  // If either of the pointers are a constant pointer-to-int return false
  auto check_const = [](const llvm::Value *v) {
    if (auto ce = dyn_cast<llvm::ConstantExpr>(v)) {
      // if c is a constexpr
      if (ce->getOpcode() == llvm::Instruction::IntToPtr) {
        return true;
      }
    }
    return false;
  };

  if (check_const(v1)) {
    return llvm::AliasResult::NoAlias;
  }

  if (check_const(v2)) {
    return llvm::AliasResult::NoAlias;
  }

  auto pv1_pts = anders_.ptsCacheGet(v1);
  auto pv2_pts = anders_.ptsCacheGet(v2);

  if (pv1_pts == nullptr) {
    /*
    llvm::dbgs() << "Anders couldn't find node: " << obj_id1 <<
      << " " << FullValPrint(obj_id1, omap_) << "\n";
    */
    return llvm::AAResultBase<SpecAndersAAResult>::alias(L1, L2);
  }

  if (pv2_pts == nullptr) {
    /*
    llvm::dbgs() << "Anders couldn't find node: " << obj_id2 <<
      << " " << FullValPrint(obj_id2, omap_) << "\n";
    */
    return llvm::AAResultBase<SpecAndersAAResult>::alias(L1, L2);
  }

  auto &pts1 = *pv1_pts;
  auto &pts2 = *pv2_pts;

  // llvm::dbgs() << "Anders Alias Check\n";

  // If either of the sets point to nothing, no alias
  if (pts1.empty() || pts2.empty()) {
    return llvm::AliasResult::NoAlias;
  }

  // Check to see if the two pointers are known to not alias.  They don't alias
  // if their points-to sets do not intersect.
  if (!pts1.intersectsIgnoring(pts2,
        ValueMap::NullValue)) {
    return llvm::AliasResult::NoAlias;
  }

  return llvm::AAResultBase<SpecAndersAAResult>::alias(L1, L2);
}
// END AA Result


// LEGACY WRAPPER

//namespace llvm {
//  //void initializeSpecAndersWrapperPassPass(PassRegistry &);
//  static RegisterPass<SpecAndersWrapperPass>
//      SpecAndersRP("SpecAnders", "Speculative Andersens Analysis", false, true);
//  // RegisterAnalysisGroup<AliasAnalysis> SpecAndersRAG(SpecAndersRP);
//}  // namespace llvm

char SpecAndersWrapperPass::ID = 0;
SpecAndersWrapperPass::SpecAndersWrapperPass() : llvm::ModulePass(ID) {
  initializeSpecAndersWrapperPassPass(*llvm::PassRegistry::getPassRegistry());
}

SpecAndersWrapperPass::SpecAndersWrapperPass(char &id) : llvm::ModulePass(id) {
  initializeSpecAndersWrapperPassPass(*llvm::PassRegistry::getPassRegistry());
}

bool SpecAndersWrapperPass::runOnModule(llvm::Module &m) {
  auto &cp = getAnalysis<ConstraintPass>();
  auto &uf = getAnalysis<UnusedFunctions>();
  auto &indir_info = getAnalysis<IndirFunctionInfo>();
  auto &call_info = getAnalysis<CallContextLoader>();
  anders_.run(m, cp, uf, indir_info, call_info);
  result_.reset(new SpecAndersAAResult(anders_));
  return false;
}

void SpecAndersWrapperPass::getAnalysisUsage(
    llvm::AnalysisUsage &usage) const {
  // Because we're an AliasAnalysis
  llvm::dbgs() << "In getAnalysisUsage!\n";
  // usage.addRequired<llvm::AAResultsWrapperPass>();
  usage.setPreservesAll();

  // Calculates the constraints for this module for both sfs and andersens
  usage.addRequired<ConstraintPass>();

  // Staging analysis for sfs
  // usage.addRequired<SpecAnders>();

  // For DCE
  usage.addRequired<UnusedFunctions>();
  // For indirect function following
  usage.addRequired<IndirFunctionInfo>();

  usage.addRequired<CallContextLoader>();
  // For dynamic ptsto removal
  // usage.addRequired<DynPtstoLoader>();
  // usage.addRequired<llvm::ProfileSummaryInfo>();
}

// Crappy requirement for crappy macros
using namespace llvm;  // NOLINT
//INITIALIZE_PASS_BEGIN(SpecAndersWrapperPass, "SpecAnders",
//    "Speculative Andersens Analysis", false, false)
//INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
//INITIALIZE_PASS_END(SpecAndersWrapperPass, "SpecAnders",
//    "Speculative Andersens Analysis", false, false)
INITIALIZE_PASS(SpecAndersWrapperPass, "SpecAnders",
    "Speculative Andersens Analysis", false, true)

// END LEGACY WRAPPER PASS


// Constructor
void SpecAndersAnalysis::run(llvm::Module &m,
    ConstraintPass &cons_pass,
    UnusedFunctions &unused_fcns,
    IndirFunctionInfo &indir_fcns,
    CallContextLoader &call_info) {
  // Set up our alias analysis
  // -- This is required for the llvm AliasAnalysis interface
  // FIXME(ddevec): fix for new alias analysis interface
  // InitializeAliasAnalysis(this);

  if (glbl_name != "") {
    llvm::dbgs() << "Got debug gv: " << glbl_name << "\n";
  }

  // Setup dynamic info
  dynInfo_ = std14::make_unique<DynamicInfo>(unused_fcns,
      indir_fcns, call_info);

  // Clear the def-use graph
  // It should already be cleared, but I'm paranoid
  cp_ = &cons_pass;
  // Our main cg is the one inhereted from cons_pass
  mainCg_ = std14::make_unique<Cg>(cons_pass.getCG());

  BasicFcnCFG fcn_cfg(m, *dynInfo_);

  // Finish off any indirect edges?
  cgCache_ = std14::make_unique<CgCache>(cons_pass.cgCache());
  callCgCache_ = std14::make_unique<CgCache>(cons_pass.callCgCache());

  mainCg_->constraintStats();

  mainCg_->lowerAllocs();
  BddPtstoSet::PtstoSetInit(*mainCg_);

  // ProfilerStart("anders_opt.prof");
  if (!anders_no_opt) {
    util::PerfTimerPrinter hvn_timer(llvm::dbgs(), "HVN");
    // llvm::dbgs() << "FIXME: Opt broken?\n";
     mainCg_->optimize();
  }
  // ProfilerStop();
  llvm::dbgs() << "SparseBitmap =='s: " << Bitmap::numEq() << "\n";
  llvm::dbgs() << "SparseBitmap hash's: " << Bitmap::numHash() << "\n";

  for (auto &id_val : id_print) {
    llvm::dbgs() << "Printing node for id: " << id_val << "\n";
    ValueMap::Id val_id(id_val);

    llvm::dbgs() << "  " << val_id << ": " <<
      FullValPrint(val_id, mainCg_->vals()) << "\n";
  }

  /* FIXME(ddevec) HCD Not yet supported...
  // Then, HCD
  {
    if (!anders_no_opt) {
      util::PerfTimerPrinter hcd_timer(llvm::dbgs(), "HCD");
      // Gather hybrid cycle info from our graph
      HCD(cg, omap);
    }
  }
  */

  // Setup our live graph using the mainCg_
  graph_.init(*mainCg_, fcn_cfg, cgCache_.get(), callCgCache_.get());

  // Fill our graph
  {
    util::PerfTimerPrinter graph_timer(llvm::dbgs(), "Graph Creation");
    // Fill our online graph with the initial constraint set
    graph_.fill();
  }

  {
    // ProfilerStart("anders_solve.prof");
    util::PerfTimerPrinter solve_timer(llvm::dbgs(), "AndersSolve");
    if (solve()) {
      error("Solve failure!");
    }
    // ProfilerStop();
  }

  for (auto &fcn_name : fcn_names) {
    // DEBUG {{{
    auto fcn = m.getFunction(fcn_name);

    llvm::dbgs() << "Printing ptsto for function: " << fcn_name << "\n";
    llvm::dbgs() << "Printing args: " << fcn_name << "\n";
    for (auto it = fcn->arg_begin(), en = fcn->arg_end();
        it != en; ++it) {
      auto &arg = *it;
      llvm::dbgs() << "Arg is: " << arg << "\n";
      if (llvm::isa<llvm::PointerType>(arg.getType())) {
        auto ids = graph_.cg().vals().getIds(&arg);
        for (auto arg_id : ids) {
          llvm::dbgs() << "  ptsto[" << arg_id << "]: " <<
            ValPrint(arg_id, graph_.cg().vals()) << "\n";

          auto rep_id = getRep(arg_id);
          if (rep_id != arg_id) {
            llvm::dbgs() << "   REP: " << rep_id << ": " <<
                FullValPrint(rep_id, graph_.cg().vals()) << "\n";
          }

          if (graph_.getNode(rep_id).id() != rep_id) {
            llvm::dbgs() << "   Graph-REP: " << graph_.getNode(rep_id).id() <<
              ": " <<
              FullValPrint(graph_.getNode(rep_id).id(), graph_.cg().vals())
              << "\n";
          }

          auto &ptsto = getPointsTo(rep_id);

          llvm::dbgs() << "    ptsto prints as: " << ptsto << "\n";
          /*
          for (ObjectMap::ObjID obj_id : ptsto) {
            llvm::dbgs() << "    " << obj_id << ": " << ValPrint(obj_id)
                << "\n";
          });
          */
        }
      }
    }

    llvm::dbgs() << "Printing instructions: " << fcn_name << "\n";
    for (auto it = inst_begin(fcn), en = inst_end(fcn);
        it != en; ++it) {
      auto &inst = *it;
      if (llvm::isa<llvm::PointerType>(inst.getType())) {
        auto ids = graph_.cg().vals().getIds(&inst);

        llvm::dbgs() << "inst: " << inst << "\n";
        for (auto val_id : ids) {
          llvm::dbgs() << "  ptsto[" << val_id << "]: " <<
            ValPrint(val_id, graph_.cg().vals()) << "\n";

          auto rep_id = getRep(val_id);
          if (rep_id != val_id) {
            llvm::dbgs() << "   REP: " << rep_id << ": " <<
              FullValPrint(rep_id, graph_.cg().vals()) << "\n";
          }

          if (graph_.getNode(rep_id).id() != rep_id) {
            llvm::dbgs() << "   Graph-REP: " << graph_.getNode(rep_id).id()
              << ": " <<
              FullValPrint(graph_.getNode(rep_id).id(), graph_.cg().vals())
              << "\n";
          }

          auto &ptsto = getPointsTo(rep_id);

          llvm::dbgs() << "    ptsto prints as: " << ptsto << "\n";

          /*
          for (auto obj_id : ptsto) {
            llvm::dbgs() << "    " << obj_id << ": " << ValPrint(obj_id)
                << "\n";
          }
          */
        }
      }
    }
    //}}}
  }

#ifndef NDEBUG
  // Verify I don't have any crazy stuff in the graph
  uint32_t num_nodes = 0;
  uint32_t num_node_pts = 0;
  uint32_t num_reps = 0;
  uint64_t num_copy_edges = 0;
  uint64_t num_gep_edges = 0;
  uint64_t total_pts_size = 0;
  for (auto &node : graph_) {
    // Gather num nodes
    num_nodes++;
    if (!graph_.isRep(node)) {
      // Assert non-reps don't have succs or ptstos
      assert(node.ptsto().empty());
      assert(node.copySuccs().empty());
      assert(node.gepSuccs().empty());
    } else {
      // Gather num reps
      num_reps++;


      if (!node.ptsto().empty()) {
        num_node_pts++;
      }

      total_pts_size += node.ptsto().count();
      num_copy_edges += node.copySuccs().count();
      num_gep_edges += node.gepSuccs().size();
      /*
      for (auto id : node.ptsto()) {
        total_pts_size++;
        // Should point to a rep
        // Not guaranteed, ever
        // assert(graph_.getNode(id).id() == id);
      }

      // Count avg number of outgoing edges
      for (auto id_val : node.copySuccs()) {
        // Should point to a rep
        // Not guaranteed, if a succ is merged after the last visit to this node
        // assert(graph_.getNode(id).id() == id);
        num_copy_edges++;
      }

      for (auto &pr : node.gepSuccs()) {
        auto id = pr.first;
        // Should point to a rep
        // Not guaranteed, if a succ is merged after the last visit to this node
        // assert(graph_.getNode(id).id() == id);
        num_gep_edges++;
      }
      */
    }
  }

  llvm::dbgs() << "num_nodes: " << num_nodes << "\n";
  llvm::dbgs() << "num_nonempty_nodes: " << num_node_pts << "\n";
  llvm::dbgs() << "num_reps: " << num_reps << "\n";
  llvm::dbgs() << "num_copy_edges: " << num_copy_edges << "\n";
  llvm::dbgs() << "num_gep_edges: " << num_gep_edges << "\n";
  llvm::dbgs() << "total_pts_size: " << total_pts_size << "\n";

#endif

  // Free any memory no longer needed by the graph (now that solve is done)
  graph_.cleanup();
}

PtstoSet *SpecAndersAnalysis::ptsCacheGet(const llvm::Value *val) {
  // Check if we've cached the value
  auto rc = ptsCache_.emplace(std::piecewise_construct,
      std::make_tuple(val), std::make_tuple());

  // If not, merge together the individual nodes
  if (rc.second) {
    auto ids = graph_.cg().vals().getIds(val);

    for (auto val_id : ids) {
      auto &node_ptsto = getPointsTo(val_id);

      rc.first->second |= node_ptsto;
    }
  }

  return &rc.first->second;
}

