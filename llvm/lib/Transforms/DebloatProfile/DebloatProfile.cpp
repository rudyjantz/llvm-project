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

//#define LOOP_BASIC


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
        std::map<Instruction*, uint64_t> jumpphinodes;
        Type *int32Ty;

        bool call_inst_is_in_loop(CallInst *call_inst);
        bool can_ignore_called_func(Function *, CallInst *);
        void init_debprof_print_func(Module &);
        void dump_stats(void);

        void backslice(Instruction *I);

        void instrument_callsite(Instruction *call_inst,
                                 unsigned int callsite_id,
                                 unsigned int called_func_id,
                                 set<Value *> func_arguments_set);
        void instrument_outside_loop_basic(Instruction *call_inst,
                                           unsigned int callsite_id,
                                           unsigned int called_func_id,
                                           set<Value *> func_arguments_set);
        void instrument_outside_loop_avail_args(Instruction *call_inst,
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
    call_inst_count = 0;
    func_count = 0;

    int32Ty = IntegerType::getInt32Ty(M.getContext());


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
                    LLVM_DEBUG(dbgs() << "Hit init\n");
                }
                //LLVM_DEBUG(dbgs() <<"\ninstrument_profile call_inst_count:"<<call_inst_count);
                //LLVM_DEBUG(dbgs() << " CallPredictionTrain: got call instr "<<*call_inst<<"\n");
                if(func_name_to_id.find(called_func_name) == func_name_to_id.end()){
                    func_name_to_id[called_func_name] = func_count++;
                }
                //LLVM_DEBUG(dbgs()<<"with arguments ::\n" );

                num_args = call_inst->getNumArgOperands();
                LLVM_DEBUG(dbgs()
                  << "\n#_CALL_instrument:funcID:" << func_name_to_id[called_func_name]
                  << ":call_inst_count:" << call_inst_to_id[call_inst]<<":"
                  << "num_args:" << num_args << "\n");

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
                            LLVM_DEBUG(dbgs() << "valid type argument::" << *argI << "\n");
                            func_arguments_set.insert(argV);
                        }else{
                            LLVM_DEBUG(dbgs() << "wtf 1\n");
                        }
                        //SmallVector<BasicBlock *, 32> RDFBlocks;
                        //PostDominatorTree &PDT = getAnalysis<PostDominatorTreeWrapperPass>().getPostDomTree();
                        //computeControlDependence(PDT, needRDFofBBs, RDFBlocks);
                        //LLVM_DEBUG(dbgs()<<"Calling rdf instrument::"<<RDFBlocks.size());
                        //std::string rdf_info_str = instrumentRDF(RDFBlocks, call_inst_to_id[call_inst], i+1 );
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
    IRBuilder<> builder(call_inst);
    std::vector<Value *> ArgsV;


    LLVM_DEBUG(dbgs() << "function::" << *debprof_print_args_func);
    LLVM_DEBUG(dbgs() << "Instrumented for callins::" << call_inst
               << " callsite::"  << callsite_id << "\n");



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
    Value *callinstr = builder.CreateCall(debprof_print_args_func, ArgsV);
    LLVM_DEBUG(dbgs() << "callinstr::" << *callinstr << "\n");

}


void DebloatProfile::instrument_outside_loop(Instruction *call_inst,
                                             unsigned int callsite_id,
                                             unsigned int called_func_id,
                                             set<Value *> func_arguments_set)
{
#ifdef LOOP_BASIC
    instrument_outside_loop_basic(call_inst,
                                  callsite_id,
                                  called_func_id,
                                  func_arguments_set);
#else
    instrument_outside_loop_avail_args(call_inst,
                                       callsite_id,
                                       called_func_id,
                                       func_arguments_set);
#endif
}


/**********************************************
 * @param instruction, branchnumber, operand number
 *        switch number
 * Backslice the operand of the branch to its
 * native birth point and instrument an emit
 * function to print out them
 * @return nothing
 *******************************************/
void DebloatProfile::backslice(Instruction *I)
{
    LLVM_DEBUG(dbgs() << "hit 0\n");

    Instruction *I2, *I3;

    // FIXME: jumpphinodes needs proper initialization, etc.
    if(!jumpphinodes[I]){
        LoadInst *LI = dyn_cast<LoadInst>(I);
        CallInst *CI = dyn_cast<CallInst>(I);
        SelectInst *SI = dyn_cast<SelectInst>(I);

        /**************************************************************************
        * If instruction is a function pointer or a binary operator, we have
        * found the native birthpoint.
        **************************************************************************/
        if(CI || SI || I->isBinaryOp() || dyn_cast<GetElementPtrInst>(I) ||
           dyn_cast<AllocaInst>(I) || dyn_cast<GlobalVariable>(I)){
            LLVM_DEBUG(dbgs() << "hit 1\n");
            //instrument(I, dyn_cast<Value>(I));
        }else if(LI){
            /**************************************************************************
            * If instruction is a load instruction, we have to check if it loading from
            * a local variable (alloca), a global variable (globalvariable) or a pointer
            * (getelementptrinst/bitcast) as this means we have found the native birthpoint.
            * Else, backslice the operand.
            **************************************************************************/
            AllocaInst *AI = dyn_cast<AllocaInst>(LI->getOperand(0));
            GlobalVariable *GV = dyn_cast<GlobalVariable>(LI->getOperand(0));
            GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(LI->getOperand(0));
            InvokeInst *II = dyn_cast<InvokeInst>(LI->getOperand(0));
            I2 = dyn_cast<Instruction>(LI->getOperand(0));

            if((!I2 && !GV) || AI || GV || GEP || II
               || dyn_cast<Instruction>(LI->getOperand(0))->isCast()){
                //instrument(I, dyn_cast<Value>(I));
                LLVM_DEBUG(dbgs() << "hit 2\n");
                LLVM_DEBUG(dbgs() << *I << "\n");
                LLVM_DEBUG(dbgs() << "hit 2 done\n");
            }
            else {
                backslice(I2);
                LLVM_DEBUG(dbgs() << "hit 3\n");
            }
        }else if(isa<PHINode>(I)){
            /**************************************************************************
            * If instruction is a Phi function, we backslice for each operand in the Phi
            * function.
            **************************************************************************/
            jumpphinodes[I] = 1;
            I3 = I;
            while(isa<PHINode>(I3)){
                I3 = I3->getNextNode();
            }
            I3 = I3->getPrevNode();

            uint64_t i;
            for(i = 0; i < I->getNumOperands(); ++i){
                I2 = dyn_cast<Instruction>(I->getOperand(i));
                if(I2){
                    InvokeInst *II = dyn_cast<InvokeInst>(I2);
                    if(!II){
                        backslice(I2);
                        LLVM_DEBUG(dbgs() << "hit 4\n");
                    }
                    else {
                        //instrument(I2, dyn_cast<Value>(I2));
                        LLVM_DEBUG(dbgs() << "hit 5\n");
                    }
                }
                else {
                    //instrument(I3, dyn_cast<Value>(I->getOperand(i)));
                    LLVM_DEBUG(dbgs() << "hit 6\n");
                }
            }

        }else {
            /**************************************************************************
            * Normally, these are casting functions left so we just backslice until we get
            * to a load
            **************************************************************************/
            I2 = dyn_cast<Instruction>(I->getOperand(0));
            if(I2){
                InvokeInst *II = dyn_cast<InvokeInst>(I2);
                if(!II){
                    backslice(dyn_cast<Instruction>(I->getOperand(0)));
                    LLVM_DEBUG(dbgs() << "hit 7\n");
                }
                else {
                    //instrument(I2, dyn_cast<Value>(I2));
                    LLVM_DEBUG(dbgs() << "hit 8\n");
                }
            }
            else {
                //instrument(I, dyn_cast<Value>(I->getOperand(0)));
                LLVM_DEBUG(dbgs() << "hit 9\n");
            }
        }
    }
}

void DebloatProfile::instrument_outside_loop_avail_args(Instruction *call_inst,
                                                        unsigned int callsite_id,
                                                        unsigned int called_func_id,
                                                        set<Value *> func_arguments_set)
{
    Loop *L;
    Instruction *insBef;
    BasicBlock *preHeaderBB;

    L = LI->getLoopFor(call_inst->getParent());
    preHeaderBB = L->getLoopPreheader();
    if(preHeaderBB){
        // if we have not yet instrumented this loop...
        if(instrumented_loops.count(L) == 0){
            instrumented_loops.insert(L);
            insBef = preHeaderBB->getTerminator();

            // FIXME for now, instrument just the callsite_id and the
            // called_func_id
            IRBuilder<> builder(insBef);
            std::vector<Value *> ArgsV;



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

                    Instruction *backslice_me = dyn_cast<Instruction>(castedArg);
                    if(backslice_me){
                        backslice(backslice_me);
                        // works, but it dumps the ptr, not the pointed-to value
                        //ArgsV.push_back(backslice_me->getOperand(0));

                        // fails at runtime
                        //ArgsV.push_back(builder.CreateIntCast(backslice_me->getOperand(0), int32Ty, true));
                        // not sure what this is even doing. behaves like the naive ptr case
                        //ArgsV.push_back(builder.CreatePtrToInt(backslice_me->getOperand(0), int32Ty));

                        ArgsV.push_back(builder.CreateLoad(int32Ty, backslice_me->getOperand(0)));

                        LLVM_DEBUG(dbgs() << "pushing::" << *backslice_me->getOperand(0) << "\n");
                        LLVM_DEBUG(dbgs() << "was type:" << *backslice_me->getOperand(0)->getType() << "\n");
                        //LLVM_DEBUG(dbgs() << "was type:" << *(*backslice_me->getOperand(0)).getType() << "\n");
                    }else{
                        ArgsV.push_back(castedArg);
                        LLVM_DEBUG(dbgs() << "pushing::" << *castedArg << "\n");
                    }
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
            Value *callinstr = builder.CreateCall(debprof_print_args_func, ArgsV);
            LLVM_DEBUG(dbgs() << "callinstr::" << *callinstr << "\n");










        }
    }else{
        // FIXME see LLVM doxygen on getLoopPreheader. The fix is to walk
        // incoming edges to the first BB of the loop
        stats.num_loops_no_preheader++;
    }

}


void DebloatProfile::instrument_outside_loop_basic(Instruction *call_inst,
                                                   unsigned int callsite_id,
                                                   unsigned int called_func_id,
                                                   set<Value *> func_arguments_set)
{
    Loop *L;
    Instruction *insBef;
    BasicBlock *preHeaderBB;

    L = LI->getLoopFor(call_inst->getParent());
    preHeaderBB = L->getLoopPreheader();
    if(preHeaderBB){
        if(instrumented_loops.count(L) == 0){
            instrumented_loops.insert(L);
            insBef = preHeaderBB->getTerminator();

            // FIXME for now, instrument just the callsite_id and the
            // called_func_id
            IRBuilder<> builder(insBef);
            std::vector<Value *> ArgsV;
            ArgsV.push_back(llvm::ConstantInt::get(int32Ty, 2, false));
            ArgsV.push_back(llvm::ConstantInt::get(int32Ty, callsite_id, false));
            ArgsV.push_back(llvm::ConstantInt::get(int32Ty, called_func_id, false));
            Value *callinstr = builder.CreateCall(debprof_print_args_func, ArgsV);
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
    Type *ArgTypes[] = { int32Ty  };
    string custom_instr_func_name("debprof_print_args");

    debprof_print_args_func =
      Function::Create(FunctionType::get(int32Ty, ArgTypes, true),
                       Function::ExternalLinkage,
                       "debprof_print_args",
                       M);


}




char DebloatProfile::ID = 0;
static RegisterPass<DebloatProfile>
Y("DebloatProfile", "Add profiling support for debloating");
