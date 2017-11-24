#include <memory>

#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"

extern std::unique_ptr<llvm::Module> TheModule;
extern llvm::LLVMContext TheContext;
extern std::unique_ptr<llvm::legacy::FunctionPassManager> TheFPM;
