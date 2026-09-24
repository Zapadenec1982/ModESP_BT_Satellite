/*
 * The endpoint re-registration machine, walked without a stack or a board.
 *
 * What is checked: a host whose deciding capabilities differ from the ones the
 * endpoints were registered from gets a deinit, then an init, only while no
 * phone is attached; a refusal is retried after a pause and never spins; the
 * first registration and unrelated capability bits leave the machine alone.
 */

#include <stdio.h>

#include "harness.h"
#include "endpoint_policy.h"

int g_checks, g_failures;

#define RELOCK  SATLINK_HOST_CAP_RELOCK
#define K96     SATLINK_HOST_CAP_96KHZ
#define WIDE    SATLINK_HOST_CAP_WIDE
#define COVER   SATLINK_HOST_CAP_COVER
#define ALL     0x1Fu       /* every codec this build has: SBC, AAC, aptX, aptX HD, LHDC V5 */
#define NO_LHDC 0x0Fu

static void test_boot_is_inert(void)
{
    ep_fsm_t f = {0};
    CHECK_EQ(f.state, EP_STATE_BOOT);
    CHECK_EQ(ep_poll(&f, RELOCK | K96, ALL, true, 100), EP_ACT_NONE);
    CHECK_EQ(ep_on_prof_state(&f, false), EP_ACT_NONE);    /* the first INIT_SUCCESS */
    CHECK_EQ(f.state, EP_STATE_BOOT);
    CHECK_EQ(ep_on_prof_state(&f, true), EP_ACT_NONE);     /* nobody asked for a deinit */
    CHECK_EQ(f.state, EP_STATE_BOOT);
}

static void test_late_wider_host(void)
{
    ep_fsm_t f = {0};
    ep_registered(&f, 0, ALL);                                   /* cold start, no HELLO */
    CHECK_EQ(f.state, EP_STATE_IDLE);
    CHECK_EQ(f.decided, 0u);

    CHECK_EQ(ep_poll(&f, 0, ALL, true, 1000), EP_ACT_NONE);      /* still no host */
    CHECK_EQ(ep_poll(&f, WIDE | COVER, ALL, true, 1000), EP_ACT_NONE);  /* bits that do not decide */
    CHECK(!ep_stale(&f, WIDE | COVER, ALL));

    CHECK(ep_stale(&f, RELOCK | K96 | WIDE, ALL));
    CHECK_EQ(ep_poll(&f, RELOCK | K96 | WIDE, ALL, true, 2000), EP_ACT_DEINIT);
    CHECK_EQ(f.state, EP_STATE_DEINIT_WAIT);
    CHECK_EQ(ep_poll(&f, RELOCK | K96 | WIDE, ALL, true, 2010), EP_ACT_NONE);   /* one at a time */

    CHECK_EQ(ep_on_prof_state(&f, false), EP_ACT_NONE);     /* a stray INIT_SUCCESS changes nothing */
    CHECK_EQ(f.state, EP_STATE_DEINIT_WAIT);

    CHECK_EQ(ep_on_prof_state(&f, true), EP_ACT_INIT);
    CHECK_EQ(f.state, EP_STATE_UNREGISTERED);
    ep_init_sent(&f, RELOCK | K96 | WIDE, ALL);
    CHECK_EQ(f.state, EP_STATE_INIT_WAIT);
    CHECK_EQ(f.decided, (unsigned)(RELOCK | K96));
    CHECK_EQ(ep_poll(&f, RELOCK | K96 | WIDE, ALL, true, 2100), EP_ACT_NONE);

    CHECK_EQ(ep_on_prof_state(&f, false), EP_ACT_NONE);
    CHECK_EQ(f.state, EP_STATE_IDLE);
    CHECK(!ep_stale(&f, RELOCK | K96 | WIDE, ALL));
    CHECK_EQ(ep_poll(&f, RELOCK | K96 | WIDE, ALL, true, 3000), EP_ACT_NONE);
}

static void test_deferred_until_idle(void)
{
    ep_fsm_t f = {0};
    ep_registered(&f, 0, ALL);
    for (int i = 0; i < 100; i++) {
        CHECK_EQ(ep_poll(&f, RELOCK, ALL, false, 1000 + i), EP_ACT_NONE);   /* phone attached */
    }
    CHECK_EQ(f.state, EP_STATE_IDLE);
    CHECK_EQ(ep_poll(&f, RELOCK, ALL, true, 5000), EP_ACT_DEINIT);           /* it left */
}

static void test_narrower_host_counts(void)
{
    ep_fsm_t f = {0};
    ep_registered(&f, RELOCK | K96, ALL);
    CHECK(ep_stale(&f, RELOCK, ALL));
    CHECK(ep_stale(&f, 0, ALL));
    CHECK_EQ(ep_poll(&f, RELOCK, ALL, true, 10), EP_ACT_DEINIT);
    CHECK_EQ(ep_on_prof_state(&f, true), EP_ACT_INIT);
    ep_init_sent(&f, RELOCK, ALL);
    CHECK_EQ(f.decided, (unsigned)RELOCK);
}

static void test_refused_deinit_backs_off(void)
{
    ep_fsm_t f = {0};
    ep_registered(&f, 0, ALL);
    CHECK_EQ(ep_poll(&f, RELOCK, ALL, true, 1000), EP_ACT_DEINIT);
    ep_action_failed(&f, 1000);
    CHECK_EQ(f.state, EP_STATE_IDLE);                       /* old endpoints still stand */
    for (uint32_t t = 1000; t < 1000 + EP_RETRY_MS; t += 100) {
        CHECK_EQ(ep_poll(&f, RELOCK, ALL, true, t), EP_ACT_NONE);
    }
    CHECK_EQ(ep_poll(&f, RELOCK, ALL, true, 1000 + EP_RETRY_MS), EP_ACT_DEINIT);
}

static void test_refused_init_is_retried(void)
{
    ep_fsm_t f = {0};
    ep_registered(&f, 0, ALL);
    CHECK_EQ(ep_poll(&f, RELOCK, ALL, true, 1000), EP_ACT_DEINIT);
    CHECK_EQ(ep_on_prof_state(&f, true), EP_ACT_INIT);
    ep_action_failed(&f, 1200);
    CHECK_EQ(f.state, EP_STATE_UNREGISTERED);
    CHECK_EQ(ep_poll(&f, RELOCK, ALL, true, 1300), EP_ACT_NONE);
    /* Not idle is impossible with no endpoints, and the caps no longer matter:
     * anything is better than nothing to connect to. */
    CHECK_EQ(ep_poll(&f, 0, ALL, false, 1200 + EP_RETRY_MS), EP_ACT_INIT);
    ep_init_sent(&f, 0, ALL);
    CHECK_EQ(ep_on_prof_state(&f, false), EP_ACT_NONE);
    CHECK_EQ(f.state, EP_STATE_IDLE);
    CHECK_EQ(f.decided, 0u);
}

static void test_retry_clock_wraps(void)
{
    ep_fsm_t f = {0};
    ep_registered(&f, 0, ALL);
    CHECK_EQ(ep_poll(&f, RELOCK, ALL, true, 0xFFFFF000u), EP_ACT_DEINIT);
    ep_action_failed(&f, 0xFFFFF000u);                      /* retry lands past the wrap */
    CHECK_EQ(ep_poll(&f, RELOCK, ALL, true, 0xFFFFFF00u), EP_ACT_NONE);
    CHECK_EQ(ep_poll(&f, RELOCK, ALL, true, 0xFFFFF000u + EP_RETRY_MS), EP_ACT_DEINIT);
}

/* Protocol 1.8: the host switches a codec off. Same machine, same moment. */
static void test_codecs_reregister_when_idle(void)
{
    ep_fsm_t f = {0};
    ep_registered(&f, RELOCK, ALL);
    CHECK_EQ(f.codecs, ALL);
    CHECK(!ep_stale(&f, RELOCK, ALL));
    CHECK(ep_stale(&f, RELOCK, NO_LHDC));

    /* A phone is attached: the change waits, however long. */
    for (int i = 0; i < 50; i++) {
        CHECK_EQ(ep_poll(&f, RELOCK, NO_LHDC, false, 1000 + i), EP_ACT_NONE);
    }
    CHECK_EQ(ep_poll(&f, RELOCK, NO_LHDC, true, 2000), EP_ACT_DEINIT);
    CHECK_EQ(ep_on_prof_state(&f, true), EP_ACT_INIT);
    ep_init_sent(&f, RELOCK, NO_LHDC);
    CHECK_EQ(f.codecs, NO_LHDC);
    CHECK_EQ(ep_on_prof_state(&f, false), EP_ACT_NONE);
    CHECK_EQ(f.state, EP_STATE_IDLE);
    CHECK_EQ(ep_poll(&f, RELOCK, NO_LHDC, true, 3000), EP_ACT_NONE);

    /* And back on: a codec coming back is a change too. */
    CHECK_EQ(ep_poll(&f, RELOCK, ALL, true, 4000), EP_ACT_DEINIT);

    /* Caps and codecs changing together cost one registration, not two. */
    ep_fsm_t g = {0};
    ep_registered(&g, 0, ALL);
    CHECK_EQ(ep_poll(&g, RELOCK | K96, NO_LHDC, true, 10), EP_ACT_DEINIT);
    CHECK_EQ(ep_on_prof_state(&g, true), EP_ACT_INIT);
    ep_init_sent(&g, RELOCK | K96, NO_LHDC);
    CHECK_EQ(ep_on_prof_state(&g, false), EP_ACT_NONE);
    CHECK(!ep_stale(&g, RELOCK | K96, NO_LHDC));
}

int main(void)
{
    test_boot_is_inert();
    test_codecs_reregister_when_idle();
    test_late_wider_host();
    test_deferred_until_idle();
    test_narrower_host_counts();
    test_refused_deinit_backs_off();
    test_refused_init_is_retried();
    test_retry_clock_wraps();
    printf("Endpoint re-registration: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
