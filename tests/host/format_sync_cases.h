/* Identical test scenarios compiled as C on the satellite and C++ on the host. */
#include <string.h>
static void verify_format_sync(void) {
    const uint8_t golden[14]={0x78,0x56,0x34,0x12,4,3,2,1,0x80,0xbb,0,0,32,2};
    fmt_request_t r={0x12345678u,0x01020304u,48000,32,2}, decoded={0};
    uint8_t data[14]; fmt_encode(data,&r);
    CHECK(memcmp(data,golden,14)==0);
    CHECK(fmt_decode(golden,14,&decoded)); CHECK(fmt_same(&r,&decoded));
    for(size_t n=0;n<14;++n) CHECK(!fmt_decode(golden,n,&decoded));
    CHECK(!fmt_decode(golden,15,&decoded));
    memset(data,0,sizeof data); CHECK(!fmt_decode(data,14,&decoded));
    const uint8_t ack_vector[9]={0x78,0x56,0x34,0x12,4,3,2,1,0};
    fmt_ready_t ack={r.nonce,r.generation,FMT_OK}, got={0};
    uint8_t wire_ack[9]; fmt_encode_ready(wire_ack,&ack);
    CHECK(memcmp(wire_ack,ack_vector,9)==0);
    CHECK(fmt_decode_ready(wire_ack,9,&got));
    for(size_t n=0;n<9;++n) CHECK(!fmt_decode_ready(wire_ack,n,&got));
    CHECK(!fmt_decode_ready(wire_ack,10,&got));
    wire_ack[8]=2; CHECK(!fmt_decode_ready(wire_ack,9,&got));

    fmt_barrier_t b; fmt_begin(&b,r,true);
    CHECK(!fmt_can_resume(&b,10000)); // queueing isn't transmission, timeout isn't ACK
    fmt_sent(&b,100); b.configured=true;
    CHECK(!fmt_can_resume(&b,1000)); // host is delayed by more than 500 ms
    fmt_ack(&b,&ack); CHECK(fmt_can_resume(&b,1000));
    fmt_begin(&b,r,true); fmt_ack(&b,&ack); fmt_sent(&b,100);
    CHECK(!fmt_can_resume(&b,150)); // receiver ready isn't transmitter configured
    b.configured=true;
    CHECK(!fmt_can_resume(&b,149)); CHECK(fmt_can_resume(&b,150));
    CHECK(!fmt_can_resume_source(&b,150,1,2)); // next decoder format queued but not handled
    CHECK(fmt_can_resume_source(&b,150,2,2));
    fmt_sent(&b,140); CHECK(fmt_can_resume(&b,150)); // retry doesn't reset silence

    r.generation++; fmt_begin(&b,r,true); fmt_sent(&b,0); b.configured=true;
    fmt_ack(&b,&ack); CHECK(!fmt_can_resume(&b,10000)); // stale transaction
    ack.generation=r.generation; ack.nonce++; fmt_ack(&b,&ack);
    CHECK(!fmt_can_resume(&b,10000)); // stale reboot nonce
    ack.nonce=r.nonce; ack.status=FMT_REFUSED; fmt_ack(&b,&ack);
    CHECK(!fmt_can_resume(&b,10000));
    ack.status=FMT_OK; fmt_ack(&b,&ack); CHECK(!fmt_can_resume(&b,10000));
    // Refusal is terminal for this transaction, even if a delayed success arrives.

    r.generation++; fmt_begin(&b,r,true); fmt_sent(&b,UINT32_MAX-20u);
    b.configured=true; ack.generation=r.generation; fmt_ack(&b,&ack);
    CHECK(!fmt_can_resume(&b,28)); CHECK(fmt_can_resume(&b,29)); // uptime wraps
    fmt_begin(&b,r,false); fmt_sent(&b,10); b.configured=true;
    CHECK(!fmt_can_resume(&b,59)); CHECK(fmt_can_resume(&b,60)); // legacy timed path
    b.request.channels=1; CHECK(!fmt_can_resume(&b,100));
    b.request.channels=2; b.request.bits=24; CHECK(!fmt_can_resume(&b,100));
    b.request.bits=16; b.request.rate_hz=0; CHECK(!fmt_can_resume(&b,100));

    CHECK(!fmt_ack_capable(2,FMT_HOST_CAP)); CHECK(!fmt_ack_capable(3,0));
    CHECK(fmt_ack_capable(3,FMT_HOST_CAP));
    CHECK_EQ(fmt_slot_bits(16,true,true),16); CHECK_EQ(fmt_slot_bits(32,true,true),32);
    CHECK_EQ(fmt_slot_bits(32,false,true),16); CHECK_EQ(fmt_slot_bits(16,false,true),16);
    CHECK_EQ(fmt_slot_bits(16,true,false),32); // legacy width policy unchanged
    CHECK_EQ(fmt_slot_bits(32,true,false),32);
}
