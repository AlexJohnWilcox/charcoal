#pragma once
#include <cstdio>

extern int g_failures;

#define CHECK(c)                                                       \
  do {                                                                 \
    if (!(c)) {                                                        \
      ++g_failures;                                                    \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
    }                                                                  \
  } while (0)
