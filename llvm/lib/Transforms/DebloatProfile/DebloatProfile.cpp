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
#include "llvm/Demangle/Demangle.h"
#include <iostream>
#include <sstream>
#include "llvm-c/Core.h"
#include <unordered_set>


using namespace llvm;
using namespace std;

#define DEBUG_TYPE "DebloatProfile"


// TODO move these to a util function when we leave the llvm tree
std::string getDemangledName(const Function &F) {
  ItaniumPartialDemangler IPD;
  std::string name = F.getName().str();
  if (IPD.partialDemangle(name.c_str())) return name;
  if (IPD.isFunction())
    return IPD.getFunctionBaseName(nullptr, nullptr);
  else return IPD.finishDemangle(nullptr, nullptr);
}

std::string getDemangledName(const Function *F) { return getDemangledName(*F); }


typedef struct{
    unsigned int max_num_args;
    unsigned int num_calls_not_in_loops;
    unsigned int num_calls_in_loops;
    unsigned int num_loops_no_preheader;
}dp_stats_t;



namespace {

    struct DebloatProfile : public FunctionPass {

      public:
        static char ID;
        LoopInfo *LI;

        DebloatProfile() : FunctionPass(ID) {}

        bool doInitialization(Module &) override;
        bool runOnFunction(Function &) override;
        bool doFinalization(Module &) override;

        void getAnalysisUsage(AnalysisUsage &au) const override
        {
            au.addRequired<PostDominatorTreeWrapperPass>();
            au.addRequired<LoopInfoWrapperPass>();
            au.setPreservesCFG();
            au.addPreserved<GlobalsAAWrapperPass>();
        }

      private:
        Function *debprof_print_args_func;
        map<CallInst *, unsigned int> call_inst_to_id;
        std::map<std::string, unsigned int> func_name_to_id;
        unsigned int call_inst_count;
        unsigned int func_count;
        dp_stats_t stats;
        set<Loop *> instrumented_loops;

        bool call_inst_is_in_loop(CallInst *call_inst);
        bool can_ignore_called_func(Function *, CallInst *);
        void init_debprof_print_func(Module &);
        void dump_stats(void);

        void instrument_callsite(Instruction *call_inst,
                                 unsigned int callsite_id,
                                 unsigned int called_func_id,
                                 set<Value *> func_arguments_set);
        void instrument_outside_loop(Instruction *call_inst,
                                     unsigned int callsite_id,
                                     unsigned int called_func_id,
                                     set<Value *> func_arguments_set);
    };
}


bool DebloatProfile::doInitialization(Module &M)
{
    init_debprof_print_func(M);
    stats.max_num_args = 0;
    stats.num_calls_not_in_loops = 0;
    stats.num_calls_in_loops = 0;
    stats.num_loops_no_preheader = 0;
    return false;
}

bool DebloatProfile::doFinalization(Module &M)
{
    dump_stats();
    return false;
}


void DebloatProfile::dump_stats(void)
{
    FILE *fp = fopen("debprof_pass_stats.txt", "w");
    fprintf(fp, "max_num_args: %u\n", stats.max_num_args);
    fprintf(fp, "num_calls_in_loops: %u\n", stats.num_calls_in_loops);
    fprintf(fp, "num_calls_not_in_loops: %u\n", stats.num_calls_not_in_loops);
    fprintf(fp, "num_loops_no_preheader: %u\n", stats.num_loops_no_preheader);
    fclose(fp);
}


bool DebloatProfile::call_inst_is_in_loop(CallInst *call_inst)
{
    if(LI && LI->getLoopFor(call_inst->getParent())) {
        LLVM_DEBUG(dbgs() << "i see this call_inst inside a loop\n");
        stats.num_calls_in_loops++;
        return true;
    }
    stats.num_calls_not_in_loops++;
    return false;
}


bool DebloatProfile::can_ignore_called_func(Function *called_func, CallInst *call_inst)
{
    if(called_func == NULL){
        LLVM_DEBUG(dbgs()<<"called_func is NULL\n");
        return true;;
    }
    if(called_func->isIntrinsic()){
        LLVM_DEBUG(dbgs()<<"called_func isIntrinsic\n");
        return true;;
    }
    if(!called_func->hasName()){
        LLVM_DEBUG(dbgs()<<"called_func !hasName\n");
        return true;;
    }
    if(call_inst->getDereferenceableBytes(0)){
        LLVM_DEBUG(dbgs()<<"Skipping derefereceable bytes: "<<*call_inst<<"\n");
        return true;;
    }
    std::string callInstrString;
    llvm::raw_string_ostream callrso(callInstrString);
    call_inst->print(callrso);
    std::string toFindin = callrso.str();
    std::string ignoreclassStr("%class.");
    if(toFindin.find(ignoreclassStr) != std::string::npos){
        LLVM_DEBUG(dbgs()<<"Skipping ignoreclass: "<<*call_inst<<"\n");
        return true;;
    }
    return false;
}


bool DebloatProfile::runOnFunction(Function &F)
{
    unsigned int num_args;
    bool cannot_instrument;
    string func_name;
    CallInst *call_inst;

    //func_name = getDemangledName(F);
    //if(func_name == "main"){
    //    LLVM_DEBUG(dbgs() << "FOUND MAIN\n");
    //}else{
    //    LLVM_DEBUG(dbgs() << "NOT MAIN: " << func_name << "\n");
    //}
    LI  = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();

    for(Function::iterator it_bb = F.begin();
        it_bb != F.end();
        ++it_bb){

        BasicBlock *bb = &*it_bb;
        for(BasicBlock::iterator it_inst = bb->begin();
            it_inst != bb->end();
            ++it_inst){

            if(dyn_cast<InvokeInst>(&*it_inst)){
                continue;
            }

            call_inst = dyn_cast<CallInst>(&*it_inst);
            if(call_inst){
                Function *called_func = call_inst->getCalledFunction();
                if(DebloatProfile::can_ignore_called_func(called_func, call_inst)){
                    continue;
                }

                std::string called_func_name = called_func->getName().str();
                LLVM_DEBUG(dbgs()<<"called_func_name: "<<called_func_name<<"\n");

                //LLVM_DEBUG(dbgs()<<"\n callinst:"<<*call_inst);
                //LLVM_DEBUG(dbgs()<<"\n called func:"<<called_func->getName());
                //LLVM_DEBUG(dbgs()<<"\n does not throw:"<<call_inst->doesNotThrow());
                //LLVM_DEBUG(dbgs()<<"\n does not throw:"<<called_func->doesNotThrow());
                //LLVM_DEBUG(dbgs()<<"\n get getDereferenceableBytes:"<<call_inst->getDereferenceableBytes(0));

                if(call_inst_to_id.find(call_inst) == call_inst_to_id.end()){
                    call_inst_to_id[call_inst] = call_inst_count++;
                }
                //LLVM_DEBUG(dbgs() <<"\ninstrument_profile call_inst_count:"<<call_inst_count);
                //LLVM_DEBUG(dbgs() << " CallPredictionTrain: got call instr "<<*call_inst<<"\n");
                if(func_name_to_id.find(called_func_name) == func_name_to_id.end()){
                    func_name_to_id[called_func_name] = func_count++;
                }
                //LLVM_DEBUG(dbgs()<<"with arguments ::\n" );

                num_args = call_inst->getNumArgOperands();
                std::ostringstream callsite_info;
                callsite_info
                  << "\n#_CALL_instrument:funcID:"
                  << func_name_to_id[called_func_name]
                  << ":call_inst_count:"
                  << call_inst_to_id[call_inst]<<":"; //<<":argNum:"<<i<<":";
                callsite_info << "_max_args_:" << num_args << ":";

                set<Value*> func_arguments_set;
                cannot_instrument = false;

                for(unsigned int i = 0 ; i < num_args; i++){
                    Value *argV = call_inst->getArgOperand(i);
                    std::string prnt_type;
                    llvm::raw_string_ostream rso(prnt_type);
                    argV->getType()->print(rso);
                    LLVM_DEBUG(dbgs() << "argument:: " << i << " = " << *argV
                               << " of type::" << rso.str() << "\n");
                    if(dyn_cast<InvokeInst>(argV)){
                        cannot_instrument = true;
                        LLVM_DEBUG(dbgs() << "IS invoke instr should ignore: "
                                   << *argV);
                        break;
                    }
                    if(Instruction *argI = dyn_cast<Instruction>(argV)){
                        if(auto *c = dyn_cast<CallInst>(argI)){
                            if(c->getDereferenceableBytes(0)){
                                cannot_instrument = true;
                                LLVM_DEBUG(dbgs()
                                           << "IS invoke instr should ignore: "
                                           << *argV);
                                break;
                            }
                        }

                        //SmallPtrSet<BasicBlock *, 16> needRDFofBBs;
                        //get_parent_PHI_def_for(argI, needRDFofBBs);

                        // Now the needrdf is set, so get the rdfs and then
                        // instrument them, before moving on to next function call
                        //LLVM_DEBUG(dbgs() << "argument::" << *argI);
                        if(dyn_cast<Instruction>(argV)
                        && (argV->getType()->isIntegerTy()
                           || argV->getType()->isFloatTy()
                           || argV->getType()->isDoubleTy()
                           || argV->getType()->isPointerTy())){
                            LLVM_DEBUG(dbgs() << "valid type argument::" << *argI);
                            func_arguments_set.insert(argV);
                        }else{
                            LLVM_DEBUG(dbgs() << "wtf 1\n");
                        }
                        //SmallVector<BasicBlock *, 32> RDFBlocks;
                        //PostDominatorTree &PDT = getAnalysis<PostDominatorTreeWrapperPass>().getPostDomTree();
                        //computeControlDependence(PDT, needRDFofBBs, RDFBlocks);
                        //LLVM_DEBUG(dbgs()<<"Calling rdf instrument::"<<RDFBlocks.size());
                        //std::string rdf_info_str = instrumentRDF(RDFBlocks, call_inst_to_id[call_inst], i+1 );
                        //callsite_info<<rdf_info_str;
                    }else{
                        LLVM_DEBUG(dbgs() << "wtf 2z\n");
                        if((argV->getType()->isIntegerTy()
                         || argV->getType()->isFloatTy()
                         || argV->getType()->isDoubleTy()
                         || argV->getType()->isPointerTy())){
                            LLVM_DEBUG(dbgs() << "valid type argument\n");
                            func_arguments_set.insert(argV);
                        }
                    }
                }
                if(cannot_instrument){
                    continue;
                }



                if(!call_inst_is_in_loop(call_inst)){
                    instrument_callsite(call_inst,
                                        call_inst_to_id[call_inst],
                                        func_name_to_id[called_func_name],
                                        func_arguments_set);
                }else{
                    instrument_outside_loop(call_inst,
                                            call_inst_to_id[call_inst],
                                            func_name_to_id[called_func_name],
                                            func_arguments_set);
                }
            }
        }
    }
    return true;
}

void DebloatProfile::instrument_callsite(Instruction *call_inst,
                                        unsigned int callsite_id,
                                        unsigned int called_func_id,
                                        set<Value *> func_arguments_set)
{
    Module *thisModule = call_inst->getModule();
    Type *int32Ty = IntegerType::getInt32Ty(thisModule->getContext());
    Function *userInstrumentFunc = debprof_print_args_func;
    LLVM_DEBUG(dbgs() << "function::" << *userInstrumentFunc);
    LLVM_DEBUG(dbgs() << "Instrumented for callins::" << call_inst
               << " callsite::"  << callsite_id << "\n");

    IRBuilder<> builder(call_inst);
    std::vector<Value *> ArgsV;


    // We have to instrument a call to debprof_print_args. To do that, we need
    // to build a list of arguments to pass to it. The first argument
    // to debprof_print_args is the number of variadic args to follow.
    // But we can't just use func_arguments_set.size() to help us here,
    // because we might ignore some arguments. So, push a 0 into the list
    // as a placeholder. We'll update it after we finish adding the other
    // arguments.
    ArgsV.push_back(llvm::ConstantInt::get(int32Ty, 0, false));

    // The next two arguments are always the callsite_id, followd by the
    // called_func_id.
    ArgsV.push_back(llvm::ConstantInt::get(int32Ty, callsite_id, false));
    ArgsV.push_back(llvm::ConstantInt::get(int32Ty, called_func_id, false));

    // Now push the args that were passed at the callsite
    for(Value *funcArg : func_arguments_set){
        LLVM_DEBUG(dbgs() << "checking funcArg\n");
        Value *castedArg = nullptr;
        if(funcArg != NULL){
            if (funcArg->getType()->isFloatTy() || funcArg->getType()->isDoubleTy()){
                LLVM_DEBUG(dbgs() << "float or double\n");
                castedArg = builder.CreateFPToSI(funcArg, int32Ty);
            }else if(funcArg->getType()->isIntegerTy()){
                LLVM_DEBUG(dbgs() << "integer\n");
                castedArg = builder.CreateIntCast(funcArg, int32Ty, true);
            }else if(funcArg->getType()->isPointerTy()){
                LLVM_DEBUG(dbgs() << "pointer\n");
                castedArg = builder.CreatePtrToInt(funcArg, int32Ty);
            }

            if(castedArg == nullptr){
                continue;
            }
            ArgsV.push_back(castedArg);
            LLVM_DEBUG(dbgs() << "pushing::" << *castedArg << "\n");
        }
    }
    // The size of ArgsV is equal to the final number of args we're passing
    // to debprof_print_args. Subtract 1 to get the number of variadic args,
    // and update argument 0 accordingly.
    unsigned int num_variadic_args = ArgsV.size() - 1;
    ArgsV[0] = llvm::ConstantInt::get(int32Ty, num_variadic_args, false);

    // Track the max number of args that debprof_print_args is going to write
    // to file.
    if(num_variadic_args > stats.max_num_args){
        stats.max_num_args = num_variadic_args;
    }

    // Create the call to debprof_print_args
    Value *callinstr = builder.CreateCall(userInstrumentFunc, ArgsV);
    LLVM_DEBUG(dbgs() << "callinstr::" << *callinstr << "\n");

}


void DebloatProfile::instrument_outside_loop(Instruction *call_inst,
                                             unsigned int callsite_id,
                                             unsigned int called_func_id,
                                             set<Value *> func_arguments_set)
{
    Loop *L;
    Instruction *insBef;
    BasicBlock *preHeaderBB;
    Function *userInstrumentFunc = debprof_print_args_func;

    L = LI->getLoopFor(call_inst->getParent());
    preHeaderBB = L->getLoopPreheader();
    if(preHeaderBB){
        if(instrumented_loops.count(L) == 0){
            instrumented_loops.insert(L);
            insBef = preHeaderBB->getTerminator();

            // FIXME for now, instrument just the callsite_id and the
            // called_func_id
            Type *int32Ty = IntegerType::getInt32Ty(call_inst->getModule()->getContext());
            IRBuilder<> builder(insBef);
            std::vector<Value *> ArgsV;
            ArgsV.push_back(llvm::ConstantInt::get(int32Ty, 2, false));
            ArgsV.push_back(llvm::ConstantInt::get(int32Ty, callsite_id, false));
            ArgsV.push_back(llvm::ConstantInt::get(int32Ty, called_func_id, false));
            Value *callinstr = builder.CreateCall(userInstrumentFunc, ArgsV);
            LLVM_DEBUG(dbgs() << "callinstr(loop)::" << *callinstr << "\n");
        }
    }else{
        // FIXME see LLVM doxygen on getLoopPreheader. The fix is to walk
        // incoming edges to the first BB of the loop
        stats.num_loops_no_preheader++;
    }

}




void DebloatProfile::init_debprof_print_func(Module &M)
{
    LLVMContext &ctxt = M.getContext();
    Type *int32Ty = IntegerType::getInt32Ty(ctxt);
    Type *ArgTypes[] = { int32Ty  };
    string custom_instr_func_name("debprof_print_args");

    /*FunctionCallee fc =
    //dyn_cast<FunctionCallee>(
    //debprof_print_args_func = dyn_cast<Function>(
        M.getOrInsertFunction(
            "debprof_print_args",
            FunctionType::get(
                int32Ty ,
                ArgTypes,
                true
            )
        );

    //);
    FunctionCallee *fcp = &fc;
    //debprof_print_args_func = dyn_cast<Function>(fcp);
    */

    debprof_print_args_func =
      Function::Create(FunctionType::get(int32Ty, ArgTypes, true),
                       Function::ExternalLinkage,
                       "debprof_print_args",
                       M);


}




char DebloatProfile::ID = 0;
static RegisterPass<DebloatProfile>
Y("DebloatProfile", "Add profiling support for debloating");
