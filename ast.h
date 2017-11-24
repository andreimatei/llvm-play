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

/// ExprAST - Base class for all expression nodes.
class ExprAST {
public:
  virtual ~ExprAST() {}
  virtual llvm::Value* codegen() = 0;  
  virtual string print() = 0;
};

/// NumberExprAST - Expression class for numeric literals like "1.0".
class NumberExprAST : public ExprAST {
public:
  NumberExprAST(double val): val(val){}
  virtual llvm::Value* codegen();
  virtual string print();

private:
  double val;
};

class VariableExprAST : public ExprAST {
private:
  std::string name;

public:
  VariableExprAST(const std::string& name) : name(name){}
  virtual llvm::Value* codegen();  
  virtual string print();
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
  virtual llvm::Value* codegen();  
  virtual string print();
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
  virtual llvm::Value* codegen();  
  virtual string print();
};

/// PrototypeAST - This class represents the "prototype" for a function,
/// which captures its name, and its argument names (thus implicitly the number
/// of arguments the function takes).
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
  unique_ptr<ExprAST> body;

public:
  FunctionAST(
      unique_ptr<PrototypeAST> proto, 
      unique_ptr<ExprAST> body) : 
    proto(move(proto)), 
    body(move(body)) {};
  llvm::Function* codegen() const;
};

#endif
