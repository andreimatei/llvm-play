#include <string>
#include <fstream>
#include <streambuf>

#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/IR/Verifier.h"

#include "parser.h"
#include "global.h"
#include "kaleidoscpe_jit.h"

using std::string;

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

  // Promote allocas to registers.
  TheFPM->add(llvm::createPromoteMemoryToRegisterPass());
  // Do simple "peephole" optimizations and bit-twiddling optzns.
  TheFPM->add(llvm::createInstructionCombiningPass());
  // Reassociate expressions.
  TheFPM->add(llvm::createReassociatePass());
  // Eliminate Common SubExpressions.
  TheFPM->add(llvm::createGVNPass());
  // Simplify the control flow graph (deleting unreachable blocks, etc).
  TheFPM->add(llvm::createCFGSimplificationPass());
  // !!!
  TheFPM->add(llvm::createVerifierPass(true /* fatalErrors */));

  TheFPM->doInitialization();
}

class StringReader {
private:
  string prog;
  size_t idx;
public:
  StringReader(const string& prog) : prog(prog), idx(0) {}
  ~StringReader() {}

  char GetNextChar() {
    if (idx < prog.length()) {
      return prog[idx++];
    }
    return EOF;
  }
};

StringReader* SR;

void CompileStr(const std::string& prog) {
  // TODO(andrei): sr leaks
  SR = new StringReader(prog);
  //auto sr = new StringReader(prog);
  std::function<char()> nextCh = []() {
    return SR->GetNextChar();
  };
  GetNextChar = nextCh;
}

string FileToString(const string& path) {
  std::ifstream t(path);
  string str((std::istreambuf_iterator<char>(t)),
              std::istreambuf_iterator<char>());
  fprintf(stderr, "program: %s\n", str.c_str());
  return str;
}

void RunProgMain() {
  llvm::JITSymbol exprSymbol = TheJIT->findSymbol("entry");
  assert(exprSymbol && "Function not found");

  // Get the symbol's address and cast it to the right type (takes no
  // arguments, returns a double) so we can call it as a native function.
  // !!! double (*fp)() = (double (*)())(intptr_t)(*exprSymbol.getAddress());
  // double res = fp();
  // fprintf(stderr, "Evaluated to: %f\n", res);
  char (*fp)(char*,char*) = (char(*)(char*, char*))(intptr_t)(*exprSymbol.getAddress());
  char* k = (char*)malloc(100);
  char* v = (char*)malloc(100);
  v[0] = 'x';
  v[1] = 'b';
  v[2] = 'c';
  v[3] = 0;
  char res = fp(k, v);
  fprintf(stderr, "Evaluated to: %d\n", int(res));
}

int main() {
  GetNextChar = &getchar;

  string progStr = FileToString("prog.in");
  CompileStr(progStr);

  InitParser();
  InitLLVM();
  
  MainLoop();

  // Print out all of the generated code.
  TheModule->print(llvm::errs(), nullptr);

  RunProgMain();

  return 0;
}

