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

// Variable name to space where the value is stored.
static std::map<string, llvm::AllocaInst*> NamedValues;

std::unique_ptr<llvm::orc::KaleidoscopeJIT> TheJIT;
// Map of function name to the (latest) prototype declared with that name.
std::map<string, std::unique_ptr<PrototypeAST>> FunctionProtos;

bool ExprAST::codegen() {
  auto* val = codegenExpr();
  return val != nullptr;
}

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

// Create an alloca instruction in the entry block of the function. This is
// used for mutable variables etc.
static llvm::AllocaInst* createEntryBlockAlloca(
    Function* fun,
    const std::string& varName) {
  IRBuilder<> TmpB(
      &fun->getEntryBlock(), fun->getEntryBlock().begin());
  return TmpB.CreateAlloca(Type::getDoubleTy(TheContext), 0, varName.c_str());
}

Value* NumberExprAST::codegenExpr() {
  return llvm::ConstantFP::get(TheContext, llvm::APFloat(val));
}

Value* VariableExprAST::codegenExpr() {
  Value* v = NamedValues[name];
  if (!v) {
    char msg[1000];
    std::sprintf(msg, "unknown variable %s", name.c_str());
    return logErrorV(msg);
  }
  // Load the value from memory.
  return Builder.CreateLoad(v, name.c_str());
}

Value* BinaryExprAST::codegenExpr() {
  // The assignment operator is a special case because we don't want to emit
  // code for the LHS.
  if (op == '=') {
    // Check that the lhs is a variable reference.
    VariableExprAST* varAST = dynamic_cast<VariableExprAST*>(lhs.get());
    if (!varAST) {
      return logErrorV("destination of assignment must be a variable");
    }
    // Codegen the RHS.
    Value* r = rhs->codegenExpr();

    // Lookup the name.
    Value* var = NamedValues[varAST->getName()];
    if (!var) {
      char msg[1000];
      sprintf(msg, "unknown varable: %s", varAST->getName().c_str());
      return logErrorV(msg);
    }
    Builder.CreateStore(r, var);
    // Return the result of the rhs.
    return r;
  }

  Value* l = lhs->codegenExpr();
  Value* r = rhs->codegenExpr();

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

Value* CallExprAST::codegenExpr() {
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
    Value* v = a->codegenExpr();
    if (!v) {
      return nullptr;
    }
    argsV.push_back(v);
  }
  return Builder.CreateCall(calleeFun, argsV, "calltmp");
}

Function* PrototypeAST::codegen() const {
  // The signature of the params.
  fprintf(stderr, "!!! PrototypeAST::codegen - args: %lu\n", args.size());
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
  fprintf(stderr, "!!! FunctionAST::codegen 1\n");
  const PrototypeAST& p = *proto;
  // Transfer ownership of the prototype to the FunctionProtos map.
  FunctionProtos[p.getName()] = std::move(proto);
  Function* f = resolveFunction(p.getName());
  fprintf(stderr, "!!! FunctionAST::codegen 2.\n");
  // We just added the function above.
  assert(f && !f->empty());

  BasicBlock *bb = BasicBlock::Create(TheContext, "entry", f);
  Builder.SetInsertPoint(bb);
  fprintf(stderr, "!!! FunctionAST::codegen 3\n");

  // Record the function arguments in the NamedValues map.
  NamedValues.clear();
  for (auto& arg : f->args()) {
    fprintf(stderr, "!!! FunctionAST::codegen 4\n");
    // Create an alloca for this variable.
    llvm::AllocaInst* alloca = createEntryBlockAlloca(f, arg.getName());
    // Store the initial value into the alloca.
    Builder.CreateStore(&arg, alloca);

    // Add the variable to the symbol table.
    NamedValues[arg.getName()] = alloca;
  }
  fprintf(stderr, "!!! FunctionAST::codegen 5\n");

  bool success = body->codegen();
  fprintf(stderr, "!!! FunctionAST::codegen 6\n");
  if (!success) {
    // In case of error in the body, we erase the function so it can be defined
    // again.
    f->eraseFromParent();
    return nullptr;
  }
  fprintf(stderr, "!!! FunctionAST::codegen 7 name: %s\n", p.getName().c_str());
  bool xxx = (p.getName() != "magic");
  fprintf(stderr, "!!! FunctionAST::codegen 77 t: %d \n", xxx);

  if (p.getName() != "magic") {
    // !!! now the return value is in the generated code, but I should assert that.
    // Builder.CreateRet(retVal);
  } else {
    fprintf(stderr, "!!! FunctionAST::codegen 8\n");
    Function* parentFun = Builder.GetInsertBlock()->getParent();
    BasicBlock* b2 = BasicBlock::Create(TheContext, "b2", parentFun);
    BasicBlock* b3 = BasicBlock::Create(TheContext, "b3", parentFun);

    Value* arg0 = NamedValues[f->args().begin()->getName()];
    auto condCode = Builder.CreateFCmpONE(
        arg0, llvm::ConstantFP::get(TheContext, llvm::APFloat(0.0)), "ifcond");
    Builder.CreateCondBr(condCode, b2, b3);

    Builder.SetInsertPoint(b2);
    auto bogusRet = llvm::ConstantFP::get(TheContext, llvm::APFloat(1.0));
    Builder.CreateRet(bogusRet);

    Builder.SetInsertPoint(b3);
    bogusRet = llvm::ConstantFP::get(TheContext, llvm::APFloat(2.0));
    Builder.CreateRet(bogusRet);
  }
  
  // Validate the generated code, checking for consistency.
  fprintf(stderr, "!!! FunctionAST::codegen 9\n");
  assert(!llvm::verifyFunction(*f));
  fprintf(stderr, "!!! FunctionAST::codegen 10\n");
  // !!!
  f->print(llvm::errs());

  TheFPM->run(*f);
  fprintf(stderr, "!!! FunctionAST::codegen 11\n");

  return f;
}

bool IfStmtAST::codegen() {
  Value* condCode = condExpr->codegenExpr();
  if (!condCode) return false; 
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
  if (!thenStmt->codegen()) return false;
  
  // Unconditional jump after the if/then/else block.
  Builder.CreateBr(mergeBlock);
  // Update thenBlock to whatever the current block is after recursively
  // generating code for the "then" block.
  thenBlock = Builder.GetInsertBlock();

  // Emit "else" code into a new block.
  parentFun->getBasicBlockList().push_back(elseBlock);
  Builder.SetInsertPoint(elseBlock);
  if (!elseStmt->codegen()) return false;
  
  // Unconditional jump after the if/then/else block.
  Builder.CreateBr(mergeBlock);
  // Update thenBlock to whatever the current block is after recursively
  // generating code for the "then" block.
  elseBlock = Builder.GetInsertBlock();

  // Emit the "merge" code.
  parentFun->getBasicBlockList().push_back(mergeBlock);
  Builder.SetInsertPoint(mergeBlock);
  return true;
}

bool ReturnStmtAST::codegen() {
  Value* retVal = expr->codegenExpr();
  if (retVal == nullptr) return false;
  Builder.CreateRet(retVal);
  return true;
}

// Output for-loop as:
//   var = alloca double
//   ...
//   start = startexpr
//   store start -> var
//   goto loop
// loop:
//   <code for body>
//   step = stepexpr
//   endcond = endexpr
//   curvar = load var
//   nextvar = curvar + step
//   store nextvar -> var
//   br endcond, loop, afterloop
// afterloop:
bool ForStmtAST::codegen() {
  Function* fun = Builder.GetInsertBlock()->getParent();
  llvm::AllocaInst* alloca = createEntryBlockAlloca(fun, varName);

  // Emit the start code first, without the loop variable in scope.
  Value* startVal = start->codegenExpr();
  if (!startVal) return false;

  Builder.CreateStore(startVal, alloca);

  // Make the new basic block for the loop header, inserting after current
  // block.
  Function* parentFun = Builder.GetInsertBlock()->getParent();
  BasicBlock* loopBB = BasicBlock::Create(TheContext, "loop", parentFun);
  // Insert an explicit fall through from the current block to the LoopBB.
  Builder.CreateBr(loopBB);

  // Start insertion in LoopBB.
  Builder.SetInsertPoint(loopBB);

  // Within the loop, the variable is defined equal to the variable we just
  // introduced. If it shadows an existing variable, we have to restore it, so
  // save it now.
  llvm::AllocaInst* oldValForLoopVar = NamedValues[varName];
  NamedValues[varName] = alloca;
  // Generate code for the body. The generated Value is ignored.
  if (!body->codegen()) return false;

  // Emit the step value.
  Value* stepVal = step->codegenExpr();
  if (!stepVal) return false;
  // Reload, increment, and restore the alloca. This handles the case where the
  // body of the loop mutates the variable.
  Value* curLoopVarVal = Builder.CreateLoad(alloca);
  Value* nextLoopVar = Builder.CreateFAdd(curLoopVarVal, stepVal, "nextvar");
  Builder.CreateStore(nextLoopVar, alloca);

  // Compute and evaluate the end condition.
  Value* endCond = end->codegenExpr();
  if (!endCond) return false;
  // Convert condition to a bool by comparing non-equal to 0.0.
  endCond = Builder.CreateFCmpONE(
    endCond, llvm::ConstantFP::get(TheContext, llvm::APFloat(0.0)), "loopcond");
  
  // Create the "after loop" block and insert it.
  BasicBlock* afterLoopBB = BasicBlock::Create(TheContext, "afterloop", parentFun);
  // Insert the conditional branch into the end of afterLoopBB.
  Builder.CreateCondBr(endCond, loopBB, afterLoopBB);
  
  // Any new code will be inserted in AfterBB.
  Builder.SetInsertPoint(afterLoopBB);

  // Restore the unshadowed variable.
  if (oldValForLoopVar) {
    NamedValues[varName] = oldValForLoopVar;
  } else {
    NamedValues.erase(varName);
  }
  return true;
}

bool BlockStmtAST::codegen() {
  for (const std::unique_ptr<StatementAST>& e : body) {
    if (!e->codegen()) return false;
  }
  return true;
}

bool VariableDeclAST::codegen() {
  Function* fun = Builder.GetInsertBlock()->getParent();

  // Emit the initializer before adding the variable to scope, this prevents
  // the initializer from referencing the variable itself, and permits stuff
  // like this:
  //  var a = 1
  //  {
  //    var a = a + 1  # refers to outer 'a'.
  //  }
  Value* initVal;
  if (val) {
    initVal = val->codegenExpr();
    if (initVal == nullptr) return false;
  } else {
    initVal = llvm::ConstantFP::get(TheContext, llvm::APFloat(0.0));
  }
  // Allocate space for the variable on the heap. 
  llvm::AllocaInst *alloca = createEntryBlockAlloca(fun, name);
  // Store the initial value in the allocated memory.
  Builder.CreateStore(initVal, alloca);
  
  // Remember this binding.
  // TODO(andrei): When do we remove the variable from scope?
  NamedValues[name] = alloca;
  return true;
}
