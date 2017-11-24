#include <cassert>
#include <memory>
#include <string>
#include <map>

#include "llvm/Support/raw_ostream.h"

#include "lexer.h"
#include "ast.h"
#include "global.h"

using std::string;
using std::unique_ptr;
using std::vector;
using std::move;

using llvm::Value;

// CurTok/getNextToken - Provide a simple token buffer. CurTok is the current
// token the parser is looking at. getNextToken reads another token from the /
// lexer and updates CurTok with its results.
int CurTok;
int getNextToken() {
  return CurTok = gettok();
}

/// logError* - These are little helper functions for error handling.
unique_ptr<ExprAST> logError(const char* str) {
  fprintf(stderr, "logError: %s\n", str);
  return nullptr;
}

unique_ptr<PrototypeAST> logErrorP(const char* str) {
  logError(str);
  return nullptr;
}

Value* logErrorV(const char* str) {
  logError(str);
  return nullptr;
}

/// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
  auto res = std::make_unique<NumberExprAST>(NumVal); 
  getNextToken(); // advance
  return std::move(res);
}

static std::unique_ptr<ExprAST> ParseExpression();

/// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
  getNextToken(); // eat '('
  auto ret = ParseExpression();
  if (!ret) {
    return nullptr;
  }
  if (CurTok != ')') {
    return logError("missing )");
  }
  getNextToken(); // eat ')'
  return ret;
}

/// identifierexpr
///   ::= identifier
///   ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
  string id = IdentifierStr;
  getNextToken();  // eat the identifier name

  // Is this a variable reference?
  if (CurTok != '(') {
    return std::make_unique<VariableExprAST>(id);
  }

  // Continue paring a function call.
  getNextToken();  // eat '('
  vector<unique_ptr<ExprAST>> args;
  auto first = true;
  while (true) {
    if (CurTok == ')') {
      getNextToken();  // eat ')'
      break;
    }
    if (!first && CurTok != ',') {
      return logError("Expected ')' or ',' in argument list");
    }
    if (!first) {
      getNextToken();  // eat ','
    }
    first = false;
    auto arg = ParseExpression();
    if (arg == nullptr) {
      return nullptr;
    }
    args.push_back(move(arg));
  }
  return std::make_unique<CallExprAST>(id, std::move(args));
}

/// primary
///   ::= identifierexpr
///   ::= numberexpr
///   ::= parenexpr
static std::unique_ptr<ExprAST> ParsePrimary() {
  switch (CurTok) {
  default:
    return logError("unknown token when expecting an expression");
  case tok_identifier:
    return ParseIdentifierExpr();
  case tok_number:
    return ParseNumberExpr();
  case '(':
    return ParseParenExpr();
  }
}

std::map<char, int> BinopPrecedence;

int GetTokPrecedence() {
  if (!isascii(CurTok)) {
    // We use a really low precedence so that the token is always rejected by
    // operator-precedence parsing.
    return -1;
  }

    // Make sure it's a declared binop.
  int tokPrec = BinopPrecedence[CurTok];
  if (tokPrec <= 0) return -1;
  return tokPrec;
}

void InitParser() {
  // Install standard binary operators.
  // 1 is lowest precedence.
  BinopPrecedence['<'] = 10;
  BinopPrecedence['+'] = 20;
  BinopPrecedence['-'] = 20;
  BinopPrecedence['*'] = 40;  // highest.

  // Prime the first token.
  fprintf(stderr, "ready> ");
  getNextToken();
}

static std::unique_ptr<ExprAST> ParseBinOpRHS(
  int exprPrec,
  unique_ptr<ExprAST> lhs
);

/// expression
///   ::= primary binoprhs
static std::unique_ptr<ExprAST> ParseExpression() {
  auto lhs = ParsePrimary();
  if (!lhs) {
    return nullptr;
  }
  return ParseBinOpRHS(0 /* exprPrec */, std::move(lhs));
}

/// binoprhs
///   ::= ('+' primary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(
  int exprPrec,
  unique_ptr<ExprAST> lhs
) {
  // Keep consuming binops until we find one whose precedence is too low.
  // lhs keeps growing as lower and lower priority operators are encountered.
  while (1) {
    int tokPrec = GetTokPrecedence();
    if (tokPrec < exprPrec) {
      return lhs;
    }
    // We know that we're positioned on a binop, otherwise the precedence check
    // would have rejected the token.
    int binOp = CurTok;
    getNextToken();  // eat binop

    // Parse the primary expression after the binary operator.
    auto rhs = ParsePrimary();
    if (!rhs) {
      return nullptr;
    }

    // If binOp binds less tightly with RHS than the operator after RHS, let
    // the pending operator take RHS as its LHS.
    int nextPrec = GetTokPrecedence();
    if (tokPrec < nextPrec) {
      rhs = ParseBinOpRHS(tokPrec+1, std::move(rhs));
      if (!rhs) {
        return nullptr;
      }
    }

    // Merge lhs/rhs and continue parsing.
    lhs = std::make_unique<BinaryExprAST>(binOp, std::move(lhs), std::move(rhs));
  }
}

/// prototype
///   ::= id '(' id* ')'
static std::unique_ptr<PrototypeAST> ParsePrototype() {
  if (CurTok != tok_identifier)
    return logErrorP("Expected function name in prototype");

  std::string fnName = IdentifierStr;
  getNextToken();  // eat the function name

  if (CurTok != '(') {
    return logErrorP("Expected '(' in prototype");
  }

  // Read the list of argument names.
  std::vector<std::string> argNames;
  while (true) {
    int tok = getNextToken();
    if (tok != tok_identifier) {
      break;
    }
    argNames.push_back(IdentifierStr);

    tok = getNextToken();
    if (tok != ',') {
      break;
    }
  }

  if (CurTok != ')') {
    return logErrorP("Expected ')' in prototype");
  }

  getNextToken();  // eat ')'.

  return std::make_unique<PrototypeAST>(fnName, std::move(argNames));
}

/// definition ::= 'def' prototype expression
static std::unique_ptr<FunctionAST> ParseDefinition() {
  getNextToken();  // eat def.
  auto proto = ParsePrototype();
  if (!proto) return nullptr;

  if (auto e = ParseExpression())
    return std::make_unique<FunctionAST>(std::move(proto), std::move(e));
  return nullptr;
}

/// external ::= 'extern' prototype
static std::unique_ptr<PrototypeAST> ParseExtern() {
  getNextToken();  // eat extern.
  return ParsePrototype();
}

/// toplevelexpr ::= expression
static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
  if (auto e = ParseExpression()) {
    // Make an anonymous proto.
    auto proto = std::make_unique<PrototypeAST>(
        "__anon_expr", std::vector<std::string>());
    return std::make_unique<FunctionAST>(std::move(proto), std::move(e));
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Top-Level parsing
//===----------------------------------------------------------------------===//

static void HandleDefinition() {
  if (auto fnAST = ParseDefinition()) {
    if (auto* fnIR = fnAST->codegen()) {
      fprintf(stderr, "Read function definition:");
      fnIR->print(llvm::errs());
    }
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

static void HandleExtern() {
  if (auto protoAST = ParseExtern()) {
    if (auto* fnIR = protoAST->codegen()) {
      fprintf(stderr, "Read extern:");
      fnIR->print(llvm::errs());
    }
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

static void HandleTopLevelExpression() {
  // Evaluate a top-level expression into an anonymous function.
  if (auto fnAST = ParseTopLevelExpr()) {
    if (auto* fnIR = fnAST->codegen()) {
      fprintf(stderr, "Read a top-level expr:");
      fnIR->print(llvm::errs());

      // JIT the module containing the anonymous expression, keeping a handle
      // so we can free it later.
      llvm::orc::KaleidoscopeJIT::ModuleHandleT modHandle = TheJIT->addModule(
          std::move(TheModule));

      llvm::JITSymbol exprSymbol = TheJIT->findSymbol("__anon_expr");
      assert(exprSymbol && "Function not found");

      // Get the symbol's address and cast it to the right type (takes no
      // arguments, returns a double) so we can call it as a native function.
      double (*fp)() = (double (*)())(intptr_t)(*exprSymbol.getAddress());
      
      double res = fp();
      fprintf(stderr, "Evaluated to: %f\n", res);

      TheJIT->removeModule(modHandle);
      // Prepare for creating a future module.
      ResetModule(); 
    }
  } else {
    // Skip token for error recovery.
    getNextToken();
  }
}

/// top ::= definition | external | expression | ';'
void MainLoop() {
  while (1) {
    fprintf(stderr, "ready> ");
    switch (CurTok) {
    case tok_eof:
      return;
    case ';': // ignore top-level semicolons.
      getNextToken();
      break;
    case tok_def:
      HandleDefinition();
      break;
    case tok_extern:
      HandleExtern();
      break;
    default:
      HandleTopLevelExpression();
      break;
    }
  }
}

