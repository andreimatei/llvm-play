#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include "parser.h"
#include "global.h"

int main() {
  InitParser();
  
  // Make the module, which holds all the code.
  TheModule = std::make_unique<llvm::Module>("my cool jit", TheContext);
  
  MainLoop();

  // Print out all of the generated code.
  TheModule->print(llvm::errs(), nullptr);

  return 0;
}

