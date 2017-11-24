#ifndef PARSER_H
#define PARSER_H

#include "llvm/IR/Value.h"

void InitParser();
void MainLoop();
llvm::Value* logErrorV(const char* str);

#endif
