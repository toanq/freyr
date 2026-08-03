#pragma once

// Minimal assertion-based test support -- no framework dependency, since
// these tests run on the host (plain g++), not through ESP-IDF/idf.py. Each
// test suite is a function returning its own failure count; tests/main.cc
// sums them and reports a final pass/fail.

#include <cstdio>

inline int g_test_failures_in_current_suite = 0;

#define TEST_CHECK(cond)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
            g_test_failures_in_current_suite++;                                             \
        }                                                                                    \
    } while (0)

#define TEST_CHECK_EQ(actual, expected)                                                                        \
    do {                                                                                                       \
        auto actual_value = (actual);                                                                         \
        auto expected_value = (expected);                                                                      \
        if (!(actual_value == expected_value)) {                                                               \
            std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #actual, #expected);                     \
            g_test_failures_in_current_suite++;                                                                \
        }                                                                                                       \
    } while (0)

// Call at the start of each RunXxxTests() function.
#define TEST_SUITE_BEGIN() g_test_failures_in_current_suite = 0

// Call at the end -- returns this suite's failure count.
#define TEST_SUITE_END() return g_test_failures_in_current_suite
