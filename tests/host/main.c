#include <stdio.h>

#include "harness.h"

int g_checks;
int g_failures;

static void run(const char *group, const test_case_t *tests, size_t count)
{
    printf("%s\n", group);
    for (size_t i = 0; i < count; i++) {
        int before = g_failures;
        tests[i].fn();
        printf("  %-26s %s\n", tests[i].name, (g_failures == before) ? "ok" : "FAILED");
    }
}

int main(void)
{
    run("format negotiation", format_tests, format_test_count);
    run("framing", frame_tests, frame_test_count);
    run("messages", message_tests, message_test_count);
    run("golden vectors from the host", golden_tests, golden_test_count);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
