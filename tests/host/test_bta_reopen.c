/* Replay the observed CLOSE -> CONFIG -> OPEN -> START ordering through the
 * production BTA dispatcher/tables and production BTC incoming-config case.
 * Transport, decoder validation and unrelated actions are stubs, not a radio test.
 */
#include <stdint.h>
#include <string.h>
#include "harness.h"
int g_checks, g_failures;
typedef uint8_t UINT8;
typedef uint16_t UINT16;
#define TRUE 1
#define FALSE 0
#define BTA_ID_AV 1
#define BTA_SYS_EVT_START(id) ((id) << 8)
#define BTA_AV_CA_INCLUDED 0
#define BTA_AV_SINK_INCLUDED TRUE
#define AVDT_REPORTING FALSE
#define BTA_AV_NUM_STRS 2
#define APPL_TRACE_EVENT(...) ((void)0)
#define APPL_TRACE_VERBOSE(...) ((void)0)
#define BTA_AV_FIRST_SSM_EVT BTA_AV_API_OPEN_EVT
#define BTA_AV_LAST_SSM_EVT BTA_AV_ACP_CONNECT_EVT
#include "bta_tables.inc"

typedef struct { struct { UINT8 bd_addr[6]; } incoming; } tBTA_AV;
typedef struct { struct { UINT8 bd_addr[6]; } str_msg; int codec; } tBTA_AV_DATA;
typedef struct scb tBTA_AV_SCB;
typedef void (*action_fn)(tBTA_AV_SCB *, tBTA_AV_DATA *);
struct scb { UINT8 state, coll_mask, hndl; int force_incoming; action_fn *p_act_tbl; };
static struct { tBTA_AV_SCB *p_scb[BTA_AV_NUM_STRS]; void (*p_cback)(int,tBTA_AV *); } bta_av_cb;
enum { BTC_AV_STATE_IDLE, BTC_AV_STATE_OPENING, BTC_AV_STATE_OPENED, BTC_AV_STATE_STARTED };
enum { BTA_AV_INCOMING_CFG_EVT = 1 };
static struct { struct { UINT8 address[6]; } peer_bda; int sm_handle; } btc_av_cb;
static int btc_state, notices, configs, starts, accepted_codec, rejected;
static int configured;
#define bdcpy(dst,src) memcpy((dst),(src),6)
static void btc_sm_change_state(int handle,int state) { (void)handle; btc_state=state; }
static void upper_event(int event,tBTA_AV *p_data) {
    notices++;
    if (btc_state==BTC_AV_STATE_IDLE) {
#include "btc_incoming.inc"
    }
}
static void ignore_action(tBTA_AV_SCB *scb,tBTA_AV_DATA *data) { (void)scb; (void)data; }
static void close_action(tBTA_AV_SCB *scb,tBTA_AV_DATA *data) {
    (void)scb; (void)data; btc_state=BTC_AV_STATE_IDLE; configured=0;
    memset(&btc_av_cb.peer_bda,0,sizeof btc_av_cb.peer_bda);
}
static void config_action(tBTA_AV_SCB *p_scb,tBTA_AV_DATA *p_data) {
#include "bta_notify.inc"
}
static void config_response(tBTA_AV_SCB *scb,tBTA_AV_DATA *data) {
    (void)scb;
    if (btc_state==BTC_AV_STATE_OPENING || btc_state==BTC_AV_STATE_OPENED) {
        configs++; accepted_codec=data->codec; configured=1;
    }
}
static void open_action(tBTA_AV_SCB *scb,tBTA_AV_DATA *data) {
    (void)scb; (void)data;
    if (btc_state==BTC_AV_STATE_OPENING) btc_state=BTC_AV_STATE_OPENED;
}
static void start_action(tBTA_AV_SCB *scb,tBTA_AV_DATA *data) {
    (void)scb; (void)data;
    if (btc_state==BTC_AV_STATE_OPENED && configured) { btc_state=BTC_AV_STATE_STARTED; starts++; }
}
static void reject_action(tBTA_AV_SCB *scb,tBTA_AV_DATA *data) { (void)scb; (void)data; rejected++; }
#include "bta_dispatch.inc"
static action_fn action_table[BTA_AV_NUM_SACTIONS];
static tBTA_AV_SCB scb, other;
static tBTA_AV_DATA request={.str_msg={{1,2,3,4,5,6}}};
static void reset_case(int state) {
    memset(&scb,0,sizeof scb); memset(&other,0,sizeof other);
    memset(&bta_av_cb,0,sizeof bta_av_cb); memset(&btc_av_cb,0,sizeof btc_av_cb);
    for (int i=0;i<BTA_AV_NUM_SACTIONS;i++) action_table[i]=ignore_action;
    action_table[BTA_AV_STR_CLOSED]=close_action;
    action_table[BTA_AV_CONFIG_IND]=config_action;
    action_table[BTA_AV_SETCONFIG_RSP]=config_response;
    action_table[BTA_AV_STR_OPENED]=open_action;
    action_table[BTA_AV_START_OK]=start_action;
    action_table[BTA_AV_SETCONFIG_REJ]=reject_action;
    scb.state=state; scb.p_act_tbl=action_table;
    bta_av_cb.p_scb[0]=&scb; bta_av_cb.p_cback=upper_event;
    btc_state=BTC_AV_STATE_IDLE; notices=configs=starts=rejected=configured=0; accepted_codec=-1;
}
static void finish_stream(int codec) {
    request.codec=codec;
    bta_av_ssm_execute(&scb,BTA_AV_STR_CONFIG_IND_EVT,&request);
    CHECK(btc_state==BTC_AV_STATE_OPENING);
    CHECK(memcmp(btc_av_cb.peer_bda.address,request.str_msg.bd_addr,6)==0);
    CHECK(!scb.force_incoming);
    bta_av_ssm_execute(&scb,BTA_AV_CI_SETCONFIG_OK_EVT,&request);
    CHECK(configured); CHECK(accepted_codec==codec);
    bta_av_ssm_execute(&scb,BTA_AV_STR_OPEN_OK_EVT,&request);
    bta_av_ssm_execute(&scb,BTA_AV_STR_START_OK_EVT,&request);
    CHECK(btc_state==BTC_AV_STATE_STARTED);
}
int main(void) {
    /* Both arrival orders: close already handled, or config queued while OPEN.
     * Codec bytes are opaque to the real dispatcher; test SBC and vendor identities.
     */
    for(int codec=0;codec<=8;codec+=8) {
        reset_case(BTA_AV_OPEN_SST); btc_state=BTC_AV_STATE_OPENED;
        CHECK(!scb.force_incoming); // CONFIG arrival saw OPEN, so the early flag was not set
        bta_av_ssm_execute(&scb,BTA_AV_STR_CLOSE_EVT,&request);
        CHECK(scb.state==BTA_AV_INIT_SST); CHECK(btc_state==BTC_AV_STATE_IDLE);
        finish_stream(codec); CHECK(notices==1); CHECK(configs==1); CHECK(starts==1);
        bta_av_ssm_execute(&scb,BTA_AV_STR_CLOSE_EVT,&request);
        finish_stream(codec==0?8:0); CHECK(notices==2); CHECK(starts==2);
        reset_case(BTA_AV_INCOMING_SST); scb.force_incoming=TRUE; // existing early path
        finish_stream(codec); CHECK(notices==1);
    }
    reset_case(BTA_AV_INIT_SST);
    other.state=BTA_AV_INCOMING_SST; other.coll_mask=7; bta_av_cb.p_scb[1]=&other;
    finish_stream(8); CHECK(other.state==BTA_AV_INIT_SST); CHECK(other.coll_mask==0);
    reset_case(BTA_AV_INCOMING_SST); btc_state=BTC_AV_STATE_OPENING;
    bdcpy(btc_av_cb.peer_bda.address,request.str_msg.bd_addr);
    finish_stream(0); CHECK(notices==0); // ordinary connection already notified by PENDING
    reset_case(BTA_AV_OPEN_SST); btc_state=BTC_AV_STATE_OPENED;
    bta_av_ssm_execute(&scb,BTA_AV_STR_CONFIG_IND_EVT,&request);
    CHECK(rejected==1); CHECK(notices==0); CHECK(!scb.force_incoming);
    reset_case(BTA_AV_CLOSING_SST);
    bta_av_ssm_execute(&scb,BTA_AV_STR_CONFIG_IND_EVT,&request);
    CHECK(rejected==1); CHECK(notices==0); CHECK(!scb.force_incoming);
    reset_case(BTA_AV_INIT_SST);
    bta_av_ssm_execute(&scb,BTA_AV_STR_OPEN_OK_EVT,&request);
    bta_av_ssm_execute(&scb,BTA_AV_STR_START_OK_EVT,&request);
    CHECK(btc_state==BTC_AV_STATE_IDLE); CHECK(starts==0); // no blind OPEN/START recovery
    bta_av_ssm_execute(&scb,BTA_AV_FIRST_SSM_EVT-1,&request);
    bta_av_ssm_execute(NULL,BTA_AV_STR_CONFIG_IND_EVT,&request);
    CHECK(notices==0);
    printf("BTA reopen replay: %d checks, %d failures\n",g_checks,g_failures);
    return g_failures?1:0;
}
