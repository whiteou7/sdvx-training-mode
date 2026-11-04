#pragma once

#include <cmath>
#include <cstdio>

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        g_checks++;                                                  \
        if (!(c)) {                                                  \
            g_fail++;                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                            \
    } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs(double(a) - double(b)) <= (eps))
