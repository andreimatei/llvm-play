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

