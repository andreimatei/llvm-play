#include <string>
#include <sstream>

#include "ast.h"

std::string NumberExprAST::print() {
  std::ostringstream s;
  s << val;
  return s.str();
}


std::string VariableExprAST::print() {
  return name;
}

std::string BinaryExprAST::print() {
  return "(" + lhs->print() + op + rhs->print() + ")";
}

std::string CallExprAST::print() {
  return callee + "(...)";
}

std::string IfExprAST::print() {
  std::ostringstream s;
  s << "if (" <<  condExpr->print() << ") then (" << thenExpr->print() 
   << ") else (" << elseExpr->print() << ")";
  return s.str();
}

std::string ForExprAST::print() {
  std::ostringstream s;
  s << "for " << varName  << " = (" << start->print() << "), " 
    << varName << " < (" << end->print() << "), (" << step->print() << ") "
    << body->print();
  return s.str();
}

std::string BlockExprAST::print() {
  std::ostringstream s;
  s << "{\n";
  for (const std::unique_ptr<ExprAST>& e : body) {
    s << e->print() << "\n";
  }
  s << "}\n";
}
