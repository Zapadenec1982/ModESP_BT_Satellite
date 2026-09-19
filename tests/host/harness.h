/*
 * A test harness small enough to read in one sitting. No framework, because the
 * thing under test is a few hundred lines of pure C with no dependencies, and a
 * framework would be the larger half of the project.
 */

#pragma once

#include <stddef.h>
#include <stdio.h>

extern int g_checks;
extern int g_failures;

#define CHECK(cond)                                                             \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        g_checks++;                                                             \
        long long _a = (long long)(a), _b = (long long)(b);                     \
        if (_a != _b) {                                                         \
            g_failures++;                                                       \
            printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n",                   \
                   __FILE__, __LINE__, #a, #b, _a, _b);                         \
        }                                                                       \
    } while (0)

typedef struct {
    const char *name;
    void      (*fn)(void);
} test_case_t;

extern const test_case_t frame_tests[];
extern const size_t      frame_test_count;

extern const test_case_t message_tests[];
extern const size_t      message_test_count;

extern const test_case_t golden_tests[];
extern const size_t      golden_test_count;

extern const test_case_t format_tests[];
extern const size_t format_test_count;
