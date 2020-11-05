#include <fstream>
#include <sstream>
#include "llvm/ADT/Statistic.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include <llvm/IR/Constants.h>
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/IteratedDominanceFrontier.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Analysis/LoopInfo.h"
#include <unordered_set>


using namespace std;
using namespace llvm;




namespace{

    struct DebloatInstrument : public FunctionPass {

        static char ID;

        DebloatInstrument() : FunctionPass(ID) {}

        bool doInitialization(Module &M) override;
        bool runOnFunction(Function &F) override;

        void getAnalysisUsage(AnalysisUsage &AU) const override {
            AU.addRequired<PostDominatorTreeWrapperPass>();
            AU.addRequired<DominatorTreeWrapperPass>();
            AU.addRequired<LoopInfoWrapperPass>();
        }
    };
}


bool DebloatInstrument::doInitialization(Module &M)
{
    return false;
}


bool DebloatInstrument::runOnFunction(Function &F)
{
    return false;
}




char DebloatInstrument::ID = 0;
static RegisterPass<DebloatInstrument>
X("DebloatInstrument", "instrument IR with debloating functionality");
