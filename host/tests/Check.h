#pragma once

#include <cstdio>

inline int g_failures = 0;

#define CHECK(expr)                                                                                          \
    do                                                                                                       \
    {                                                                                                        \
        if (!(expr))                                                                                         \
        {                                                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                  \
            ++g_failures;                                                                                    \
        }                                                                                                    \
    } while (0)

void RunProtocolTests();
void RunEdidTests();
void RunAnnexBTests();
void RunPointerShapeTests();
void RunRotationTests();
void RunTouchTests();
void RunAdbTests();
void RunAdaptiveBitrateTests();
