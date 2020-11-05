#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Transforms/Scalar.h"
#include <fstream>
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/IteratedDominanceFrontier.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include <iostream>
#include <sstream>
#include "llvm-c/Core.h"
#include <unordered_set>


using namespace llvm;
using namespace std;




namespace {

    struct DebloatProfile : public FunctionPass {

      public:
        static char ID;

        DebloatProfile() : FunctionPass(ID) {}

        bool doInitialization(Module &M) override;
        bool runOnFunction(Function &F) override;

        void getAnalysisUsage(AnalysisUsage &AU) const override
        {
            AU.addRequired<PostDominatorTreeWrapperPass>();
            AU.addRequired<LoopInfoWrapperPass>();
            AU.setPreservesCFG();
            AU.addPreserved<GlobalsAAWrapperPass>();
        }

      private:

    };
}


bool DebloatProfile::doInitialization(Module &M)
{
    return false;
}


bool DebloatProfile::runOnFunction(Function &F)
{
    return false;
}




char DebloatProfile::ID = 0;
static RegisterPass<DebloatProfile>
Y("DebloatProfile", "Add profiling support for debloating");
