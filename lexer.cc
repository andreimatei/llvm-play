#include <string>

#include "lexer.h"

std::string IdentifierStr; // Filled in if tok_identifier
double NumVal;             // Filled in if tok_number

/// gettok - Return the next token from standard input.
int gettok() {
  // static because a previous call may leave a character not consumed.
  static int lastCh = ' ';

  // Skip white space.
  while (isspace(lastCh)) {
    lastCh = getchar();
  }

  if (isalpha(lastCh)) {  // identifier: [a-zA-Z][a-zA-Z0-9]*
    IdentifierStr = lastCh;

    while (isalnum(lastCh = getchar())) {
      IdentifierStr += lastCh;
    }
    if (IdentifierStr == "def") {
      return tok_def;
    }
    if (IdentifierStr == "extern") {
      return tok_extern;
    }
    if (IdentifierStr == "if")
      return tok_if;
    if (IdentifierStr == "then")
      return tok_then;
    if (IdentifierStr == "else")
      return tok_else;
    if (IdentifierStr == "for")
      return tok_for;
    if (IdentifierStr == "return")
      return tok_return;
    // !!! did I get rid of "in"?
    if (IdentifierStr == "in")
      return tok_in;
    return tok_identifier;
  }

  if (lastCh == '{') {
    lastCh = getchar();
    return tok_block_open;
  }
  if (lastCh == '}') {
    lastCh = getchar();
    return tok_block_close;
  }
  if (lastCh == ';') {
    lastCh = getchar();
    return tok_semi;
  }

  if (isdigit(lastCh) || lastCh == '.') { // Number: [0-9.]+
    std::string num;
    do {
      num += lastCh;
      lastCh = getchar();
    } while (isdigit(lastCh) || lastCh == '.');
    NumVal = strtod(num.c_str(), nullptr /* endptr */);
    return tok_number;
  }
  
  if (lastCh == '#') {
    // Comment until end of line.
    do {
      lastCh = getchar();
    } while (lastCh != EOF && lastCh != '\n' && lastCh != '\r');
    if (lastCh != EOF) {
      return gettok();
    }
  }
  
  // Check for end of file.  Don't eat the EOF.
  if (lastCh == EOF) {
    return tok_eof;
  }

  // Otherwise, just return the character as its ascii value.
  int thisCh = lastCh;
  lastCh = getchar();
  return thisCh;
}
