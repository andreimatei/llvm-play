#include <cstdio>
#include <cassert>
#include <memory>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LegacyPassManager.h"

#include "ast.h"
#include "parser.h"
#include "kaleidoscpe_jit.h"

using std::vector;
using std::sprintf;
using std::string;

using llvm::Function;
using llvm::Value;
using llvm::LLVMContext;
using llvm::IRBuilder;
using llvm::Module;
using llvm::Type;
using llvm::BasicBlock;

LLVMContext TheContext;
static IRBuilder<> Builder(TheContext);
std::unique_ptr<Module> TheModule;
std::unique_ptr<llvm::legacy::FunctionPassManager> TheFPM;
static std::map<string, Value*> NamedValues;
std::unique_ptr<llvm::orc::KaleidoscopeJIT> TheJIT;
// Map of function name to the (latest) prototype declared with that name.
std::map<string, std::unique_ptr<PrototypeAST>> FunctionProtos;

// resolveFunction takes a function name and returns the corresponding Function
// from the current module (if present) or generates the function from a
// registered prototype if the function had previously been generated in
// another module.
Function* resolveFunction(const string& name) {
  Function* f = TheModule->getFunction(name);
  if (f) {
    return f;
  }
  auto it = FunctionProtos.find(name);
  if (it != FunctionProtos.end()) {
    return it->second->codegen();
  }
  return nullptr;
}

Value* NumberExprAST::codegen() {
  return llvm::ConstantFP::get(TheContext, llvm::APFloat(val));
}

Value* VariableExprAST::codegen() {
  Value* v = NamedValues[name];
  if (!v) {
    char msg[1000];
    std::sprintf(msg, "unknown variable %s", name.c_str());
    return logErrorV(msg);
  }
  return v;
}

Value* BinaryExprAST::codegen() {
  Value* l = lhs->codegen();
  Value* r = rhs->codegen();

  if (!l || !r) {
    return nullptr;
  }

  switch (op) {
  case '+':
    return Builder.CreateFAdd(l, r, "addtmp");
  case '-':
    return Builder.CreateFSub(l, r, "subtmp");
  case '*':
    return Builder.CreateFMul(l, r, "multmp");
  case '<':
    // compare unordered less than
    l = Builder.CreateFCmpULT(l, r, "cmptmp");
    // Convert bool 0/1 to double 0.0 or 1.0
    return Builder.CreateUIToFP(
        l, Type::getDoubleTy(TheContext), "booltmp");
  default:
    char msg[1000];
    sprintf(msg, "invalid bin op: %c", op);
    return logErrorV(msg);
  }
}

Value* CallExprAST::codegen() {
  // Resolve the function, either in the current module or in the list of
  // functions in all the modules.
  Function* calleeFun = resolveFunction(callee);
  if (!calleeFun) {
    char msg[1000];
    sprintf(msg, "unknown function referenced: %s", callee.c_str());
    return logErrorV(msg);
  }

  if (calleeFun->arg_size() != args.size()) {
    char msg[1000];
    sprintf(msg, "incorrect # arguments passed to %s: expected %lu, got %lu",
        callee.c_str(), calleeFun->arg_size(), args.size());
    return logErrorV(msg);
  }

  vector<Value*> argsV;
  for (const std::unique_ptr<ExprAST>& a : args) {
    Value* v = a->codegen();
    if (!v) {
      return nullptr;
    }
    argsV.push_back(v);
  }
  return Builder.CreateCall(calleeFun, argsV, "calltmp");
}

Function* PrototypeAST::codegen() const {
  // The signature of the params.
  vector<Type*> paramTypes(args.size(), Type::getDoubleTy(TheContext));
  llvm::FunctionType* ft = llvm::FunctionType::get(
      Type::getDoubleTy(TheContext), paramTypes, false /* isVarArg */);
  // Insert the function into the module.
  Function* f = Function::Create(ft, Function::ExternalLinkage, name, TheModule.get());

  // Set argument names;
  int idx = 0;
  for (auto& p : f->args()) {
    p.setName(args[idx++]);
  }
  return f;
}

Function* FunctionAST::codegen() {
  const PrototypeAST& p = *proto;
  // Transfer ownership of the prototype to the FunctionProtos map.
  FunctionProtos[p.getName()] = std::move(proto);
  Function* f = resolveFunction(p.getName());
  // We just added the function above.
  assert(f && !f->empty());

  BasicBlock *bb = BasicBlock::Create(TheContext, "entry", f);
  Builder.SetInsertPoint(bb);

  // Record the function arguments in the NamedValues map.
  NamedValues.clear();
  for (auto& arg : f->args()) {
    NamedValues[arg.getName()] = &arg;
  }

  Value* retVal = body->codegen();
  if (!retVal) {
    // In case of error in the body, we erase the function so it can be defined
    // again.
    f->eraseFromParent();
    return nullptr;
  }
  Builder.CreateRet(retVal);
  
  // Validate the generated code, checking for consistency.
  assert(!llvm::verifyFunction(*f));

  TheFPM->run(*f);

  return f;
}

Value* IfExprAST::codegen() {
  Value* condCode = condExpr->codegen();
  if (!condCode) {
    return nullptr;
  }
  // Convert condition to a bool by comparing non-equal to 0.0.
  condCode = Builder.CreateFCmpONE(
      condCode, llvm::ConstantFP::get(TheContext, llvm::APFloat(0.0)), "ifcond");

  // Get a reference to the function in which we're generating code. We'll
  // create new blocks in this function.
  Function* parentFun = Builder.GetInsertBlock()->getParent();

  // Create blocks for the then and else cases. 
  // The 'then' block is inserted at the end of the function; the others will
  // be inserted later.
  BasicBlock* thenBlock = BasicBlock::Create(TheContext, "then", parentFun);
  BasicBlock* elseBlock = BasicBlock::Create(TheContext, "if");
  BasicBlock* mergeBlock = BasicBlock::Create(TheContext, "ifcont");
  Builder.CreateCondBr(condCode, thenBlock, elseBlock);

  // Emit "then" code into a new block.
  Builder.SetInsertPoint(thenBlock);
  Value* thenCode = thenExpr->codegen();
  if (!thenCode) return nullptr;
  
  // Unconditional jump after the if/then/else block.
  Builder.CreateBr(mergeBlock);
  // Update thenBlock to whatever the current block is after recursively
  // generating code for the "then" block.
  thenBlock = Builder.GetInsertBlock();

  // Emit "else" code into a new block.
  parentFun->getBasicBlockList().push_back(elseBlock);
  Builder.SetInsertPoint(elseBlock);
  Value* elseCode = elseExpr->codegen();
  if (!elseCode) {
    return nullptr;
  }
  // Unconditional jump after the if/then/else block.
  Builder.CreateBr(mergeBlock);
  // Update thenBlock to whatever the current block is after recursively
  // generating code for the "then" block.
  elseBlock = Builder.GetInsertBlock();

  // Emit the "merge" code.
  parentFun->getBasicBlockList().push_back(mergeBlock);
  Builder.SetInsertPoint(mergeBlock);
  llvm::PHINode* phi = Builder.CreatePHI(Type::getDoubleTy(TheContext), 2, "iftmp");
  phi->addIncoming(thenCode, thenBlock);
  phi->addIncoming(elseCode, elseBlock);
  return phi;
}

Value* ForExprAST::codegen() {
  // Emit the start code first, without the loop variable in scope.
  Value* startVal = start->codegen();
  if (!startVal) return nullptr;

  // Make the new basic block for the loop header, inserting after current
  // block.
  Function* parentFun = Builder.GetInsertBlock()->getParent();
  BasicBlock* preheaderBB = Builder.GetInsertBlock(); 
  BasicBlock* loopBB = BasicBlock::Create(TheContext, "loop", parentFun);
  // Insert an explicit fall through from the current block to the LoopBB.
  Builder.CreateBr(loopBB);

  // Start insertion in LoopBB.
  Builder.SetInsertPoint(loopBB);
  // Start the PHI node with an entry for Start.
  llvm::PHINode* loopVarPhi = Builder.CreatePHI(
      Type::getDoubleTy(TheContext), 2, varName.c_str());
  loopVarPhi->addIncoming(startVal, preheaderBB);

  // Within the loop, the variable is defined equal to the PHI node. If it
  // shadows an existing variable, we have to restore it, so save it now.
  Value* oldValForLoopVar = NamedValues[varName];
  NamedValues[varName] = loopVarPhi;
  // Generate code for the body. The generated Value is ignored.
  if (!body->codegen()) return nullptr;

  // Emit the step value.
  Value* stepVal = step->codegen();
  if (!stepVal) return nullptr;
  // Increment the loop variable by the step value. This will be fed into
  // loopVarPhi later.
  Value* nextLoopVar = Builder.CreateFAdd(loopVarPhi, stepVal, "nextvar");

  // Compute and evaluate the end condition.
  Value* endCond = end->codegen();
  if (!endCond) return nullptr;
  // Convert condition to a bool by comparing non-equal to 0.0.
  endCond = Builder.CreateFCmpONE(
    endCond, llvm::ConstantFP::get(TheContext, llvm::APFloat(0.0)), "loopcond");
  
  // Create the "after loop" block and insert it.
  BasicBlock* loopEndBB = Builder.GetInsertBlock();
  BasicBlock* afterBB = BasicBlock::Create(TheContext, "afterloop", parentFun);
  // Insert the conditional branch into the end of LoopEndBB.
  Builder.CreateCondBr(endCond, loopBB, afterBB);
  // Any new code will be inserted in AfterBB.
  Builder.SetInsertPoint(afterBB);

  // Add a new entry to the PHI node for the backedge.
  loopVarPhi->addIncoming(nextLoopVar, loopEndBB);

  // Restore the unshadowed variable.
  if (oldValForLoopVar) {
    NamedValues[varName] = oldValForLoopVar;
  } else {
    NamedValues.erase(varName);
  }
  // for expr always returns 0.0.
  return llvm::Constant::getNullValue(Type::getDoubleTy(TheContext)); 
}

Value* BlockExprAST::codegen() {
  Value* val;
  for (const std::unique_ptr<ExprAST>& e : body) {
    val = e->codegen();
  }
  // If there are no expressions, we need insert a return value.
  if (body.empty()) {
    return llvm::Constant::getNullValue(Type::getDoubleTy(TheContext)); 
  }
  // return the last value.
  return val;
}
