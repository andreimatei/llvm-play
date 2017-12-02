#ifndef AST_H
#define AST_H

#include <string>
#include <memory>
#include <utility>
#include <vector>

#include "llvm/IR/Value.h"
#include "llvm/IR/Function.h"

using std::string;
using std::vector;
using std::unique_ptr;
using std::move;

class StatementAST {
public:
  virtual ~StatementAST() {}
  // Returns true on success, false on error.
  virtual bool codegen() = 0;  
  virtual string print() = 0;
};

/// ExprAST - Base class for all expression nodes.
class ExprAST : public StatementAST{
public:
  virtual ~ExprAST() {}
  // codegenExpr is like codegen, except it return a value.
  virtual llvm::Value* codegenExpr() = 0;  
  virtual bool codegen() override;
};


/// NumberExprAST - Expression class for numeric literals like "1.0".
class NumberExprAST : public ExprAST {
public:
  NumberExprAST(double val): val(val){}
  virtual llvm::Value* codegenExpr();
  virtual string print();

private:
  double val;
};

// Variable references.
class VariableExprAST : public ExprAST {
private:
  std::string name;

public:
  VariableExprAST(const std::string& name) : name(name){}
  llvm::Value* codegenExpr() override;
  string print() override;
  string getName() const { return name; }
};

// Variable declarations.
class VariableDeclAST : public StatementAST {
private:
  std::string name;
  // Initial value. Null if the variable is to be zero-initialized.
  std::unique_ptr<ExprAST> val;

public:
  VariableDeclAST(const std::string& name, std::unique_ptr<ExprAST> val) : 
    name(name), val(std::move(val)) {}

  bool codegen() override;
  string print() override;
};

class BinaryExprAST : public ExprAST {
private:
  char op;
  std::unique_ptr<ExprAST> lhs, rhs;

public:
  BinaryExprAST(
      char op, 
      std::unique_ptr<ExprAST> lhs, 
      std::unique_ptr<ExprAST> rhs) : 
    op(op), lhs(std::move(lhs)), rhs(std::move(rhs)) {}
  llvm::Value* codegenExpr() override;
  string print() override;
};

// Function calls.
class CallExprAST : public ExprAST {
private:
  std::string callee;
  std::vector<std::unique_ptr<ExprAST> > args;

public:
  CallExprAST(
      std::string callee, 
      std::vector<std::unique_ptr<ExprAST> > args) :
    callee(callee), args(std::move(args)) {}
  llvm::Value* codegenExpr() override;
  string print() override;
};

// IfStmtAST - if/then/else.
class IfStmtAST : public StatementAST {
private:
  std::unique_ptr<ExprAST> condExpr;
  std::unique_ptr<StatementAST> thenStmt, elseStmt;

public:
  IfStmtAST(
      std::unique_ptr<ExprAST> condExpr,
      std::unique_ptr<StatementAST> thenStmt,
      std::unique_ptr<StatementAST> elseStmt) : 
    condExpr(std::move(condExpr)), thenStmt(std::move(thenStmt)), elseStmt(std::move(elseStmt)) {};

  bool codegen() override;
  string print() override;
};

// ForExprAST - for loop.
class ForStmtAST : public StatementAST {
  std::string varName;
  // As opposed to the LLVM Kaleidoskope tutorial, step will never be nil. It
  // will be an expression yielding 1.0 if its missing from the source we're
  // compiling.
  std::unique_ptr<ExprAST> start, end, step;
  std::unique_ptr<StatementAST> body;

public:
  ForStmtAST(const std::string& varName,
             std::unique_ptr<ExprAST> start,
             std::unique_ptr<ExprAST> end, 
             std::unique_ptr<ExprAST> step,
             std::unique_ptr<StatementAST> body)
    : varName(varName), start(std::move(start)), end(std::move(end)),
      step(std::move(step)), body(std::move(body)) {}

  bool codegen() override;
  string print() override;
};

// BlockStmtAST - a block (i.e. {...}).
class BlockStmtAST : public StatementAST {
  std::vector<std::unique_ptr<StatementAST>> body;

public:
  BlockStmtAST(std::vector<std::unique_ptr<StatementAST>> body)
    : body(std::move(body)) {}

  bool codegen() override;
  string print() override;
};

class ReturnStmtAST : public StatementAST {
private:
  unique_ptr<ExprAST> expr;

public:
  ReturnStmtAST(unique_ptr<ExprAST> expr) : expr(std::move(expr)) {}

  bool codegen() override;
  string print() override;
};

// PrototypeAST - This class represents the "prototype" for a function, which
// captures its name, and its argument names (thus implicitly the number of
// arguments the function takes).
class PrototypeAST {
private:
  string name;
  vector<string> args;

public:
  PrototypeAST(string name, vector<string> args) : name(name), args(std::move(args)) {}
  string getName() const { return name; }
  llvm::Function* codegen() const;
};

/// FunctionAST - This class represents a function definition itself.
class FunctionAST {
private:
  unique_ptr<PrototypeAST> proto;
  unique_ptr<StatementAST> body;

public:
  FunctionAST(
      unique_ptr<PrototypeAST> proto, 
      unique_ptr<StatementAST> body) : 
    proto(move(proto)), 
    body(move(body)) {};

  // This codegen is not const because it destroys proto. It can only be called
  // once.
  llvm::Function* codegen();
};

#endif
