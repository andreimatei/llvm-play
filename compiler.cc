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

  llvm::BasicBlock *bb = llvm::BasicBlock::Create(TheContext, "entry", f);
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


