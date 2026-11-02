/*
 * test_fh_frame.c — host unit test for the fh_protocol wire format.
 *
 * This file is NOT part of any firmware build. It exists so the pure-C protocol
 * (the one piece of FHAdvanced with no hardware/framework dependency) can be
 * verified on a PC. The build machine that wrote FHAdvanced had NO C compiler,
 * so this was reasoned through by hand, not executed — run it yourself to be sure:
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -I ../components/fh_protocol/include \
 *       ../components/fh_protocol/fh_frame.c test_fh_frame.c -o test_fh_frame
 *   ./test_fh_frame
 *
 * Expected: "ALL TESTS PASSED" and exit code 0.
 */

#include "fh_frame.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); g_fail++; } \
    else         { printf("ok  : %s\n", (msg)); } \
} while (0)

/* -- CRC known-answer test ---------------------------------------------------
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xorout) of the
 * ASCII string "123456789" is the standard check value 0x29B1. */
static void test_crc_kat(void)
{
    const uint8_t s[] = { '1','2','3','4','5','6','7','8','9' };
    uint16_t crc = fh_crc16(s, sizeof(s));
    printf("    CRC16(\"123456789\") = 0x%04X (expect 0x29B1)\n", crc);
    CHECK(crc == 0x29B1, "CRC16/CCITT-FALSE known-answer 0x29B1");
}

/* -- capture callback bookkeeping ------------------------------------------- */
typedef struct {
    int      count;
    fh_type_t last_type;
    uint8_t  last_len;
    uint8_t  last_payload[FH_MAX_PAYLOAD];
} capture_ctx_t;

static void on_frame(fh_type_t type, const uint8_t *pl, uint8_t len, void *user)
{
    capture_ctx_t *c = (capture_ctx_t *)user;
    c->count++;
    c->last_type = type;
    c->last_len  = len;
    if (len && pl) memcpy(c->last_payload, pl, len);
}

/* -- encode/decode round trip ------------------------------------------------ */
static void test_roundtrip(void)
{
    fh_heartbeat_t hb = {0};
    hb.src_id = FH_SRC_C5_WIFI5;
    hb.uptime_s = 1234;
    hb.capture_count = 99;
    hb.dropped_keys = 3;
    hb.task_alive = FH_TASK_CAPTURE | FH_TASK_GPS;
    hb.free_heap_kb = 212;

    uint8_t frame[FH_MAX_FRAME];
    size_t n = fh_frame_encode(FH_TYPE_HEARTBEAT, (const uint8_t *)&hb,
                               (uint8_t)sizeof(hb), frame, sizeof(frame));
    CHECK(n == FH_HDR_LEN + sizeof(hb) + FH_CRC_LEN, "heartbeat framed length");
    CHECK(frame[0] == FH_SYNC0 && frame[1] == FH_SYNC1, "SYNC bytes present");
    CHECK(frame[2] == FH_TYPE_HEARTBEAT, "TYPE byte");
    CHECK(frame[3] == sizeof(hb), "LEN byte");

    fh_decoder_t d;
    fh_decoder_init(&d);
    capture_ctx_t ctx = {0};
    fh_decoder_push(&d, frame, n, on_frame, &ctx);
    CHECK(ctx.count == 1, "one frame decoded");
    CHECK(ctx.last_type == FH_TYPE_HEARTBEAT, "decoded TYPE matches");
    CHECK(ctx.last_len == sizeof(hb), "decoded LEN matches");
    CHECK(memcmp(ctx.last_payload, &hb, sizeof(hb)) == 0, "payload round-trips");
    CHECK(d.good_frames == 1 && d.crc_errors == 0, "decoder counters clean");
}

/* -- resync after leading garbage + a corrupted frame ----------------------- */
static void test_resync(void)
{
    /* Build two valid PING frames. */
    uint8_t p1[FH_MAX_FRAME], p2[FH_MAX_FRAME];
    uint8_t seqa = 7, seqb = 8;
    size_t n1 = fh_frame_encode(FH_TYPE_PING, &seqa, 1, p1, sizeof(p1));
    size_t n2 = fh_frame_encode(FH_TYPE_PING, &seqb, 1, p2, sizeof(p2));

    fh_decoder_t d;
    fh_decoder_init(&d);
    capture_ctx_t ctx = {0};

    /* Leading junk, then frame1 with ONE byte flipped (CRC should fail), then
     * clean frame2. Decoder must drop the bad one and still deliver frame2. */
    uint8_t junk[] = { 0x00, 0xFF, 0xA5, 0x13, 0x5A };  /* noise incl. a fake sync */
    fh_decoder_push(&d, junk, sizeof(junk), on_frame, &ctx);

    uint8_t bad[FH_MAX_FRAME];
    memcpy(bad, p1, n1);
    bad[4] ^= 0xFF;   /* corrupt the payload so CRC mismatches */
    fh_decoder_push(&d, bad, n1, on_frame, &ctx);
    fh_decoder_push(&d, p2, n2, on_frame, &ctx);

    CHECK(ctx.count == 1, "exactly one good frame after junk+corruption");
    CHECK(ctx.last_type == FH_TYPE_PING && ctx.last_payload[0] == seqb,
          "the surviving frame is the clean frame2");
    CHECK(d.crc_errors >= 1, "decoder recorded the CRC failure");
}

/* -- byte-at-a-time feed (simulates slow UART) ------------------------------ */
static void test_byte_dribble(void)
{
    fh_gps_fix_t fix = {0};
    fix.gps_time = 1760000000u;
    fix.lat = 405000000;  /* 40.5 deg x 1e7 */
    fix.lon = -1052000000;
    fix.sats = 9;
    fix.fix_qual = 1;

    uint8_t frame[FH_MAX_FRAME];
    size_t n = fh_frame_encode(FH_TYPE_GPS_FIX, (const uint8_t *)&fix,
                               (uint8_t)sizeof(fix), frame, sizeof(frame));
    fh_decoder_t d;
    fh_decoder_init(&d);
    capture_ctx_t ctx = {0};
    for (size_t i = 0; i < n; ++i) {
        fh_decoder_push(&d, &frame[i], 1, on_frame, &ctx);  /* one byte at a time */
    }
    CHECK(ctx.count == 1, "frame decodes when fed one byte at a time");
    CHECK(memcmp(ctx.last_payload, &fix, sizeof(fix)) == 0, "gps_fix payload intact");
}

/* -- SYNC bytes INSIDE the payload must not break framing ------------------- *
 * Framing is length-driven after the header, so a payload that itself contains
 * the 0xA5 0x5A sync sequence (and a bare 0xA5) must round-trip without the
 * decoder false-triggering a new frame on those bytes. This is the adversarial
 * case the original test suite did not cover. */
static void test_sync_in_payload(void)
{
    /* A LoRa-relay payload whose data deliberately embeds A5 5A and a lone A5. */
    uint8_t payload[] = {
        0x01,             /* seq   */
        0x00,             /* flags */
        0xA5, 0x5A,       /* the full SYNC sequence, as data                   */
        0x5A, 0xA5,       /* reversed                                          */
        0xA5, 0xA5, 0x5A, /* double-A5 then 5A (the primed-sync edge case)     */
        0xDE, 0xAD, 0xBE  /* trailing data                                     */
    };
    uint8_t frame[FH_MAX_FRAME];
    size_t n = fh_frame_encode(FH_TYPE_LORA_RELAY_TX, payload,
                               (uint8_t)sizeof(payload), frame, sizeof(frame));
    CHECK(n > 0, "frame with SYNC bytes in payload encodes");

    fh_decoder_t d;
    fh_decoder_init(&d);
    capture_ctx_t ctx = {0};
    fh_decoder_push(&d, frame, n, on_frame, &ctx);

    CHECK(ctx.count == 1, "exactly one frame despite SYNC bytes in payload");
    CHECK(ctx.last_len == sizeof(payload), "payload length preserved");
    CHECK(memcmp(ctx.last_payload, payload, sizeof(payload)) == 0,
          "payload with embedded SYNC round-trips byte-for-byte");
    CHECK(d.good_frames == 1 && d.crc_errors == 0, "no false frame / CRC error");
}

int main(void)
{
    printf("fh_protocol host tests\n----------------------\n");
    test_crc_kat();
    test_roundtrip();
    test_resync();
    test_byte_dribble();
    test_sync_in_payload();
    printf("----------------------\n");
    if (g_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
