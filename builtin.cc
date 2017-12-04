#include <cstdio>
#include <cstring>

#ifdef LLVM_ON_WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

/// putchard - putchar that takes a double and returns 0.
extern "C" DLLEXPORT double putchard(double x) {
  fputc((char)x, stderr);
  return 0;
}

extern "C" DLLEXPORT char my_strcmp(const char *str1, const char *str2) {
  return char(strcmp(str1, str2)); 
}
