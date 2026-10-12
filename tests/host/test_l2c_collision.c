/* Replay a paging collision through the production l2c_link_hci_conn_comp.
 *
 * The function comes out of the generated l2c_link.c, the link states out of
 * l2c_int.h and the HCI codes out of hcidefs.h; the LCB, the CCB and everything
 * the function calls are stubs that only record. Bench log 12.10.2026: with the
 * phone's link up as handle 0x80, a Connection Complete for the same phone came
 * 1.3 s later as handle 0x81 with Connection Already Exists, most likely the end
 * of the board's own auto-reconnect page after both sides paged at once. Stock
 * tore the live link down for it, without an HCI_Disconnect, so the controller
 * kept an ACL the host no longer knew. Patch 0011 ignores that event. Not a
 * radio test.
 */
#include <stdint.h>
#include <string.h>
#include "harness.h"
int g_checks, g_failures;

typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint8_t BOOLEAN;
typedef UINT8 BD_ADDR[6];
typedef UINT8 DEV_CLASS[3];
typedef UINT8 tBTM_BD_NAME[249];
typedef uintptr_t TIMER_PARAM_TYPE;   /* UINT32 on the 32-bit target */
#define TRUE 1
#define FALSE 0
#define CLASSIC_BT_INCLUDED TRUE
#define BD_ADDR_LEN 6
#include "l2c_defs.inc"

#define BT_TRANSPORT_BR_EDR 1
#define BTM_BLI_PAGE_DONE_EVT 2
#define L2CAP_EXTENDED_FEATURES_INFO_TYPE 2
#define L2CEVT_LP_CONNECT_CFM 0
#define L2CEVT_LP_CONNECT_CFM_NEG 1
#define BTU_TTYPE_L2CAP_LINK 2
#define BTU_TTYPE_L2CAP_LINK_RETRY 113
#define L2CAP_ECHO_RSP_TOUT 30
#define L2CAP_LINK_STARTUP_TOUT 60
#define L2C_LP_CONN_RETRY_DELAY_TOUT 1
#define L2CAP_MAX_RECONNECT_ON_COLLISION 5
#define L2CAP_TRACE_WARNING(...) ((void)0)
#define L2CAP_TRACE_ERROR(...) ((void)0)

typedef struct { TIMER_PARAM_TYPE param; UINT16 event; int running; } TIMER_LIST_ENT;
typedef struct { UINT8 status; BD_ADDR bd_addr; } tL2C_CONN_INFO;
typedef struct ccb { struct ccb *p_next_ccb; } tL2C_CCB;
typedef struct {
    tL2C_LINK_STATE link_state;
    UINT16 handle;
    UINT8 link_role, disc_reason, br_edr_create_con_retries;
    BOOLEAN is_bonding;
    void *p_echo_rsp_cb;
    struct { tL2C_CCB *p_first_ccb; } ccb_queue;
    TIMER_LIST_ENT timer_entry, retry_timer_entry;
} tL2C_LCB;
typedef struct { DEV_CLASS dev_class; tBTM_BD_NAME sec_bd_name; } tBTM_SEC_DEV_REC;
static struct { UINT16 btm_def_link_super_tout; } btm_cb;

static tL2C_LCB s_lcb;
static tL2C_CCB s_ccb;
static BD_ADDR s_phone = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
static int s_disc_comps, s_releases, s_create_conns, s_acl_created, s_cfm, s_cfm_neg;
static UINT16 s_disc_handle;

static void btm_acl_update_busy_level(int event) { (void)event; }
static tL2C_LCB *l2cu_find_lcb_by_bd_addr(BD_ADDR bda, int transport) {
    (void)transport;
    return memcmp(bda, s_phone, sizeof(BD_ADDR)) == 0 ? &s_lcb : NULL;
}
static BOOLEAN l2c_link_hci_disc_comp(UINT16 handle, UINT8 reason) {
    (void)reason;
    s_disc_comps++;
    s_disc_handle = handle;
    s_lcb.link_state = LST_DISCONNECTING;
    return TRUE;
}
static void l2cu_send_peer_info_req(tL2C_LCB *p_lcb, UINT16 info_type) { (void)p_lcb; (void)info_type; }
static tBTM_SEC_DEV_REC *btm_find_dev(BD_ADDR bda) { (void)bda; return NULL; }
static void btm_acl_created(BD_ADDR bda, UINT8 *dc, UINT8 *name, UINT16 handle, UINT8 role, int transport) {
    (void)bda; (void)dc; (void)name; (void)handle; (void)role; (void)transport;
    s_acl_created++;
}
static void BTM_SetLinkSuperTout(BD_ADDR bda, UINT16 timeout) { (void)bda; (void)timeout; }
static BOOLEAN l2cu_start_post_bond_timer(UINT16 handle) { (void)handle; return FALSE; }
static void l2c_process_held_packets(BOOLEAN timed_out) { (void)timed_out; }
static void btu_stop_timer(TIMER_LIST_ENT *p_tle) { p_tle->running = 0; }
static void btu_start_timer(TIMER_LIST_ENT *p_tle, UINT16 type, UINT32 timeout) {
    (void)timeout;
    p_tle->event = type;
    p_tle->running = 1;
}
/* The CCB stays queued on a negative confirm, as l2c_csm.c keeps it for a collision. */
static void l2c_csm_execute(tL2C_CCB *p_ccb, UINT16 event, void *p_data) {
    (void)p_ccb; (void)p_data;
    if (event == L2CEVT_LP_CONNECT_CFM) s_cfm++;
    if (event == L2CEVT_LP_CONNECT_CFM_NEG) s_cfm_neg++;
}
static void l2cu_send_peer_echo_req(tL2C_LCB *p_lcb, UINT8 *p_data, UINT16 data_len) {
    (void)p_lcb; (void)p_data; (void)data_len;
}
static BOOLEAN l2cu_lcb_disconnecting(void) { return FALSE; }
static void l2cu_release_lcb(tL2C_LCB *p_lcb) { (void)p_lcb; s_releases++; }
static BOOLEAN l2cu_create_conn(tL2C_LCB *p_lcb, int transport) { (void)p_lcb; (void)transport; s_create_conns++; return TRUE; }

#include "l2c_conn_comp.inc"

/* An LCB in `state` with one channel on it, as the stack holds a link whose A2DP
 * signalling channel is open or being opened. */
static void reset_case(tL2C_LINK_STATE state, UINT16 handle) {
    memset(&s_lcb, 0, sizeof s_lcb);
    memset(&s_ccb, 0, sizeof s_ccb);
    s_lcb.link_state = state;
    s_lcb.handle = handle;
    s_lcb.ccb_queue.p_first_ccb = &s_ccb;
    s_disc_comps = s_releases = s_create_conns = s_acl_created = s_cfm = s_cfm_neg = 0;
    s_disc_handle = HCI_INVALID_HANDLE;
}

/* The bench case: the phone's link is up as 0x80, our page ends as 0x81 with 0x0B. */
static void test_late_page_leaves_live_link(void) {
    reset_case(LST_CONNECTED, 0x80);
    l2c_link_hci_conn_comp(HCI_ERR_CONNECTION_EXISTS, 0x81, s_phone);
    CHECK_EQ(s_disc_comps, 0);
    CHECK_EQ(s_releases, 0);
    CHECK_EQ(s_create_conns, 0);
    CHECK_EQ(s_cfm_neg, 0);
    CHECK_EQ(s_lcb.link_state, LST_CONNECTED);
    CHECK_EQ(s_lcb.handle, 0x80);
    CHECK(s_lcb.ccb_queue.p_first_ccb == &s_ccb);
}

/* The same refusal by Command Status (btu_hcif.c passes HCI_INVALID_HANDLE). */
static void test_refused_command_leaves_live_link(void) {
    reset_case(LST_CONNECTED, 0x80);
    l2c_link_hci_conn_comp(HCI_ERR_CONNECTION_EXISTS, HCI_INVALID_HANDLE, s_phone);
    CHECK_EQ(s_disc_comps, 0);
    CHECK_EQ(s_lcb.link_state, LST_CONNECTED);
    CHECK_EQ(s_lcb.handle, 0x80);
}

/* A completion for another address is not this link's either. */
static void test_unknown_peer_is_ignored(void) {
    BD_ADDR other = {1, 2, 3, 4, 5, 6};
    reset_case(LST_CONNECTED, 0x80);
    l2c_link_hci_conn_comp(HCI_ERR_CONNECTION_EXISTS, 0x81, other);
    CHECK_EQ(s_disc_comps, 0);
    CHECK_EQ(s_lcb.link_state, LST_CONNECTED);
}

/* The ordinary path is untouched: our page comes up while we wait for it. */
static void test_connecting_link_comes_up(void) {
    reset_case(LST_CONNECTING, HCI_INVALID_HANDLE);
    s_lcb.retry_timer_entry.running = 1;
    s_lcb.br_edr_create_con_retries = 2;
    l2c_link_hci_conn_comp(HCI_SUCCESS, 0x80, s_phone);
    CHECK_EQ(s_lcb.link_state, LST_CONNECTED);
    CHECK_EQ(s_lcb.handle, 0x80);
    CHECK_EQ(s_acl_created, 1);
    CHECK_EQ(s_cfm, 1);
    CHECK_EQ(s_lcb.br_edr_create_con_retries, 0);
    CHECK_EQ(s_lcb.retry_timer_entry.running, 0);
    CHECK_EQ(s_disc_comps, 0);
}

/* IDF 5.5.5's collision retry is untouched: 0x0B while still connecting keeps the
 * LCB and its channel and arms the back-off for another Create_Connection. */
static void test_connecting_collision_still_retries(void) {
    reset_case(LST_CONNECTING, HCI_INVALID_HANDLE);
    l2c_link_hci_conn_comp(HCI_ERR_CONNECTION_EXISTS, 0x81, s_phone);
    CHECK_EQ(s_lcb.link_state, LST_CONNECTING);
    CHECK_EQ(s_cfm_neg, 1);
    CHECK_EQ(s_lcb.br_edr_create_con_retries, 1);
    CHECK_EQ(s_lcb.retry_timer_entry.running, 1);
    CHECK_EQ(s_lcb.retry_timer_entry.event, BTU_TTYPE_L2CAP_LINK_RETRY);
    CHECK_EQ(s_releases, 0);
    CHECK_EQ(s_disc_comps, 0);
}

/* Like AOSP's, the exception is for a connected link only: a link already on its
 * way down still takes stock's teardown. */
static void test_disconnecting_link_still_torn_down(void) {
    reset_case(LST_DISCONNECTING, 0x80);
    l2c_link_hci_conn_comp(HCI_ERR_CONNECTION_EXISTS, 0x81, s_phone);
    CHECK_EQ(s_disc_comps, 1);
    CHECK_EQ(s_disc_handle, 0x80);
}

int main(void) {
    test_late_page_leaves_live_link();
    test_refused_command_leaves_live_link();
    test_unknown_peer_is_ignored();
    test_connecting_link_comes_up();
    test_connecting_collision_still_retries();
    test_disconnecting_link_still_torn_down();
    printf("L2CAP collision replay: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
