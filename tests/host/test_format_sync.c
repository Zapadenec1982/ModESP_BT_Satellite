#include "harness.h"
#include "satlink/format_sync.h"
#include "satlink/frame.h"
#include "format_sync_cases.h"
#include <string.h>
static void verify_format_frames(void) {
    const fmt_request_t request = {0x12345678u,0x01020304u,48000,32,2};
    const fmt_ready_t ready = {request.nonce,request.generation,FMT_OK};
    uint8_t prepare[14], reply[9];
    fmt_encode(prepare,&request); fmt_encode_ready(reply,&ready);
    static const uint8_t prepare_wire[] = {0x02, 0x16, 0x0b, 0x78, 0x56, 0x34, 0x12, 0x04, 0x03, 0x02, 0x01, 0x80, 0xbb, 0x01, 0x05, 0x20, 0x02, 0x01, 0xf7, 0x00};
    static const uint8_t ready_wire[] = {0x02, 0x23, 0x09, 0x78, 0x56, 0x34, 0x12, 0x04, 0x03, 0x02, 0x01, 0x01, 0x02, 0x89, 0x00};
    uint8_t wire[SATLINK_WIRE_MAX]; size_t n=0;
    CHECK(satlink_frame_encode(FMT_PREPARE,0,prepare,sizeof prepare,wire,sizeof wire,&n));
    CHECK(n==sizeof prepare_wire+1); CHECK(wire[0]==0);
    CHECK(memcmp(wire+1,prepare_wire,sizeof prepare_wire)==0);
    CHECK(satlink_frame_encode(FMT_READY,0,reply,sizeof reply,wire,sizeof wire,&n));
    CHECK(n==sizeof ready_wire+1); CHECK(wire[0]==0);
    CHECK(memcmp(wire+1,ready_wire,sizeof ready_wire)==0);
}
const test_case_t format_tests[] = {
    {"format barrier", verify_format_sync},
    {"format wire fixtures", verify_format_frames},
};
const size_t format_test_count = sizeof format_tests / sizeof format_tests[0];
