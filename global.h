#include <memory>

#include "llvm/IR/Module.h"

extern std::unique_ptr<llvm::Module> TheModule;
extern llvm::LLVMContext TheContext;
