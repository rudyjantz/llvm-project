/*
 * Copyright (C) 2015 David Devecsery
 */

#include "llvm/Oha/Cg.h"

#include <cassert>
#include <cstdint>

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

#include "llvm/Oha/Assumptions.h"
#include "llvm/Oha/ExtInfo.h"
#include "llvm/Oha/ValueMap.h"
#include "llvm/Oha/lib/IndirFcnTarget.h"
#include "llvm/Oha/lib/UnusedFunctions.h"

llvm::cl::opt<bool>
  no_spec("anders-no-spec", llvm::cl::init(false),
      llvm::cl::value_desc("bool"),
      llvm::cl::desc("if set anders will not make any "
        "speculative assumptions"));

// Helpers for contraint IDs {{{
static bool traceInt(const llvm::Value *val, std::set<const llvm::Value *> &src,
    std::map<const llvm::Value *, bool> &seen) {
  auto it = seen.find(val);

  if (it != std::end(seen)) {
    return it->second;
  }

  seen[val] = false;

  // llvm::dbgs() << "  Tracing: " << *val << "\n";

  int32_t opcode = 0;

  std::vector<const llvm::Value *> ops;

  if (llvm::isa<llvm::Argument>(val) || llvm::isa<llvm::ConstantInt>(val)) {
    seen[val] = true;
    return true;
  } else if (auto ce = dyn_cast<llvm::ConstantExpr>(val)) {
    opcode = ce->getOpcode();
    for (size_t i = 0; i < ce->getNumOperands(); i++) {
      ops.push_back(ce->getOperand(i));
    }
  } else if (auto ins = dyn_cast<llvm::Instruction>(val)) {
    opcode = ins->getOpcode();
    for (size_t i = 0; i < ins->getNumOperands(); i++) {
      ops.push_back(ins->getOperand(i));
    }
  } else {
    llvm_unreachable("Unknown Integeral type");
  }

  bool ret;

  switch (opcode) {
    case llvm::Instruction::Invoke:
    case llvm::Instruction::FPToUI:
    case llvm::Instruction::FPToSI:
    case llvm::Instruction::ICmp:
    case llvm::Instruction::FCmp:
    case llvm::Instruction::Call:
    case llvm::Instruction::VAArg:
    case llvm::Instruction::ExtractElement:
    case llvm::Instruction::ExtractValue:
      ret = true;
      break;

    case llvm::Instruction::PtrToInt:
      src.insert(ops[0]);
      ret = false;
      break;

    // For loads, do what we can...
    case llvm::Instruction::Load:
      {
        // If its a load from a global
        if (auto gv = dyn_cast<llvm::GlobalVariable>(ops[0])) {
          auto gi = gv->getInitializer();

          ret = traceInt(gi, src, seen);
        } else {
          auto li = cast<llvm::LoadInst>(val);

          auto addr = ops[0];
          const llvm::Value *source = nullptr;

          auto bb = li->getParent();
          for (auto &ins : *bb) {
            if (auto si = dyn_cast<llvm::StoreInst>(&ins)) {
              if (si->getPointerOperand() == addr) {
                source = si->getOperand(0);
              }
            } else if (auto ld = dyn_cast<llvm::LoadInst>(&ins)) {
              if (ld == li) {
                break;
              }
            }
          }

          if (source != nullptr) {
            ret = traceInt(source, src, seen);
          } else {
            ret = true;
          }
        }
        break;
      }
    // 1 input arith operations, trace the addr
    case llvm::Instruction::Shl:
    case llvm::Instruction::LShr:
    case llvm::Instruction::AShr:
    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
    case llvm::Instruction::BitCast:
      {
        auto op_type = ops[0]->getType();

        if (llvm::isa<llvm::IntegerType>(op_type)) {
          ret = traceInt(ops[0], src, seen);
        } else {
          assert(opcode == llvm::Instruction::BitCast);
          ret = true;
        }

        break;
      }
    // Binary arithmetic
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::Mul:
    case llvm::Instruction::UDiv:
    case llvm::Instruction::SDiv:
    case llvm::Instruction::URem:
    case llvm::Instruction::SRem:
    case llvm::Instruction::And:
    case llvm::Instruction::Or:
    case llvm::Instruction::Xor:
      ret = traceInt(ops[0], src, seen) &&
                 traceInt(ops[1], src, seen);
      break;

    case llvm::Instruction::PHI:
      ret = false;
      for (auto op : ops) {
        auto op_type = op->getType();
        if (llvm::isa<llvm::IntegerType>(op_type)) {
          ret |= traceInt(op, src, seen);
        } else if (llvm::isa<llvm::PointerType>(op_type)) {
          src.insert(op);
        } else {
          ret = true;
        }
      }
      break;

    // Select...
    case llvm::Instruction::Select:
      ret = traceInt(ops[0], src, seen) &&
                 traceInt(ops[1], src, seen);
      break;

    default:
      ret = true;
      llvm_unreachable("Unsupported trace_int operand");
  }

  seen[val] = ret;
  return ret;
}

void Cg::addConstraintForType(ConstraintType ctype,
    const llvm::Type *type, Id dest,
    Id src_obj) {

  dout("Passed in inferred_type: " << type << "\n");

  // Strip wrapping arrays
  while (auto at = dyn_cast<llvm::ArrayType>(type)) {
    type = at->getContainedType(0);
  }

  if (auto st = dyn_cast<llvm::StructType>(type)) {
    auto &si = modInfo_.getStructInfo(st);

    // Only create one addressof object, per alloc
    if (ctype == ConstraintType::AddressOf) {
      add(ctype, src_obj, dest, 0);
    } else {
      for (size_t i = 0; i < si.numSizes(); i++) {
        // Add an addr of to this offset
        dout("Adding AddressOf for struct.  Dest: " << dest << ", src "
          << src_obj << " + " << i << "\n");
        add(ctype, src_obj, dest, i);
      }
    }
  } else {
    // No offs defaults to 0 in offs column, which is what we want for a
    //   non-struct object
    add(ctype, src_obj, dest);
  }
}

bool Cg::addConstraintsForExternalCall(llvm::ImmutableCallSite &cs,
    const llvm::Function *called_fcn,
    const CallInfo &call_info) {
  llvm::dbgs() << "have external fcn: " << called_fcn->getName() << "\n";
  // Get the exteral
  auto &info = extInfo_.getInfo(called_fcn);

  // Just for good measure, make sure I've got a def of the values
  auto ArgI = cs.arg_begin();
  auto ArgE = cs.arg_end();

  for (; ArgI != ArgE; ++ArgI) {
    getDef(*ArgI);
  }

  if (extInfo_.isUnknownFunction((info))) {
    llvm::dbgs() << "WARNING: Unknwon external function: " <<
      ValPrinter(cs.getInstruction()) << "\n";
  }

  return info.insertCallCons(*this, cs, call_info);
}

void Cg::addConstraintsForDirectCall(llvm::ImmutableCallSite &cs,
    const llvm::Function *called_fcn,
    const CallInfo &caller_info,
    const CallInfo &callee_info) {
  auto &caller_args = caller_info.args();
  auto caller_ret_id = caller_info.ret();

  auto &callee_args = callee_info.args();
  auto callee_ret_id = callee_info.ret();

  // If this call returns a pointer
  if (llvm::isa<llvm::PointerType>(cs.getInstruction()->getType())) {
    // If the function that's called also returns a pointer
    // Add a copy from the return value into this value
    // Copy from the caller to callee for rets
    /*
    llvm::dbgs() << "Adding copy from callee " << callee_ret_id <<
      " to caller: " << caller_ret_id << " ci: " << *cs.getInstruction() <<
      "\n";
    */
    add(ConstraintType::Copy, callee_ret_id, caller_ret_id);
  // The callsite returns a non-pointer, but the function returns a
  // pointer value, treat it as a pointer cast to a non-pointer
  } else if (
      llvm::isa<llvm::PointerType>(called_fcn->getFunctionType()->getReturnType())) {  // NOLINT
    // The call now aliases the universal value
    llvm::dbgs() << "FIXME: Ignoring int to ptr for call\n";
  }

  auto ArgI = cs.arg_begin();
  auto ArgE = cs.arg_end();

  // This instance should have been handled by extinfo
  assert(!called_fcn->isDeclaration());

  auto FargI = called_fcn->arg_begin();
  auto FargE = called_fcn->arg_end();

  size_t argno = 0;
  // For each arg
  for (; FargI != FargE && ArgI != ArgE; ++FargI, ++ArgI) {
    // If we expect a pointer type
    if (llvm::isa<llvm::PointerType>(FargI->getType())) {
      // And we get one!
      if (llvm::isa<llvm::PointerType>((*ArgI)->getType())) {
        // llvm::dbgs() << "Adding arg copy!\n";
        // llvm::dbgs() << "Callinst: " << *cs.getInstruction() << "\n";
        auto node_id = vals_.createPhonyID();
        auto dest_id = callee_args[argno];
        auto src_id = caller_args[argno];

        add(ConstraintType::Copy, node_id, src_id, dest_id);
      // But if its not a pointer type...
      } else {
        // Map it to int stores (i2p)
        // auto node_id = omap.createPhonyID();
        // auto dest_id = getValue(cg, omap, FargI);

        llvm::dbgs() << "FIXME: Ignoring int to ptr for arg\n";
      }
    }

    argno++;
  }

  // Varargs magic :(
  if (called_fcn->getFunctionType()->isVarArg()) {
    for (; ArgI != ArgE; ++ArgI) {
      if (llvm::isa<llvm::PointerType>((*ArgI)->getType())) {
        add(ConstraintType::Copy, caller_args[argno],
            callee_info.varArg());
      }
      argno++;
    }
  }
}

void Cg::addConstraintsForIndirectCall(llvm::ImmutableCallSite &cs,
    const CallInfo &call_info) {
  auto &called_val = *cs.getCalledValue();

  if (llvm::isa<llvm::InlineAsm>(&called_val)) {
    llvm::errs() << "WARNING: Ignoring inline asm!\n";
    return;
  }

  auto id = vals_.getDef(&called_val);

  // Prepare for inserting the call info into the live graph
  indirCalls_.emplace_back(id, call_info, cfgId_);
}

bool Cg::addConstraintsForCall(
    llvm::ImmutableCallSite &cs) {
  auto &called_val = *cs.getCalledValue();

  if (llvm::isa<llvm::InlineAsm>(&called_val)) {
    llvm::errs() << "WARNING: Ignoring inline asm!\n";
    return false;
  }

  calls_.emplace_back(*this, cs);

  return true;
}

void Cg::addGlobalInit(Id src_id, Id dest_id) {
  auto glbl_id = vals_.createPhonyID();

  // Add the store to the constraint graph
  add(ConstraintType::Store, glbl_id, src_id, dest_id);
}

ValueMap::Id Cg::getGlobalInitializer(const llvm::GlobalValue &glbl) {
  Id ret = ValueMap::UniversalValue;

  auto name = glbl.getName();

  if (name == "stdout") {
    ret = vals_.getNamed("stdio");
  } else if (name == "stderr") {
    ret = vals_.getNamed("stdio");
  } else if (name == "stdin") {
    ret = vals_.getNamed("stdio");
  } else if (name == "environ") {
    // envp points to env
    ret = vals_.getNamed("envp");
  }

  return ret;
}

int32_t Cg::addGlobalInitializerConstraints(Id dest,
    const llvm::Constant *C) {
  int32_t offset = 1;
  /*
  dbg << "In glbl init cons\n";
  dbg << "glbl init cons dest: (" << dest << ") " <<
    ValPrint(dest) << "\n";
  */
  // llvm::dbgs() << "Entry constant: " << *C << ", dest: " << dest << "\n";
  // Simple case, single initializer
  if (C->getType()->isSingleValueType()) {
    if (llvm::isa<llvm::PointerType>(C->getType())) {
      auto const_id = getDef(C);
      /*
      llvm::dbgs() << "Adding global init for: (" << dest << ") " <<
          ValPrint(dest, vals_) << " to (" << const_id << ") "
          << ValPrint(const_id, vals_) << "\n";
      */

      /*
      llvm::dbgs() << "Assigning constant: " << *C << "\n";
      llvm::dbgs() << "  To: " << dest << " from: " << const_id << "\n";
      */
      addGlobalInit(const_id, dest);
    }
  // Initialized to null value
  } else if (C->isNullValue()) {
    // NOTE: We ignore this, because null values don't point to anything...
    // dbg << "Glbl init on: (" << dest << ") " << ValPrint(dest) << "\n";
    if (llvm::isa<llvm::StructType>(C->getType())) {
      // FIXME: Offset = sizeof struct type?
      auto &si = modInfo_.getStructInfo(cast<llvm::StructType>(C->getType()));
      offset = si.size();
    } else {
    }
  // Set to some other defined value
  } else if (!llvm::isa<llvm::UndefValue>(C)) {
    // It must be an array, or struct
    assert(llvm::isa<llvm::ConstantArray>(C) ||
        llvm::isa<llvm::ConstantStruct>(C) ||
        llvm::isa<llvm::ConstantDataSequential>(C));

    /*
    dbg << "Adding STRUCT global init for: (" << dest << ") " <<
      ValPrint(dest) << "\n";
      */
    // For each field of the initializer, add a constraint for the field
    // This is done differently for structs than for array
    if (auto cs = dyn_cast<llvm::ConstantStruct>(C)) {
      // llvm::dbgs() << "Splitting struct constant: " << *C << "\n";
      // Need to reset to 0, because we're adding fields
      offset = 0;
      for (const llvm::Use &field : cs->operands()) {
        auto field_cons = cast<const llvm::Constant>(field);
        auto new_dest = vals().createPhonyID();
        add(ConstraintType::Copy, dest, new_dest, offset);
        offset += addGlobalInitializerConstraints(new_dest,
            field_cons);
      }
    } else {
      // llvm::dbgs() << "Arraying constant: " << *C << "\n";
      for (const auto &field : C->operands()) {
        auto field_cons = cast<const llvm::Constant>(field);
        offset = addGlobalInitializerConstraints(dest, field_cons);
      }
    }
  } else {
    // Undef values get no ptsto
    auto type = C->getType();
    while (auto at = dyn_cast<llvm::ArrayType>(type)) {
      type = at->getContainedType(0);
    }

    if (auto st = dyn_cast<llvm::StructType>(type)) {
      auto &si = modInfo_.getStructInfo(st);
      offset = si.size();
    } else {
      offset = 1;
    }
  }

  return offset;
}

void Cg::addGlobalConstraintForType(ConstraintType ctype,
    const llvm::Type *, ValueMap::Id src,
    ValueMap::Id dest) {
  /*
  llvm::dbgs() << "Adding Global AddressOf for NON-struct.  Dest: " << dest
      << ", src " << src << "\n";
  */
  add(ctype, src, dest);
}
//}}}

// Helpers for individual instruction constraints -- idTypeInst {{{
void Cg::idRetInst(const llvm::Instruction &inst) {
  auto &ret = cast<const llvm::ReturnInst>(inst);

  // Returns without arguments (void) don't add constraints
  if (ret.getNumOperands() == 0) {
    return;
  }

  // Get the return value
  llvm::Value *src = ret.getOperand(0);

  // If its not a pointer, we don't care about it
  if (!llvm::isa<llvm::PointerType>(src->getType())) {
    return;
  }

  // The function in which ret was called
  const llvm::Function *parent_fcn = ret.getParent()->getParent();

  auto returned_id = getDef(src);

  llvm::dbgs() << "ret edge: " << returned_id << " -> "
    << getCallInfo(parent_fcn).ret() << "\n";
  add(ConstraintType::Copy,
      returned_id, getCallInfo(parent_fcn).ret());
}

bool Cg::idCallInst(const llvm::Instruction &inst) {
  auto ci = dyn_cast<llvm::CallInst>(&inst);
  llvm::ImmutableCallSite cs(ci);

  auto called_fcn = LLVMHelper::getFcnFromCall(ci);

  // Determine if this is an external call
  auto &info = extInfo_.getInfo(cs);
  auto alloc_info = info.getAllocInfo(cs, modInfo_);
  // If this does an allocation, create addressof operations for it
  if (called_fcn != nullptr &&
      // This is a complex conditional, it means:
      //   If the called function is a declaration (no body) and extinfo says it
      //     allocs
      //   OR
      //   If the called fucntion has a body, and extinfo recoginizes it (not
      //     unknown) AND extinfo says it allocs
      ((called_fcn->isDeclaration() && alloc_info.first != AllocStatus::None) ||
       (called_fcn->isDeclaration() && alloc_info.first != AllocStatus::None &&
         !extInfo_.isUnknownFunction(info)))) {
    /*
    llvm::dbgs() << "Have malloc call: " <<
      inst.getParent()->getParent()->getName() << ":" << inst << "\n";
    */
    // If its a malloc, we don't add constriants for the call, we instead
    //   pretend the call is actually a addressof operation
    //
    // Unfortunately, malloc doesn't tell us what size strucutre is
    //   being allocated, we infer this information from its uses
    auto inferred_type = alloc_info.second;
    auto size = modInfo_.getSizeOfType(inferred_type);

    auto dest_id = getDef(&inst);
    auto src_obj_id = vals_.createAlloc(&inst, size);

    dout("Malloc addAddressForType(" << dest_id << ", " << src_obj_id
        << ")\n");
    llvm::dbgs() << "Malloc src: " << src_obj_id << " size: "  << size <<
      " inst: " << inst << "\n";
    addConstraintForType(ConstraintType::AddressOf,
        inferred_type, dest_id, src_obj_id);

    return false;
  }

  return addConstraintsForCall(cs);
}

void Cg::idAllocaInst(const llvm::Instruction &inst) {
  auto &alloc = cast<const llvm::AllocaInst>(inst);

  // If the thing we're allocating is a structure... then we need to handle
  //   addressof for all sub-fields!
  auto type = alloc.getAllocatedType();
  auto size = modInfo_.getSizeOfType(type);

  auto dest_id = getDef(&alloc);
  auto src_obj_id = vals_.createAlloc(&alloc, size);
  /*
  llvm::dbgs() << "Alloca inst has src: " << src_obj_id << ": "
    << alloc << "\n";
  */

  addConstraintForType(ConstraintType::AddressOf,
      type, dest_id, src_obj_id);
}

void Cg::idLoadInst(const llvm::Instruction &inst) {
  auto &ld = cast<const llvm::LoadInst>(inst);

  auto addr_id = getDef(ld.getOperand(0));

  if (llvm::isa<llvm::PointerType>(ld.getType())) {
    auto dest_id = getDef(&ld);

    add(ConstraintType::Load, dest_id, addr_id, dest_id);
  } else if (auto ptr_t =
      dyn_cast<llvm::PointerType>(ld.getOperand(0)->getType())) {
    if (llvm::isa<llvm::PointerType>(ptr_t->getElementType()) &&
        llvm::isa<llvm::IntegerType>(ld.getType())) {
      // Ld is an int value... those may alias.  So we instead create a
      //   phony id
      auto dest_id = getDef(&ld);

      llvm::dbgs() << __LINE__ << ": Load int into pointer\n";
      add(ConstraintType::Load, addr_id,
          dest_id,
          ValueMap::IntValue);
    }
  } else if (llvm::isa<llvm::StructType>(ld.getType())) {
    llvm::errs() << "FIXME: Unhandled struct load!\n";
  }
}

void Cg::idStoreInst(const llvm::Instruction &inst) {
  auto &st = cast<const llvm::StoreInst>(inst);

  auto st_id = getDef(&st);

  dout("store is: " << ValPrint(st_id) << "\n");
  dout("arg(0) is: " << *st.getOperand(0) << "\n");
  dout("arg(1) is: " << *st.getOperand(1) << "\n");

  auto dest_type = dyn_cast<llvm::PointerType>(st.getOperand(1)->getType());
  // We must have a def for operand 1 EOM!
  getDef(st.getOperand(1));

  if (llvm::isa<llvm::PointerType>(st.getOperand(0)->getType())) {
    // Store from ptr
    auto dest = getDef(st.getOperand(1));
    dout("Got ptr dest of: " << dest << " : " << ValPrint(dest) <<
      "\n");
    add(ConstraintType::Store,
        st_id,
        getDef(st.getOperand(0)),
        dest);
  } else if (llvm::ConstantExpr *ce =
      dyn_cast<llvm::ConstantExpr>(st.getOperand(0))) {
    // If we just cast a ptr to an int then stored it.. we can keep info on it
    if (ce->getOpcode() == llvm::Instruction::PtrToInt) {
      auto dest = getDef(st.getOperand(1));
      if (dest == ValueMap::NullValue) {
        // If this is not an object, store to the value
        dest = getDef(st.getOperand(1));
        llvm::dbgs() << "No object for store dest: " << dest << " : " <<
          ValPrint(dest, vals_) << "\n";
      }
      llvm::dbgs() << "Store on inst: " << ValPrinter(&inst) << "\n";
      add(ConstraintType::Store,
          st_id,
          getDef(st.getOperand(0)),
          dest);
    // Uhh, dunno what to do now
    } else {
      llvm::errs() << "FIXME: Unhandled constexpr case!\n";
    }
  // put int value into the int pool
  } else if (llvm::isa<llvm::IntegerType>(st.getOperand(0)->getType()) &&
      llvm::isa<llvm::PointerType>(st.getOperand(1)->getType())) {
    if (!llvm::isa<llvm::IntegerType>(dest_type->getContainedType(0))) {
      auto dest = getDef(st.getOperand(1));

      llvm::dbgs() << __LINE__ << ": Store int into pointer: " <<
        st << "\n";
      add(ConstraintType::Store,
          st_id,
          ValueMap::IntValue,
          dest);
    } else {
      // Just set up the pointer dest... yeah, its weird
      getDef(st.getOperand(1));
      /*
      llvm::dbgs() << "Skipping Universal Cons for store to int *: " << st <<
        "\n";
      */
      // NOTE: We must return here, because we didn't acutlaly add a store!
      return;
    }
  // Poop... structs
  } else if (llvm::isa<llvm::StructType>(st.getOperand(0)->getType())) {
    llvm::errs() << "FIXME: Ignoring struct store\n";
    /*
    add(ConstraintType::Store, st_id,
        omap.getValue(st.getOperand(1)),
        ObjectMap::AgregateNode);
        */
  } else {
    // Floats are stored, but not in graph...
    if (!st.getOperand(0)->getType()->isFloatTy() &&
        !st.getOperand(0)->getType()->isDoubleTy()) {
      // Didn't add it to the graph?
      llvm::errs() << "FIXME: Not adding store object to graph?: "
          << st << "\n";
    }
    return;
  }
}

void Cg::idGEPInst(const llvm::Instruction &inst) {
  auto &gep = cast<const llvm::GetElementPtrInst>(inst);

  auto gep_id = getDef(&gep);
  auto src_offs = LLVMHelper::getGEPOffs(modInfo_, gep);
  auto src_id = getDef(gep.getOperand(0));

  dout("id gep_id: " << ValPrint(gep_id) << "\n");
  dout("  src_offs: " << src_offs << "\n");
  dout("  src_id: " << src_id << "\n");

  /*
  static size_t gep_count = 0;
  gep_count++;
  if (gep_count % 100000 == 0) {
    assert(0);
  }
  */

  add(ConstraintType::Copy,
      src_id,
      gep_id,
      src_offs);
}

void Cg::idI2PInst(const llvm::Instruction &inst) {
  // ddevec - FIXME: Could trace through I2P here, by keeping a listing
  //    of i2ps...
  // sfs does this, Andersens doesn't.  I don't think its a sound approach, as
  // something external may modify any int reference passed to it (where we're
  // unaware of what's in it) and screw up our tracking
  // Instead I'm just going to go w/ the Andersen's, approach, give it an
  // int value

  auto dest_val = getDef(&inst);

  std::set<const llvm::Value *> src;
  std::map<const llvm::Value *, bool> seen;

  // llvm::dbgs() << "Start trace\n";
  bool has_i2p = traceInt(inst.getOperand(0), src, seen);
  // llvm::dbgs() << "Finish trace\n";
  seen.clear();

  for (auto val : src) {
    add(ConstraintType::Copy,
        getDef(val), dest_val);
  }

  if (has_i2p) {
    // llvm::dbgs() << __LINE__ << ": i2p int into pointer " << inst << "\n";
    add(ConstraintType::Copy,
        ValueMap::IntValue, dest_val);
  }
}

void Cg::idBitcastInst(const llvm::Instruction &inst) {
  auto &bcast = cast<const llvm::BitCastInst>(inst);

  assert(llvm::isa<llvm::PointerType>(inst.getType()));

  // llvm::dbgs() << "bitcast: " << bcast << "\n";
  auto dest_id = getDef(&bcast);
  auto src_id = getDef(bcast.getOperand(0));

  assert(llvm::isa<llvm::PointerType>(bcast.getOperand(0)->getType()));

  add(ConstraintType::Copy, src_id, dest_id);
}

void Cg::idPhiInst(const llvm::Instruction &inst) {
  auto &phi = *cast<const llvm::PHINode>(&inst);

  assert(llvm::isa<llvm::PointerType>(phi.getType()));

  // hheheheheh PHI-d
  auto phid = getDef(&phi);

  for (size_t i = 0, e = phi.getNumIncomingValues(); i != e; ++i) {
    add(ConstraintType::Copy, getDef(phi.getIncomingValue(i)),
        phid);
  }
}

void Cg::idSelectInst(const llvm::Instruction &inst) {
  auto &select = cast<const llvm::SelectInst>(inst);

  // this inst --> select: op(0) ? op(1) : op(2)

  if (llvm::isa<llvm::PointerType>(select.getType())) {
    auto sid = getDef(&select);

    add(ConstraintType::Copy, getDef(select.getOperand(1)), sid);
    add(ConstraintType::Copy, getDef(select.getOperand(2)), sid);
  } else if (llvm::isa<llvm::StructType>(select.getType())) {
    llvm::errs() << "FIXME: unsupported select on struct!\n";
  }
}

void Cg::idVAArgInst(const llvm::Instruction &) {
  llvm_unreachable("Vaarg not handled yet");
}

void Cg::idExtractInst(const llvm::Instruction &inst) {
  auto &extract_inst = cast<llvm::ExtractValueInst>(inst);
  if (llvm::isa<llvm::PointerType>(extract_inst.getType())) {
    add(ConstraintType::Copy,
        ValueMap::AggregateValue,
        getDef(&extract_inst));
  } else if (llvm::isa<llvm::IntegerType>(extract_inst.getType())) {
    llvm::dbgs() << __LINE__ << ": EXTRACT INT?\n";
    add(ConstraintType::Copy,
        ValueMap::AggregateValue,
        ValueMap::IntValue);
  }
}

void Cg::idInsertInst(const llvm::Instruction &inst) {
  auto &insert_inst = cast<llvm::InsertValueInst>(inst);
  auto src_val = insert_inst.getOperand(1);

  if (llvm::isa<llvm::PointerType>(src_val->getType())) {
    add(ConstraintType::Copy,
        getDef(src_val),
        ValueMap::AggregateValue);
  } else if (llvm::isa<llvm::IntegerType>(src_val->getType())) {
    llvm::dbgs() << __LINE__ << ": INSERT INT?\n";
    add(ConstraintType::Copy,
        ValueMap::IntValue,
        ValueMap::AggregateValue);
  }
}
//}}}

// Cg Constructor
Cg::Cg(const llvm::Function *fcn,
      const DynamicInfo &dyn_info,
      AssumptionSet &as,
      ModInfo &mod_info,
      ExtLibInfo &ext_info,
      CsCFG &cfg) :
      csCFG_(cfg),
      dynInfo_(dyn_info),
      as_(as),
      modInfo_(mod_info),
      extInfo_(ext_info) {
  extInfo_.init(*fcn->getParent(), vals());
  // Assume we're part of main for now...
  curStacks_.emplace_back(1);
  curStacks_.back().back() =
    util::convert_id<CsCFG::Id>(dynInfo_.call_info.getMainContext());

  // Populate constraint set for this function (and only this function)
  // Create CallInfo for fcn_
  CallInfo ci(*this, fcn);
  // Add my fcn/ci to CFG
  cfgId_ = localCFG_.addNode(fcn, ci);

  callInfo_.emplace(std::piecewise_construct,
      std::make_tuple(fcn),
      std::make_tuple(std::move(ci), cfgId_));
  // Populate constraints
  populateConstraints(as);
}

void Cg::populateConstraints(AssumptionSet &as) {
  assert(callInfo_.size() == 1);
  assert(std::begin(callInfo_)->first != nullptr);
  auto entry_block = &std::begin(callInfo_)->first->getEntryBlock();
  assert(dynInfo_.used_info.isUsed(entry_block) || no_spec);
  std::set<const llvm::BasicBlock *> seen;
  // Assert this function has a body?
  scanBB(entry_block, as, seen);
}

void Cg::scanBB(const llvm::BasicBlock *bb,
    AssumptionSet &as, std::set<const llvm::BasicBlock *> &seen) {
  auto &unused_fcns = dynInfo_.used_info;

  if (!unused_fcns.isUsed(bb) && !no_spec) {
    as.add(std::unique_ptr<Assumption>(
          new DeadCodeAssumption(const_cast<llvm::BasicBlock *>(bb))));
    return;
  }

  // If we've analyzed this block before, skip it
  auto rc = seen.emplace(bb);
  if (!rc.second) {
    return;
  }

  for (auto &inst : *bb) {
    bool is_ptr = llvm::isa<llvm::PointerType>(inst.getType());

    switch (inst.getOpcode()) {
      case llvm::Instruction::Ret:
        {
          assert(!is_ptr);

          idRetInst(inst);
        }
        break;
      case llvm::Instruction::Invoke:
      case llvm::Instruction::Call:
        idCallInst(inst);
        break;
      case llvm::Instruction::Alloca:
        assert(is_ptr);
        idAllocaInst(inst);
        break;
      case llvm::Instruction::Load:
        idLoadInst(inst);
        break;
      case llvm::Instruction::Store:
        assert(!is_ptr);
        idStoreInst(inst);
        break;
      case llvm::Instruction::GetElementPtr:
        assert(is_ptr);
        idGEPInst(inst);
        break;
      case llvm::Instruction::PtrToInt:
        assert(!is_ptr);
        // P2I does not need to be handled, as it only consumes a ptr, and does
        //   not def/mod one
        /*
        idP2IInst(cg, omap, inst);
        */
        break;
      case llvm::Instruction::IntToPtr:
        assert(is_ptr);
        idI2PInst(inst);
        break;
      case llvm::Instruction::BitCast:
        if (is_ptr) {
          idBitcastInst(inst);
        }
        break;
      case llvm::Instruction::PHI:
        if (is_ptr) {
          idPhiInst(inst);
        }
        break;
      case llvm::Instruction::Select:
          idSelectInst(inst);
        break;
      case llvm::Instruction::VAArg:
        if (is_ptr) {
          idVAArgInst(inst);
        }
        break;
      case llvm::Instruction::ExtractValue:
        if (is_ptr) {
          idExtractInst(inst);
        }
        break;
      case llvm::Instruction::InsertValue:
        idInsertInst(inst);
        break;
      default:
        assert(!is_ptr && "Unknown instruction has a pointer return type!");
    }
  }

  // Process all of our successor blocks (In DFS order)
  for (auto it = succ_begin(bb), en = succ_end(bb);
      it != en; ++it) {
    scanBB(*it, as, seen);
  }
}

void Cg::addGlobalConstraints(const llvm::Module &m) {
  // Special Constraints {{{
  // First, we set up some constraints for our special constraints:
  // Universal value
  add(ConstraintType::AddressOf, ValueMap::UniversalValue,
      ValueMap::UniversalValue);

  // FIXME: The SFS component does not know the predecessors of UniversalValue,
  //   as Andersens does not provide them...  So I (unsoundly) removed it for
  //   now?
  auto ui_store_id = vals_.createPhonyID();
  add(ConstraintType::Store, ui_store_id, ValueMap::UniversalValue,
      ValueMap::UniversalValue);

  extInfo_.addGlobalConstraints(m, *this);
  //}}}

  // Global Variables {{{
  // Okay, first create nodes for all global variables:
  for (const llvm::GlobalVariable &glbl : m.globals()) {
    // Associate the global address with a node:
    auto type = glbl.getType()->getElementType();

    auto size = modInfo_.getSizeOfType(type);
    /*
    llvm::dbgs() << "size for: " << glbl.getName() << " is: " <<
        size << "\n";
    */
    // Okay, so I need to do this for each global...
    auto val_id = getDef(&glbl);
    auto obj_id = vals_.createAlloc(&glbl, size);

    /*
    llvm::dbgs() << "Adding glbl constraint for: " << glbl <<
     "(thats val: " << val_id << ", obj: " << obj_id << ")\n";
    */

    addGlobalConstraintForType(ConstraintType::AddressOf,
      type, obj_id, val_id);

    // If its a global w/ an initalizer
    // NOTE: We assume we have everything linked together, so the initializer
    // wont be over-written by a library... this may be false in some cases,
    // those should use "definitive initializer"...
    // if (glbl.hasDefinitiveInitializer())
    if (glbl.hasInitializer() &&
        llvm::isa<llvm::ConstantPointerNull>(glbl.getInitializer())) {
      dout("Global Zero Initializer: " << glbl.getName() << "\n");
      // We don't add any ptsto constraints for null value here, because null
      //   value points to nothing...
    } else if (glbl.hasInitializer()) {
      dout("Adding glbl initializer for: " << glbl << "\n");
      // llvm::dbgs() << "Adding glbl initializer for: " << glbl << "\n";
      addGlobalInitializerConstraints(val_id,
        glbl.getInitializer());
    // Doesn't have initializer
    } else {
      auto glbl_val = getGlobalInitializer(glbl);

      if (glbl_val == ValueMap::UniversalValue) {
        llvm::dbgs() << "FIXME: Global Init -- universal value -- global: "
          << glbl.getName() << "\n";
      }
      /*
      cg.add(ConstraintType::Copy, omap.getValue(&glbl),
          glbl_val);
      */

      // Also store the value into this
      addGlobalInit(glbl_val, getDef(&glbl));
    }
  }

  // Also add ptstos for fcns
  for (auto &fcn : m) {
    auto fcn_val = getDef(&fcn);
    auto fcn_alloc = vals_.createAlloc(&fcn, 1);

    /*
    llvm::dbgs() << "fcn copy (" << fcn.getName() << "): " <<
      fcn_val << " <- " << fcn_alloc << "\n";
    */

    add(ConstraintType::AddressOf, fcn_alloc, fcn_val);
  }


  // Finally, argv
  auto main_fcn = m.getFunction("main");
  auto &main_ci = callInfo_.at(main_fcn).first;
  // Fill in the argumetns
  auto &main_args = main_ci.args();
  if (main_args.size() >= 2) {
    auto argv_dest = main_args[1];
    auto argv_src = vals_.getNamed("argv");
    add(ConstraintType::Copy, argv_src, argv_dest);
  }

  if (main_args.size() == 3) {
    auto envp_dest = main_args[2];
    auto envp_src = vals_.getNamed("envp");
    add(ConstraintType::Copy, envp_src, envp_dest);
  }

  //}}}
}

// Inserts the constraints from rhs into this
//   Returns a "CgRemap" containing the remapped addresses of any externally
//     visible constraints (eg calls)
std::map<const llvm::Function *, std::pair<CallInfo, CsFcnCFG::Id>>
Cg::mapIn(const Cg &rhs) {
  // Merge global constraints from rhs into vals_
  // Create new nodes in vals_ for each non-global constraint in rhs
  auto rhs_remap = vals_.import(rhs.vals_);
  auto cfg_remap = localCFG_.copyNodes(rhs.localCFG_, rhs_remap);

  auto id_xfrm = [&rhs_remap] (const Id &old_id) {
    return rhs_remap[old_id];
  };

  auto cons_xfrm = [&id_xfrm] (const Constraint &old_cons) {
    // llvm::dbgs() << "remap cons: " << old_cons << "\n";
    auto new_src = id_xfrm(old_cons.src());
    assert(new_src != Id::invalid());
    auto new_dest = id_xfrm(old_cons.dest());
    assert(new_dest != Id::invalid());
    auto new_rep = id_xfrm(old_cons.rep());
    assert(new_rep != Id::invalid());
    return Constraint(old_cons.type(),
        new_src, new_dest, new_rep,
        old_cons.offs());
  };

  // Copy all constraints from rhs into vals_, remapping according to the newly
  //    created ids
  constraints_.reserve(constraints_.size() + rhs.constraints_.size());
  std::transform(std::begin(rhs.constraints_), std::end(rhs.constraints_),
      std::back_inserter(constraints_), cons_xfrm);

  // Convert the calls_ from rhs
  calls_.reserve(calls_.size() + rhs.calls_.size());
  std::transform(std::begin(rhs.calls_), std::end(rhs.calls_),
      std::back_inserter(calls_),
      [&rhs_remap] (const CallInfo &ci) {
        CallInfo new_ci(ci);
        new_ci.remap(rhs_remap);
        return new_ci;
      });
  indirCalls_.reserve(indirCalls_.size() + rhs.indirCalls_.size());
  std::transform(std::begin(rhs.indirCalls_), std::end(rhs.indirCalls_),
      std::back_inserter(indirCalls_),
      [&rhs_remap, &cfg_remap]
      (const std::tuple<Id, CallInfo, CsFcnCFG::Id> &tup) {
        auto &id = std::get<0>(tup);
        auto &ci = std::get<1>(tup);
        auto &cfg_id = std::get<2>(tup);

        CallInfo new_ci(ci);
        new_ci.remap(rhs_remap);
        return std::make_tuple(rhs_remap[id], new_ci, cfg_remap[cfg_id]);
      });

  // Add the new calls to calls_

  // Convert the callInfo_ from rhs using the newly created ids, store in ret
  std::map<const llvm::Function *, std::pair<CallInfo, CsFcnCFG::Id>>
    ret(rhs.callInfo_);
  for (auto &pr : ret) {
    auto &rhs_pr = pr.second;

    rhs_pr.first.remap(rhs_remap);
    rhs_pr.second = cfg_remap[rhs_pr.second];
  }

  // Also merge any invalid stacks gathered from their cg
  invalidStacks_.insert(std::begin(rhs.invalidStacks_),
      std::end(rhs.invalidStacks_));

  // Return the externally visible remapping
  return ret;
}

// Resolve an internal call
void Cg::resolveDirCyclicCall(llvm::ImmutableCallSite &cs,
    const llvm::Function *called_fcn, CallInfo &caller_info,
    CallInfo &callee_info, CsFcnCFG::Id callee_node_id,
    std::vector<std::vector<CsCFG::Id>> new_stacks) {
  // If this is a cycle, we shouldn't have to add a frame to our stack...
  // for each stack in curStacks_
  //   If back != new_id
  //     Add a new stack with this back
  //     Check the new stack is valid

  for (auto &stack : new_stacks) {
    curStacks_.emplace_back(std::move(stack));
  }

  addConstraintsForDirectCall(cs, called_fcn, caller_info, callee_info);
  auto callee_cfg_node = localCFG_.getNode(callee_node_id);
  callee_cfg_node.addPred(cfgId_);
}

void Cg::resolveDirAcyclicCall(CgCache &base_cgs, CgCache &full_cgs,
    llvm::ImmutableCallSite &cs,
    const llvm::Function *called_fcn, CallInfo &caller_info,
    std::vector<std::vector<CsCFG::Id>> new_stacks,
    std::vector<std::vector<CsCFG::Id>> invalid_stacks) {
  auto &call_info = dynInfo_.call_info;
  // For each stack in curStacks_
  //   new_stack = stack;
  //   If new_stack.back() != new_id
  //     stack.push_back(back);
  //
  //   if new_stack.valid():
  //     new_stacks.push_back(new_stack)
  // if new_stacks.empty()
  //   Don't do call, invalid dyn call path

  // If we don't have a valid stack
  if (call_info.hasDynData() && !no_spec && new_stacks.empty()) {
    // llvm::dbgs() << "Instruction: " << ValPrinter(cs.getInstruction())
    //     << "\n";
    llvm::dbgs() << "  Skipping call due to no valid dyn stack\n";
    // llvm::dbgs() << "  cs id: " << csCFG_.getId(cs.getInstruction()) << "\n";
    // Add invalid stacks which made me skip this call to my list of invalid
    //   stacks!
    for (auto &stack : invalid_stacks) {
       // llvm::dbgs() << "  stack: " << util::print_iter(stack) << "\n";
      invalidStacks_.emplace(std::move(stack));
    }
    return;
  }

  // Check for the full cg:
  auto tmp_cg = std::move(base_cgs.getCg(called_fcn).clone(
        std::move(new_stacks)));
  const Cg *pcg = nullptr;
  if (!call_info.hasDynData() || no_spec) {
    pcg = full_cgs.tryGetCg(called_fcn);
  }

  if (pcg == nullptr) {
    tmp_cg.resolveCalls(base_cgs, full_cgs);

    if (!call_info.hasDynData() || no_spec) {
      full_cgs.addCg(called_fcn, std::move(tmp_cg));
      pcg = full_cgs.tryGetCg(called_fcn);
    } else {
      pcg = &tmp_cg;
    }
  }
  auto &dest_cg = *pcg;

  // Create a new copy of dest_cg's nodes in my cg
  auto remap = mapIn(dest_cg);

  // Connect the call args into dest_cg
  auto &callee_pr = remap.at(called_fcn);
  auto &callee_info = callee_pr.first;
  auto &callee_cfg_id = callee_pr.second;

  addConstraintsForDirectCall(cs, called_fcn, caller_info, callee_info);

  // llvm::dbgs() << "Have localCFG: " << localCFG_ << "\n";

  // Finally, update my localCFG_
  auto &callee_cfg_node = localCFG_.getNode(callee_cfg_id);
  /*
  llvm::dbgs() << "!! adding pred?: "
     << localCFG_.getNode(cfgId_).fcn()->getName() << " <- " <<
    callee_cfg_node.fcn()->getName() << "\n";
  */
  callee_cfg_node.addPred(cfgId_);
}

std::vector<std::vector<CsCFG::Id>>
Cg::getCalleeStacks(llvm::ImmutableCallSite &cs,
    std::vector<std::vector<CsCFG::Id>> *pinvalid_stacks) {
  auto &call_info = dynInfo_.call_info;
  std::vector<std::vector<CsCFG::Id>> new_stacks;
  if (!call_info.hasDynData() || no_spec) {
    return new_stacks;
  }

  for (auto &stack : curStacks_) {
    auto new_id = csCFG_.getId(cs.getInstruction());
    if (stack.back() != new_id) {
      std::vector<CsCFG::Id> new_stack(stack.size() + 1);
      std::vector<CsCFG::Id> cs_new_stack(stack.size() + 1);
      // std::copy(std::begin(stack), std::end(stack), std::begin(new_stack));
      size_t i = 0;
      for (auto &id : stack) {
        cs_new_stack[i] = util::convert_id<CsCFG::Id>(id);
        new_stack[i] = id;
        ++i;
      }
      cs_new_stack.back() = util::convert_id<CsCFG::Id>(new_id);
      new_stack.back() = new_id;

      if (call_info.hasDynData() && !call_info.isValid(cs_new_stack)) {
        if (pinvalid_stacks != nullptr) {
          pinvalid_stacks->emplace_back(std::move(new_stack));
        }
        // This is an invalid stack
        continue;
      }
      new_stacks.emplace_back(std::move(new_stack));
    }
  }
  return new_stacks;
}

void Cg::resolveDirCalls(CgCache &base_cgs, CgCache &full_cgs,
    std::vector<call_tuple> &dir_calls) {

  std::vector<call_tuple> acyc_calls;
  for (auto &tup : dir_calls) {
    auto &cs = std::get<0>(tup);
    auto called_fcn = std::get<1>(tup);
    auto &caller_info = *std::get<2>(tup);

    // If this is direct && is external
    if (called_fcn->isDeclaration()) {
      // Add it as an external function
      addConstraintsForExternalCall(cs, called_fcn, caller_info);
    // If it is to a function within our scc (recursion), then connect those
    //   nodes
    } else {
      llvm::dbgs() << "  called_fcn is: " << called_fcn->getName() << "\n";

      auto it = callInfo_.find(called_fcn);
      if (it != std::end(callInfo_)) {
        auto &callee_info = it->second.first;
        auto callee_node_id = it->second.second;
        auto new_stacks = getCalleeStacks(cs, nullptr);
        resolveDirCyclicCall(cs, called_fcn, caller_info, callee_info,
            callee_node_id, std::move(new_stacks));
      } else {
        acyc_calls.emplace_back(cast<llvm::CallInst>(cs.getInstruction()),
            called_fcn, &caller_info);
      }
    }
  }

  // Now that we've resolved any recursive calls, or external calls, handle
  //   acyclic calls
  for (auto &tup : acyc_calls) {
    auto &cs = std::get<0>(tup);
    auto called_fcn = std::get<1>(tup);
    auto &caller_info = *std::get<2>(tup);

    std::vector<std::vector<CsCFG::Id>> invalid_stacks;
    auto new_stacks = getCalleeStacks(cs, &invalid_stacks);

    // llvm::dbgs() << "Resolving acyc: " << ValPrinter(called_fcn) << "\n";

    resolveDirAcyclicCall(base_cgs, full_cgs, cs, called_fcn, caller_info,
        std::move(new_stacks), std::move(invalid_stacks));
  }
}

// Handles calls outside of this function
//   NOTE -- assumes SCCs are merged
void Cg::resolveCalls(CgCache &base_cgs, CgCache &full_cgs) {
  auto &indir_info = dynInfo_.indir_info;
  // Resolve each call
  std::vector<call_tuple> dir_calls;
  for (auto &caller_info : calls_) {
    auto ci = caller_info.ci();
    llvm::ImmutableCallSite cs(ci);

    auto called_fcn = LLVMHelper::getFcnFromCall(cs);

    // llvm::dbgs() << "Resolve call: " << ValPrinter(ci) << "\n";

    // If it is a direct call, add the appropriate constraints...
    // llvm::dbgs() << "Resolve call: " << ValPrinter(ci) << "\n";
    if (called_fcn != nullptr) {
      /*
      llvm::dbgs() << "  Have dir resolution: " << called_fcn->getName() <<
        "\n";
      */
      dir_calls.emplace_back(cs, called_fcn, &caller_info);
    // Else add an external call constraint
    } else {
      // Check for indir info:
      if (indir_info.hasInfo() && !no_spec) {
        // llvm::dbgs() << "have indir info!\n";
        // llvm::dbgs() << "ci is: " << *ci << "\n";
        auto &targets = indir_info.getTargets(ci);

        for (auto &target : targets) {
          auto fcn_target = cast<llvm::Function>(target);

          /*
          llvm::dbgs() << "Forcing direct resolution of: " << ValPrinter(ci)
            << "\n";
          llvm::dbgs() << "  to: " << fcn_target->getName() << "\n";
          */
          /*
          llvm::dbgs() << "  Have dir resolution: " << fcn_target->getName() <<
            "\n";
          */
          dir_calls.emplace_back(cs, fcn_target, &caller_info);
        }

        // Add spec assumption:
        // First build the assumption set
        std::vector<const llvm::Value *> fcn_asmps;
        for (auto val : targets) {
          // llvm::dbgs() << "Target is: " << ValPrinter(val) << "\n";
          fcn_asmps.emplace_back(val);
        }

        // Now add the assumption
        // Note the assumption is about the callsites called fcn ptr
        /*
        llvm::dbgs() << "Adding pts asmp @: " << ValPrinter(ci) << "\n";
        llvm::dbgs() << "   arg: " << ValPrinter(cs.getCalledValue()) << "\n";
        */
        as_.add(
            std14::make_unique<PtstoAssumption>(
              const_cast<llvm::Instruction *>(cs.getInstruction()),
              cs.getCalledValue(),
              fcn_asmps));
      } else {
        // llvm::dbgs() << "  Indir call?\n";
        addConstraintsForIndirectCall(cs, caller_info);
      }
    }
  }

  resolveDirCalls(base_cgs, full_cgs, dir_calls);

  // Remove all calls after they have been resolved
  calls_.clear();
}

void Cg::lowerAllocs() {
  // First remap w/in the allocation list
  auto remap = vals_.lowerAllocs();

  /*
  auto call_info_remap =
    [&remap] (std::pair<const llvm::Function *, CallInfo> &pr) {
      pr.second.remap(remap);
    };
  */

  auto call_remap =
    [&remap] (CallInfo ci) {
      ci.remap(remap);
    };

  auto indir_call_remap =
    [&remap] (std::tuple<Id, CallInfo, CsFcnCFG::Id> &tup) {
      std::get<1>(tup).remap(remap);
      std::get<0>(tup) = remap[std::get<0>(tup)];
    };

  auto cons_remap = [&remap] (Constraint &cons) {
    cons.remap(remap);
  };

  // First, call info
  for (auto &pr : callInfo_) {
    pr.second.first.remap(remap);
  }
  // Then, calls
  std::for_each(std::begin(calls_), std::end(calls_), call_remap);
  // Indir calls
  std::for_each(std::begin(indirCalls_), std::end(indirCalls_),
      indir_call_remap);
  // finally Constraints
  std::for_each(std::begin(constraints_), std::end(constraints_),
      cons_remap);

  localCFG_.updateNodes(remap);
}

void Cg::mergeCalls(const std::vector<CallInfo> &calls,
    std::vector<CallInfo> &new_calls,
    const std::map<const llvm::Function *,
        std::pair<CallInfo, CsFcnCFG::Id>> &call_remap) {
  for (auto &caller_info : calls) {
    // If this is a direct call to a fcn in rhs
    auto ci = caller_info.ci();
    llvm::ImmutableCallSite cs(ci);

    auto called_fcn = LLVMHelper::getFcnFromCall(cs);

    // Make a new call_info vector
    // llvm::dbgs() << "checking merge: " << called_fcn->getName() << "\n";
    auto it = call_remap.find(called_fcn);
    if (it != std::end(call_remap)) {
      auto &callee_info = it->second.first;
      // llvm::dbgs() << "Merging call: " << called_fcn->getName() << "\n";
      // Connect the caller to callee:
      addConstraintsForDirectCall(cs, called_fcn, caller_info, callee_info);

      // Add edge in my localCFG_
      auto callee_cfg_id = it->second.second;
      auto caller_id = callInfo_.at(ci->getParent()->getParent()).second;
      auto &callee_cfg_node = localCFG_.getNode(callee_cfg_id);
      callee_cfg_node.addPred(caller_id);
    } else {
      new_calls.push_back(caller_info);
    }
  }
}

// Done before resolve calls, to merge together statically detected SCCs
void Cg::mergeCg(const Cg &rhs) {
  mergeScc(rhs);
}

void Cg::mergeScc(const Cg &rhs) {
  // Do the scc merge
  // Encorporate their fcns into my fcns
  //   ?Assert that the two don't overlap?
  //   Merge in constraints for their fcns
  //     -- This includes "ValueMaps"

  // First, sanity check that rhs and I are disjoint
  /*
  llvm::dbgs() << "Adding rhs with callinfos:\n";
  for (auto &pr : rhs.callInfo_) {
    llvm::dbgs() << "  " << pr.first->getName() << "\n";
  }

  llvm::dbgs() << "\n  My with callinfos:\n";
  for (auto &pr : callInfo_) {
    llvm::dbgs() << "    " << pr.first->getName() << "\n";
  }
  */
  if_debug_enabled(
      for (auto &pr : rhs.callInfo_) {
        assert(callInfo_.find(pr.first) == std::end(callInfo_));
      });

  // Map in the CG of rhs
  auto remap_fcns = mapIn(rhs);

  // Now move all of the remap call infos into our callInfo_ map
  std::move(std::begin(remap_fcns), std::end(remap_fcns),
      std::inserter(callInfo_, std::begin(callInfo_)));

  std::vector<CallInfo> new_calls;

  // Resolve any direct calls from their fcns to my fcns
  mergeCalls(calls_, new_calls, callInfo_);

  calls_ = std::move(new_calls);

  //   Assert indirCalls_ empty, those should be resolved after scc merges
  assert(indirCalls_.empty());
  assert(rhs.indirCalls_.empty());

  // FIXME(ddevec) -- see below
  llvm::dbgs() << "Connect localCFG?\n";

  // Dun?
}

CgCache::CgCache(const llvm::Module &m,
      const DynamicInfo &di,
      const BasicFcnCFG &cfg,
      ModInfo &mod_info,
      ExtLibInfo &ext_info,
      AssumptionSet &as,
      CsCFG &cs_cfg) : cfg_(cfg) {
  // Fill local constraint info for each CFG in function call graph
  // Ensure we only visit each fcn once
  std::unordered_set<const llvm::Function *> visited;
  auto &used_info = di.used_info;

  // For each fcn
  llvm::dbgs() << "VISIT START\n";
  for (auto &fcn : m) {
    if (!used_info.isUsed(fcn) && !no_spec) {
      continue;
    }

    if (fcn.isIntrinsic() || fcn.isDeclaration()) {
      continue;
    }
    // get the SCC from my static analysis
    auto &scc_fcns = cfg_.getSCC(&fcn);

    auto it = std::begin(scc_fcns);
    auto en = std::end(scc_fcns);
    assert(it != en);

    auto first_fcn = *it;
    it = std::next(it);

    // If it has not been visited (can be visited multiple times b/c of SCCs,
    //     this resolves that)
    auto rc = visited.emplace(first_fcn);
    if (!rc.second) {
      continue;
    }

    llvm::dbgs() << " First fcn: " << first_fcn->getName() << "\n";
    // Populate the first function locally
    Cg cg(first_fcn, di, as, mod_info, ext_info, cs_cfg);

    // Combine any other functions internally
    for (; it != en; it = std::next(it)) {
      auto scc_fcn = *it;
      // Make sure we note they are visited
      llvm::dbgs() << "  visit fcn: " << scc_fcn->getName() << "\n";
      if_debug_enabled(auto scc_rc = )
        visited.emplace(scc_fcn);
      assert(scc_rc.second);

      // Parse the local function
      Cg to_merge(scc_fcn, di, as, mod_info, ext_info, cs_cfg);

      // Merge the scc components
      cg.mergeScc(to_merge);
    }

    // Insert the cg into my map
    auto fcn_id = cfg_.getId(&fcn);
    llvm::dbgs() << "Inserting fcn: " << fcn.getName() << " to " << fcn_id <<
      "\n";
    map_.emplace(fcn_id, std::move(cg));
  }
  llvm::dbgs() << "VISIT STOP\n";
}


Cg::Id Cg::getConstValue(const llvm::Constant *c) {
  if (llvm::isa<const llvm::ConstantPointerNull>(c) ||
      llvm::isa<const llvm::UndefValue>(c)) {
    return ValueMap::NullValue;
  } else if (auto gv = dyn_cast<llvm::GlobalValue>(c)) {
    // Global values should always have one def...
    // return getDef(gv);
    auto pr = vals_.getConst(gv);
    return pr.second;
  } else if (auto ce = dyn_cast<llvm::ConstantExpr>(c)) {
    switch (ce->getOpcode()) {
      case llvm::Instruction::GetElementPtr:
        {
          // Need to calc offset here...
          // But this encounters obj vs value issues...

          auto pr = vals_.getConst(c);
          auto obj_id = pr.second;
          if (pr.first) {
            // Add constraints for pr.second
            auto offs = LLVMHelper::getGEPOffs(modInfo_, *c);
            auto src_id = getDef(c->getOperand(0));
            /*
            llvm::dbgs() << "Constant: " << *c << " gets copy cons: " <<
              src_id << " -> " << obj_id << " offs: " << offs << "\n";
            */
            // Create the copy constraint
            add(ConstraintType::Copy, src_id, obj_id, offs);
          }

          // Now, do offset copy if needed...
          return obj_id;

          /*
          return ObjectMap::getOffsID(obj_id, offs);
          */
        }
      case llvm::Instruction::IntToPtr:
        // assert(0);
        // llvm::dbgs() << "getConstValue returns IntValue\n";
        return ValueMap::IntValue;
      case llvm::Instruction::PtrToInt:
        llvm::dbgs() << __LINE__ << ": getConstValue returns IntValue\n";
        // assert(0);
        return ValueMap::IntValue;
      case llvm::Instruction::BitCast:
        {
          auto pr = vals_.getConst(c);
          auto dest_id = pr.second;

          if (pr.first) {
            auto dest_val = ce->getOperand(0);

            auto src_id = getDef(dest_val);

            // Okay, if we cast from a struct to an array, we need to collapse
            //   some nodes
            // Technically this is a bitcast ptr(struct) -> ptr(array(type))
            auto dest_type = c->getType();
            auto src_type = dest_val->getType();
            auto src_np_type =
              cast<llvm::PointerType>(src_type)->getElementType();
            auto dest_np_type =
              cast<llvm::PointerType>(dest_type)->getElementType();
            // We have a cast to an array type from a struct type
            // First check to type
            auto src_st_type = dyn_cast<llvm::StructType>(src_np_type);
            if (llvm::isa<llvm::ArrayType>(dest_np_type) &&
                src_st_type != nullptr) {
              // Get each of the major subfields of the structure,
              //   and copy them into our new dest
              auto &si = modInfo_.getStructInfo(src_st_type);
              auto &offsets = si.offsets();
              for (auto offset : offsets) {
                // Add a copy from src + offs to dest
                add(ConstraintType::Copy, src_id, dest_id, offset);
              }
            } else {
              add(ConstraintType::Copy, src_id, dest_id);
            }
          }

          return dest_id;
        }
      case llvm::Instruction::Add:
        {
          auto pr = vals_.getConst(c);
          auto dest_id = pr.second;

          if (pr.first) {
            auto op0 = ce->getOperand(0);
            auto op1 = ce->getOperand(1);

            bool op0_ptr = llvm::isa<llvm::PointerType>(op0->getType());
            bool op1_ptr = llvm::isa<llvm::PointerType>(op1->getType());

            // If neither operand is a ptr copy intval
            if (!op0_ptr && !op1_ptr) {
              add(ConstraintType::Copy, ValueMap::IntValue, dest_id);
            } else {
              // If either is a ptr, add copy
              if (op0_ptr) {
                add(ConstraintType::Copy, getConstValue(op0), dest_id);
              }
              if (op1_ptr) {
                add(ConstraintType::Copy, getConstValue(op1), dest_id);
              }
            }
          }

          return dest_id;
        }
      default:
        llvm::errs() << "Const Expr not yet handled: " << *ce << "\n";
        llvm_unreachable(0);
    }
  } else if (llvm::isa<llvm::ConstantInt>(c)) {
    // llvm::dbgs() << __LINE__ << ": getConstValue returns IntValue\n";
    // assert(0);
    return ValueMap::IntValue;
  } else if (llvm::isa<llvm::ConstantFP>(c)) {
    // llvm::dbgs() << __LINE__ << ": getConstValue returns IntValue\n";
    // assert(0);
    return ValueMap::IntValue;
  } else {
    llvm::errs() << "Const Expr not yet handled: " << *c << "\n";
    llvm_unreachable("Unknown constant expr ptr");
  }
}

Cg::Id Cg::getDef(const llvm::Value *val) {
  // NOTE: Constants handles globals
  Cg::Id ret;
  if (auto c = dyn_cast<llvm::Constant>(val)) {
    ret = vals_.getRep(getConstValue(c));
  } else {
    ret = vals_.getDef(val);
  }

  return ret;
}

// Print constraint stats
void Cg::constraintStats() const {
  // AddrOf
  // Load
  // Store
  // Copy (nogep)
  // Copy (GEP)
  size_t num_addr = 0;
  size_t num_load = 0;
  size_t num_store = 0;
  size_t num_copy = 0;
  size_t num_gep = 0;
  for (auto &cons : constraints_) {
    switch (cons.type()) {
      case ConstraintType::AddressOf:
        num_addr++;
        break;
      case ConstraintType::Load:
        num_load++;
        break;
      case ConstraintType::Store:
        num_store++;
        break;
      case ConstraintType::Copy:
        {
          if (cons.offs() == 0) {
            num_copy++;
          } else {
            num_gep++;
          }
        }
        break;
      default:
        llvm_unreachable("bad constraint?");
    }
  }

  llvm::dbgs() << "Constraint stats for cg:\n";
  llvm::dbgs() << "  AddressOf: " << num_addr << "\n";
  llvm::dbgs() << "  Load: " << num_load << "\n";
  llvm::dbgs() << "  Store: " << num_store << "\n";
  llvm::dbgs() << "  Copy: " << num_copy << "\n";
  llvm::dbgs() << "  GEP: " << num_gep << "\n";
}

