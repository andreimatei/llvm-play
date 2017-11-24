#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Support/TargetSelect.h"

#include "parser.h"
#include "global.h"
#include "kaleidoscpe_jit.h"

void InitLLVM() {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  TheJIT = std::make_unique<llvm::orc::KaleidoscopeJIT>();

  ResetModule();
}

void ResetModule() {
  // Open a new module.
  TheModule = std::make_unique<llvm::Module>("my cool jit", TheContext);
  TheModule->setDataLayout(TheJIT->getTargetMachine().createDataLayout());

  // Create a new pass manager attached to it.
  TheFPM = std::make_unique<llvm::legacy::FunctionPassManager>(TheModule.get());

  // Do simple "peephole" optimizations and bit-twiddling optzns.
  TheFPM->add(llvm::createInstructionCombiningPass());
  // Reassociate expressions.
  TheFPM->add(llvm::createReassociatePass());
  // Eliminate Common SubExpressions.
  TheFPM->add(llvm::createGVNPass());
  // Simplify the control flow graph (deleting unreachable blocks, etc).
  TheFPM->add(llvm::createCFGSimplificationPass());

  TheFPM->doInitialization();
}

int main() {
  InitParser();
  InitLLVM();
  
  MainLoop();

  // Print out all of the generated code.
  TheModule->print(llvm::errs(), nullptr);

  return 0;
}

