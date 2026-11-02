/*
 * main.c — FHAdvanced-REMIX CC1101 sub-GHz blade, native Raspberry Pi Pico SDK.
 *
 * UNVERIFIED FIRST DRAFT — reasoned-not-compiled, NOT bench-tested (project
 * rule). This is the pilot pico-sdk port of the arduino-pico cc1101 blade.
 *
 * TWO SPI BLOCKS (decision 2026-11-01):
 *   core0 — HUB LINK: spi0 SLAVE, MODE 1, the BENCH-PROVEN GPS pump copied
 *           verbatim (full SPI0 reset + 8-byte preload in the idle gap +
 *           paired RX/TX push to 261 bytes + CS-edge realign). Decodes inbound
 *           PING->PONG and FH_OD_CC1101_REQ; presents the staged response for
 *           the hub's next read.
 *   core1 — RADIO: spi1 MASTER to the CC1101 (vetted ELECHOUSE SDK driver).
 *           Runs the fired on-demand op (LISTEN/RX_ONE/RAW_OOK/REPLAY) — a
 *           capture can take seconds, so it lives OFF the pump, like the GPS
 *           blade's core split. Stages the RESP for core0 to clock out.
 *
 * RADIO CORE (capture ring + decode + transmit) is carried from the arduino
 * blade, adapted to SDK timing (time_us_32 / gpio IRQ). The ELECHOUSE calls go
 * through the vetted SDK driver (ELECHOUSE_CC1101_SRC_DRV.*).
 *
 * KNOWN RISKS at EOF. The hub-link pump is the proven part; the radio core and
 * the GDO0 capture-IRQ on spi1 are the bench surface.
 */
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/mutex.h"
#include "pico/multicore.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/resets.h"
#include "hardware/sync.h"     /* __dmb() cross-core memory barrier */

#include "fh_frame.h"
#include "fh_ondemand.h"
#include "fh_pico_pins_sdk.h"

/* ---- C shim over the C++ ELECHOUSE driver ----
 * main.c is compiled as C and MUST NOT include the driver's C++ class header
 * (a C compiler cannot parse `class`). The driver is reached ONLY through these
 * extern "C" wrappers, defined in cc_shim.cpp. */
extern bool  cc_present(void);
extern void  cc_init(float mhz);
extern void  cc_enter_rx(void);
extern int   cc_rssi(void);
extern void  cc_set_freq(float mhz);
extern void  cc_tx_raw(unsigned long value, int bits, int proto, int repeats);

#define SPI_HUB   spi0            /* hub link (slave) — same as GPS */

/* ======================================================================== */
/* RADIO CORE — OOK capture ring + decode (carried from the arduino blade,    */
/* adapted to SDK). GDO0 edge IRQ timestamps half-pulses; decode matches the  */
/* RCSwitch-style protocol table.                                             */
/* ======================================================================== */
#define MAX_EDGES 512
static volatile uint32_t g_edgeDur[MAX_EDGES];
static volatile uint8_t  g_edgeLvl[MAX_EDGES];
static volatile uint16_t g_isrHead = 0;
static volatile uint32_t g_lastEdgeUs = 0;
static volatile bool     g_capturing = false;

static uint32_t g_dur[MAX_EDGES];
static uint8_t  g_lvl[MAX_EDGES];
static int      g_edgeCount = 0;

/* last decode (for REPLAY) */
static unsigned long g_lastValue = 0;
static int           g_lastBits  = 0;
static int           g_lastProto = 0;
static bool          g_haveCapture = false;

typedef struct {
    uint16_t pulseLength;
    uint8_t  syncHigh, syncLow, zeroHigh, zeroLow, oneHigh, oneLow;
    bool     inverted;
} RCProtocol;

static const RCProtocol RC_PROTOCOLS[] = {
    {   0,  0,  0, 0,  0, 0,  0, false },
    { 350,  1, 31, 1,  3, 3,  1, false },
    { 650,  1, 10, 1,  2, 2,  1, false },
    { 100, 30, 71, 4, 11, 9,  6, false },
    { 380,  1,  6, 1,  3, 3,  1, false },
    { 500,  6, 14, 1,  2, 2,  1, false },
    { 450, 23,  1, 1,  2, 2,  1, true  },
    { 150,  2, 62, 1,  6, 6,  1, false },
    { 200,  3,130, 7, 16, 3, 16, false },
    { 200,130,  7,16,  7,16,  3, true  },
    { 365, 18,  1, 3,  1, 1,  3, true  },
    { 270, 36,  1, 1,  2, 2,  1, true  },
    { 320, 36,  1, 1,  2, 2,  1, true  },
};
static const int NUM_RC_PROTOCOLS = (int)(sizeof(RC_PROTOCOLS)/sizeof(RC_PROTOCOLS[0]));

static void gdo0_isr(uint gpio, uint32_t events) {
    (void)gpio; (void)events;
    uint32_t now = time_us_32();
    if (!g_capturing) { g_lastEdgeUs = now; return; }
    uint32_t delta = now - g_lastEdgeUs;
    g_lastEdgeUs = now;
    uint8_t levelNow = (uint8_t)gpio_get(FH_CC_GDO0);
    uint8_t heldLevel = levelNow ? 0 : 1;
    uint16_t h = g_isrHead;
    if (h < MAX_EDGES) { g_edgeDur[h] = delta; g_edgeLvl[h] = heldLevel; g_isrHead = h + 1; }
}

static void capture_start(void) {
    g_isrHead = 0;
    g_lastEdgeUs = time_us_32();
    __dmb();                 /* publish the ring reset before capturing goes live */
    g_capturing = true;
    /* NOTE: GDO0's GPIO IRQ is enabled ONCE on core0 at startup (see main()),
     * NOT here. On the RP2040 a GPIO IRQ is serviced only on the core that
     * enabled it, and the bank callback (cs_gpio_isr) is registered on core0 —
     * so GDO0 MUST be enabled on core0 too. capture_start/stop run on core1, so
     * they only flip g_capturing; gdo0_isr already no-ops when !g_capturing, so
     * the always-enabled GDO0 edges are simply ignored outside a window. */
}
static void capture_stop(void) {
    g_capturing = false;
}

static int capture_snapshot(void) {
    __dmb();                 /* see the core0 ISR's final edge writes */
    int n = (int)g_isrHead;
    if (n > MAX_EDGES) n = MAX_EDGES;
    for (int i = 0; i < n; i++) { g_dur[i] = g_edgeDur[i]; g_lvl[i] = g_edgeLvl[i]; }
    g_edgeCount = n;
    return n;
}

static int capture_frame(uint32_t windowMs) {
    capture_start();
    uint32_t start = to_ms_since_boot(get_absolute_time());
    const uint32_t IDLE_GAP_US = 12000;
    while ((to_ms_since_boot(get_absolute_time()) - start) < windowMs) {
        uint16_t head = g_isrHead;
        if (head >= MAX_EDGES) break;
        if (head > 16) {
            uint32_t since = time_us_32() - g_lastEdgeUs;
            if (since > IDLE_GAP_US) break;
        }
        tight_loop_contents();
    }
    capture_stop();
    return capture_snapshot();
}

static bool pulse_match(uint32_t dur, uint32_t target, uint8_t tolPct) {
    if (target == 0) return false;
    uint32_t tol = (target * tolPct) / 100;
    uint32_t lo = (tol >= target) ? 0 : (target - tol);
    return (dur >= lo) && (dur <= (target + tol));
}
static int next_high(int from) {
    for (int i = from; i < g_edgeCount; i++) if (g_lvl[i] == 1) return i;
    return -1;
}

static bool decode_signal(unsigned long *outValue, int *outBits, int *outProto) {
    if (g_edgeCount < 16) return false;
    for (int p = 1; p < NUM_RC_PROTOCOLS; p++) {
        const RCProtocol *pr = &RC_PROTOCOLS[p];
        if (pr->pulseLength == 0) continue;
        uint32_t T = pr->pulseLength;
        uint32_t sHi = T*pr->syncHigh, sLo = T*pr->syncLow;
        uint32_t zHi = T*pr->zeroHigh, zLo = T*pr->zeroLow;
        uint32_t oHi = T*pr->oneHigh,  oLo = T*pr->oneLow;
        for (int i = 0; i + 1 < g_edgeCount; i++) {
            if (g_lvl[i] != 1 || g_lvl[i+1] != 0) continue;
            uint32_t hi = g_dur[i], lo = g_dur[i+1];
            bool syncFound = pr->inverted
                ? (pulse_match(lo, sHi, 70) && pulse_match(hi, sLo, 70))
                : (pulse_match(hi, sHi, 70) && pulse_match(lo, sLo, 70));
            if (!syncFound) continue;
            unsigned long value = 0; int bits = 0;
            int j = next_high(i + 2);
            while (j >= 0 && j + 1 < g_edgeCount && bits < 32) {
                if (g_lvl[j] != 1 || g_lvl[j+1] != 0) { j = next_high(j + 1); continue; }
                uint32_t h = g_dur[j], l = g_dur[j+1];
                if (l > T * 10) break;
                bool isZero, isOne;
                if (!pr->inverted) {
                    isZero = pulse_match(h, zHi, 60) && pulse_match(l, zLo, 60);
                    isOne  = pulse_match(h, oHi, 60) && pulse_match(l, oLo, 60);
                } else {
                    isZero = pulse_match(l, zHi, 60) && pulse_match(h, zLo, 60);
                    isOne  = pulse_match(l, oHi, 60) && pulse_match(h, oLo, 60);
                }
                if (isZero) { value <<= 1; bits++; }
                else if (isOne) { value = (value << 1) | 1; bits++; }
                else break;
                j += 2;
            }
            if (bits >= 8) { *outValue = value; *outBits = bits; *outProto = p; return true; }
        }
    }
    return false;
}

/* ======================================================================== */
/* HUB LINK — spi0 slave pump, COPIED FROM THE PROVEN GPS BLADE.              */
/* ======================================================================== */
static mutex_t  s_stage_mtx;
static uint8_t  s_tx[FH_SPI_XFER_BYTES];      /* published outbound frame */
static uint8_t  s_pump_tx[FH_SPI_XFER_BYTES]; /* core0 snapshot for the pump */
static uint8_t  s_rx[FH_SPI_XFER_BYTES];
static fh_decoder_t s_in_dec;

/* Cached once by core1 at init. core0 reads it for the PING_REQ radios_ready
 * bit so it NEVER touches spi1 (which core1 owns) — a live cc_present() from
 * core0 during a core1 capture would be a cross-core spi1 collision. */
static volatile bool s_radio_ready = false;

static volatile bool s_cs_fell = false;
static volatile bool s_cs_rose = false;
static volatile int  s_rx_len  = 0;

/* --- fired-op handoff: core0 (decode) -> core1 (execute) -> core0 (stage) ---
 * SINGLE-WRITER DISCIPLINE (restored from the GPS blade): ONLY core0 ever calls
 * stage_frame(). core1 computes the CC1101 RESP into s_resultbuf + sets
 * s_result_ready; core0 drains it in spi_after_txn and stages it. This removes
 * the cross-core staging race the first draft had (core1 and core0 both writing
 * s_tx could lose each other's frame). core0 handles PING/PING_REQ/BUSY itself. */
static volatile bool    s_req_pending  = false;  /* core0 sets, core1 clears   */
static fh_od_cc1101_req_t s_req_cc;               /* core0 writes, core1 reads  */
static uint8_t          s_resultbuf[sizeof(fh_od_cc1101_resp_t)]; /* core1->core0 */
static volatile uint8_t s_result_len   = 0;
static volatile bool    s_result_ready = false;  /* core1 sets, core0 clears   */

/* RAW_OOK capture-to-dump (FH_OD_DUMP_IND 0x2F chunk stream). core1 serializes
 * the edge buffer (5 B/edge: u32 dur_us LE + u8 level) into s_dump_buf and the
 * summary RESP into s_resultbuf, then sets s_stream_ready. core0 (single writer)
 * paces ONE chunk per completed hub transaction — exactly the arduino blade's
 * one-chunk-per-poll model, where the pump's per-transaction cadence replaces
 * the arduino s_tx_done gate — then stages the summary RESP LAST. */
#define DUMP_CHUNK_DATA_MAX 250u
static uint8_t          s_dump_buf[MAX_EDGES * 5];
static volatile uint16_t s_dump_len    = 0;       /* core1 fills, core0 reads    */
static volatile bool    s_stream_ready = false;   /* core1 sets, core0 clears    */
static uint8_t          s_dump_chunk   = 0;       /* core0-only pacing cursor    */
static bool             s_streaming    = false;   /* core0-only: stream active   */

/* Ext-band CC1101 TX (0x34, IMPLEMENTED verb). core0 decodes the generic ext
 * REQ and latches the transmit triple; core1 (the spi1 owner) runs cc_tx_raw.
 * core0 stages the generic ext RESP (0x35) when core1 reports done. */
static volatile bool    s_ext_tx_pending = false;  /* core0 sets, core1 clears   */
static volatile bool    s_ext_tx_ready   = false;  /* core1 sets, core0 clears   */
static uint8_t          s_ext_resultbuf[sizeof(fh_od_ext_resp_t)];
static volatile uint8_t s_ext_result_len = 0;
static struct { uint8_t seq, proto, bits, reps; uint32_t value, freq_khz; }
                        s_ext_tx;                  /* core0 writes, core1 reads  */

static void stage_frame(fh_type_t type, const uint8_t *payload, uint8_t len) {
    uint8_t local[FH_SPI_XFER_BYTES];
    size_t n = fh_frame_encode(type, payload, len, local, sizeof(local));
    uint8_t pad[FH_SPI_XFER_BYTES]; memset(pad, 0, sizeof(pad));
    if (n) memcpy(pad, local, n);
    mutex_enter_blocking(&s_stage_mtx);
    memcpy(s_tx, pad, sizeof(s_tx));
    mutex_exit(&s_stage_mtx);
}

/* true while any radio op (capture, dump stream, or ext-TX) is in flight —
 * the one-at-a-time contract: a new REQ in this window gets FH_OD_BUSY. */
static bool radio_busy(void) {
    return s_req_pending || s_result_ready || s_stream_ready || s_streaming
        || s_ext_tx_pending || s_ext_tx_ready;
}

static void on_in_frame(fh_type_t type, const uint8_t *payload, uint8_t len, void *user) {
    (void)user;
    if (type == FH_TYPE_PING && len >= 1) {
        fh_ping_t p; p.seq = payload[0];
        stage_frame(FH_TYPE_PONG, (uint8_t*)&p, sizeof(p));   /* core0 writer */
    } else if ((uint8_t)type == FH_OD_PING_REQ && len >= 1) {
        fh_od_ping_resp_t r; memset(&r, 0, sizeof(r));
        r.seq = payload[0];
        r.radios_ready = s_radio_ready ? FH_OD_RADIO_CC1101 : 0;
        stage_frame((fh_type_t)FH_OD_PING_RESP, (uint8_t*)&r, sizeof(r));
    } else if ((uint8_t)type == FH_OD_CC1101_REQ && len >= sizeof(fh_od_cc1101_req_t)) {
        if (!radio_busy()) {
            memcpy(&s_req_cc, payload, sizeof(s_req_cc));
            __dmb();                  /* publish the req before the flag */
            s_req_pending = true;     /* core1 picks it up */
        } else {
            fh_od_cc1101_resp_t r; memset(&r, 0, sizeof(r));
            r.seq = payload[0]; r.status = FH_OD_BUSY;
            stage_frame((fh_type_t)FH_OD_CC1101_RESP, (uint8_t*)&r, sizeof(r));
        }
    } else if ((uint8_t)type >= FH_OD_EXT_CC_JAM_REQ && (uint8_t)type <= 0x37
               && (((uint8_t)type & 1u) == 0) && len >= 15) {
        /* EXTERN VERB BAND (Tier 3), CC1101 side (0x30..0x37 even). Decoded
         * with the generic fh_od_ext_req_t; answered with fh_od_ext_resp_t at
         * TYPE+1. CC TX (0x34) is IMPLEMENTED (transmit author-specified OOK
         * bits for YOUR OWN devices, authorized via a hub REQ); JAM/BRUTE are
         * deliberately NOT authored — they answer FH_OD_EXT_STUB to prove the
         * route. Same contract and stub posture as the arduino blade. */
        fh_od_ext_req_t ext; memset(&ext, 0, sizeof(ext));
        memcpy(&ext, payload, len > sizeof(ext) ? sizeof(ext) : len);
        if ((uint8_t)type == FH_OD_EXT_CC_TX_REQ) {
            uint8_t  proto = ext.tail[0];
            uint32_t value = (uint32_t)ext.tail[1] | ((uint32_t)ext.tail[2] << 8)
                           | ((uint32_t)ext.tail[3] << 16)
                           | ((uint32_t)ext.tail[4] << 24);
            uint8_t  bits  = (uint8_t)(ext.p2 & 0xFF);
            uint8_t  reps  = ext.tail[5]; if (reps == 0) reps = 3;
            if (radio_busy()) {
                fh_od_ext_resp_t er; memset(&er, 0, sizeof(er));
                er.seq = ext.seq; er.status = FH_OD_BUSY;
                stage_frame((fh_type_t)FH_OD_EXT_CC_TX_RESP, (uint8_t*)&er, sizeof(er));
            } else if (!s_radio_ready || bits < 1 || bits > 32
                       || proto == 0 || proto >= NUM_RC_PROTOCOLS) {
                fh_od_ext_resp_t er; memset(&er, 0, sizeof(er));
                er.seq = ext.seq; er.status = FH_OD_BAD_REQ;
                stage_frame((fh_type_t)FH_OD_EXT_CC_TX_RESP, (uint8_t*)&er, sizeof(er));
            } else {
                /* Hand the transmit to core1 (the spi1 owner). */
                s_ext_tx.seq = ext.seq; s_ext_tx.proto = proto;
                s_ext_tx.bits = bits;   s_ext_tx.reps = reps;
                s_ext_tx.value = value; s_ext_tx.freq_khz = ext.p1;
                __dmb();
                s_ext_tx_pending = true;
            }
        } else {
            /* EXTERNAL AUTHOR'S SLOT (JAM/BRUTE not authored in the remix). */
            fh_od_ext_resp_t er; memset(&er, 0, sizeof(er));
            er.seq = ext.seq; er.status = FH_OD_EXT_STUB;
            stage_frame((fh_type_t)((uint8_t)type + 1), (uint8_t*)&er, sizeof(er));
        }
    }
}

/* --- Shared GPIO-bank callback, registered on CORE0. The SDK's
 * gpio_set_irq_enabled_with_callback installs ONE callback for the whole GPIO
 * bank on the calling core, so BOTH the hub CS edges and the radio GDO0 capture
 * edges arrive here (dispatched by pin). Both pins' IRQs are enabled on core0
 * (CS + GDO0 in main) — the RP2040 services a GPIO IRQ only on the core that
 * enabled it, so enabling GDO0 from core1 would NOT work (that bug was fixed in
 * the 2nd multi-check). gdo0_isr no-ops unless g_capturing. HONEST TRADEOFF
 * (R-SPI2): during a multi-second capture the OOK edge storm shares core0 with
 * the pump; tolerable because the hub polls. Escalation if it starves:
 * irq_set_exclusive_handler(IO_IRQ_BANK0) — NOT done. --- */
static void cs_gpio_isr(uint gpio, uint32_t events) {
    if (gpio == FH_PICO_SPI_CS) {
        if (events & GPIO_IRQ_EDGE_FALL) s_cs_fell = true;
        if (events & GPIO_IRQ_EDGE_RISE) s_cs_rose = true;
        return;
    }
    if (gpio == FH_CC_GDO0) { gdo0_isr(gpio, events); }
}

static int spi_reset_and_preload(const uint8_t *tx) {
    reset_block(RESETS_RESET_SPI0_BITS);
    unreset_block_wait(RESETS_RESET_SPI0_BITS);
    spi_init(SPI_HUB, 8000000);
    spi_set_slave(SPI_HUB, true);
    spi_set_format(SPI_HUB, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);  /* MODE 1 */
    gpio_set_function(FH_PICO_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(FH_PICO_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(FH_PICO_SPI_MISO, GPIO_FUNC_SPI);
    gpio_set_function(FH_PICO_SPI_CS,   GPIO_FUNC_SPI);
    int n = 0;
    while (n < 8 && spi_is_writable(SPI_HUB)) { spi_get_hw(SPI_HUB)->dr = tx[n++]; }
    return n;
}

static void spi_txn_pump(const uint8_t *tx_active) {
    int tx_next = spi_reset_and_preload(tx_active);
    int rx_next = 0;
    absolute_time_t t0 = get_absolute_time();
    for (;;) {
        if (s_cs_fell) { s_cs_fell = false; rx_next = 0; }
        if (s_cs_rose) { s_cs_rose = false; s_rx_len = rx_next; return; }
        if (spi_is_readable(SPI_HUB)) {
            s_rx[rx_next++] = (uint8_t)(spi_get_hw(SPI_HUB)->dr);
            if (tx_next < FH_SPI_XFER_BYTES && spi_is_writable(SPI_HUB)) {
                spi_get_hw(SPI_HUB)->dr = tx_active[tx_next++];
            }
            if (rx_next == FH_SPI_XFER_BYTES) { s_rx_len = FH_SPI_XFER_BYTES; return; }
        } else if (absolute_time_diff_us(t0, get_absolute_time()) > 200000) {
            s_rx_len = rx_next; return;
        } else {
            tight_loop_contents();
        }
    }
}

static const uint8_t *spi_after_txn(const uint8_t *rx, int rx_len) {
    fh_decoder_push(&s_in_dec, rx, (size_t)rx_len, on_in_frame, NULL);

    /* Single-writer staging, in priority order. */
    if (s_stream_ready && !s_streaming) {   /* core1 just finished a RAW_OOK dump */
        __dmb();
        s_streaming  = true;
        s_dump_chunk = 0;
        s_stream_ready = false;
    }
    if (s_streaming) {
        /* Pace ONE DUMP_IND chunk per transaction; summary RESP goes last. */
        uint16_t off = (uint16_t)s_dump_chunk * (uint16_t)DUMP_CHUNK_DATA_MAX;
        if (off >= s_dump_len) {
            stage_frame((fh_type_t)FH_OD_CC1101_RESP, s_resultbuf, s_result_len);
            s_streaming = false;
            s_dump_len = 0; s_dump_chunk = 0;
            s_req_pending = false;          /* op complete — accept the next */
        } else {
            fh_od_dump_ind_t ind; memset(&ind, 0, sizeof(ind));
            ind.seq   = s_resultbuf[0];     /* summary RESP's seq is byte 0 */
            ind.radio = FH_DUMP_RADIO_CC1101;
            ind.chunk = s_dump_chunk;
            uint16_t n = (uint16_t)(s_dump_len - off);
            if (n > DUMP_CHUNK_DATA_MAX) n = DUMP_CHUNK_DATA_MAX;
            memcpy(ind.data, s_dump_buf + off, n);
            ind.len  = (uint8_t)n;
            ind.more = (uint8_t)((off + n) < s_dump_len);
            stage_frame((fh_type_t)FH_OD_DUMP_IND, (uint8_t*)&ind, sizeof(ind));
            s_dump_chunk++;
        }
    } else if (s_result_ready) {            /* plain (non-dump) RESP */
        __dmb();
        stage_frame((fh_type_t)FH_OD_CC1101_RESP, s_resultbuf, s_result_len);
        s_result_ready = false;
    } else if (s_ext_tx_ready) {            /* ext-band CC TX (0x34) finished */
        __dmb();
        stage_frame((fh_type_t)FH_OD_EXT_CC_TX_RESP, s_ext_resultbuf, s_ext_result_len);
        s_ext_tx_ready = false;
    }

    mutex_enter_blocking(&s_stage_mtx);
    memcpy(s_pump_tx, s_tx, sizeof(s_pump_tx));
    mutex_exit(&s_stage_mtx);
    return s_pump_tx;
}

/* ======================================================================== */
/* core1 — RADIO op executor (fired ops run here, off the pump).             */
/* ======================================================================== */
void core1_entry(void) {
    cc_init(433.92f);           /* bring up the CC1101 on spi1 (async OOK RX) */
    bool ready = cc_present();
    if (ready) cc_enter_rx();
    s_radio_ready = ready;      /* publish for core0's PING_REQ (no spi1 on core0) */

    for (;;) {
        /* Ext-band CC TX (0x34): transmit author-specified OOK bits on spi1. */
        if (s_ext_tx_pending) {
            __dmb();
            if (s_radio_ready) {
                float mhz = (float)s_ext_tx.freq_khz / 1000.0f;
                if (mhz >= 300.0f && mhz <= 928.0f) cc_set_freq(mhz);
                cc_tx_raw(s_ext_tx.value, s_ext_tx.bits, s_ext_tx.proto, s_ext_tx.reps);
            }
            fh_od_ext_resp_t er; memset(&er, 0, sizeof(er));
            er.seq = s_ext_tx.seq;
            er.status = s_radio_ready ? FH_OD_OK : FH_OD_RADIO_FAIL;
            if (s_radio_ready) {
                er.len = 6;                 /* echo: proto, bits, value LE */
                er.data[0] = s_ext_tx.proto;
                er.data[1] = s_ext_tx.bits;
                er.data[2] = (uint8_t)(s_ext_tx.value & 0xFF);
                er.data[3] = (uint8_t)((s_ext_tx.value >> 8) & 0xFF);
                er.data[4] = (uint8_t)((s_ext_tx.value >> 16) & 0xFF);
                er.data[5] = (uint8_t)((s_ext_tx.value >> 24) & 0xFF);
            }
            memcpy(s_ext_resultbuf, &er, sizeof(er));
            s_ext_result_len = (uint8_t)sizeof(er);
            __dmb();
            s_ext_tx_ready   = true;
            s_ext_tx_pending = false;
            continue;
        }
        if (!s_req_pending) { sleep_ms(2); continue; }

        fh_od_cc1101_req_t req = s_req_cc;
        __dmb();
        fh_od_cc1101_resp_t resp; memset(&resp, 0, sizeof(resp));
        resp.seq = req.seq;

        if (!s_radio_ready) {
            resp.status = FH_OD_RADIO_FAIL;
        } else if (req.action == FH_OD_CC1101_ACT_REPLAY) {
            if (!g_haveCapture) { resp.status = FH_OD_NO_RESULT; }
            else {
                float mhz = (float)req.freq_khz / 1000.0f;
                if (mhz >= 300.0f && mhz <= 928.0f) cc_set_freq(mhz);
                cc_tx_raw(g_lastValue, g_lastBits, g_lastProto, 3);
                resp.status = FH_OD_OK;
                resp.data[0] = (uint8_t)g_lastProto;
                resp.data[1] = (uint8_t)g_lastBits;
                resp.data[2] = (uint8_t)(g_lastValue & 0xFF);
                resp.data[3] = (uint8_t)((g_lastValue >> 8) & 0xFF);
                resp.data[4] = (uint8_t)((g_lastValue >> 16) & 0xFF);
                resp.data[5] = (uint8_t)((g_lastValue >> 24) & 0xFF);
                resp.len = 6;
            }
        } else {
            float mhz = (float)req.freq_khz / 1000.0f;
            if (mhz >= 300.0f && mhz <= 928.0f) cc_set_freq(mhz);
            uint32_t window = req.dwell_ms ? req.dwell_ms : 4000;
            if (window > 30000) window = 30000;
            int n = capture_frame(window);
            if (n <= 0) { resp.status = FH_OD_NO_RESULT; }
            else {
                unsigned long v; int b, p;
                if (decode_signal(&v, &b, &p)) {
                    g_lastValue = v; g_lastBits = b; g_lastProto = p; g_haveCapture = true;
                    resp.status = FH_OD_OK;
                    resp.data[0] = (uint8_t)p;
                    resp.data[1] = (uint8_t)b;
                    resp.data[2] = (uint8_t)(v & 0xFF);
                    resp.data[3] = (uint8_t)((v >> 8) & 0xFF);
                    resp.data[4] = (uint8_t)((v >> 16) & 0xFF);
                    resp.data[5] = (uint8_t)((v >> 24) & 0xFF);
                    resp.len = 6;
                    resp.rssi = (int8_t)cc_rssi();
                } else {
                    resp.status = FH_OD_NO_RESULT;
                }
            }
        }
        /* Hand the finished op to core0 (the single staging writer). */
        if (req.action == FH_OD_CC1101_ACT_RAW_OOK && resp.status == FH_OD_OK) {
            /* RAW_OOK capture-to-dump: serialize the edge buffer (5 B/edge:
             * u32 dur_us LE + u8 level) and let core0 pace the DUMP_IND chunks
             * with the summary RESP last. The RESP keeps the decode triple +
             * RSSI, so a later REPLAY works from a raw capture too. */
            int cnt = g_edgeCount; if (cnt > MAX_EDGES) cnt = MAX_EDGES;
            uint16_t dl = 0;
            for (int i = 0; i < cnt && dl + 5 <= (uint16_t)sizeof(s_dump_buf); ++i) {
                uint32_t d = g_dur[i];
                s_dump_buf[dl++] = (uint8_t)(d & 0xFF);
                s_dump_buf[dl++] = (uint8_t)((d >> 8) & 0xFF);
                s_dump_buf[dl++] = (uint8_t)((d >> 16) & 0xFF);
                s_dump_buf[dl++] = (uint8_t)((d >> 24) & 0xFF);
                s_dump_buf[dl++] = g_lvl[i];
            }
            memcpy(s_resultbuf, &resp, sizeof(resp));
            s_result_len = (uint8_t)sizeof(resp);
            s_dump_len   = dl;
            __dmb();
            s_stream_ready = true;
            s_req_pending  = false;   /* op accepted; core0 drains the stream */
        } else {
            memcpy(s_resultbuf, &resp, sizeof(resp));
            s_result_len = (uint8_t)sizeof(resp);
            __dmb();
            s_result_ready = true;
            s_req_pending  = false;   /* op complete — accept the next */
        }
    }
}

/* ======================================================================== */
/* core0 — hub link main loop.                                               */
/* ======================================================================== */
static void bench_blink_n(int n) {
    gpio_init(FH_PICO_LED); gpio_set_dir(FH_PICO_LED, GPIO_OUT);
    for (int i = 0; i < n; ++i) {
        gpio_put(FH_PICO_LED, 1); sleep_ms(80);
        gpio_put(FH_PICO_LED, 0); sleep_ms(120);
    }
    sleep_ms(600);
}

int main(void) {
    mutex_init(&s_stage_mtx);
    memset(s_tx, 0, sizeof(s_tx));
    fh_decoder_init(&s_in_dec);
    bench_blink_n(1);

    /* hub-link spi0 slave, MODE 1 (same as GPS) */
    spi_init(SPI_HUB, 8000000);
    spi_set_slave(SPI_HUB, true);
    spi_set_format(SPI_HUB, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(FH_PICO_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(FH_PICO_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(FH_PICO_SPI_MISO, GPIO_FUNC_SPI);
    gpio_set_function(FH_PICO_SPI_CS,   GPIO_FUNC_SPI);

    /* Install the one GPIO-bank callback (cs_gpio_isr) here on core0 and enable
     * the hub CS edges now. The radio GDO0 edges use the SAME callback but are
     * ENABLED only during a capture window (capture_start/stop on core1). See
     * cs_gpio_isr + R-SPI2. */
    gpio_init(FH_PICO_SPI_CS); gpio_set_dir(FH_PICO_SPI_CS, GPIO_IN);
    gpio_set_function(FH_PICO_SPI_CS, GPIO_FUNC_SPI);
    gpio_set_irq_enabled_with_callback(FH_PICO_SPI_CS,
        GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, cs_gpio_isr);

    /* Enable the radio GDO0 capture edges ON CORE0 too — the RP2040 services a
     * GPIO IRQ only on the core that enabled it, and the bank callback above is
     * core0's, so GDO0 MUST be enabled here (NOT from core1's capture_start).
     * It stays enabled; gdo0_isr no-ops unless g_capturing, so edges outside a
     * capture window are harmless. GDO0 is an input (the driver drives it out
     * only during a TX, on core1). */
    gpio_init(FH_CC_GDO0); gpio_set_dir(FH_CC_GDO0, GPIO_IN);
    gpio_set_irq_enabled(FH_CC_GDO0, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);

    /* launch the radio executor on core1 (it owns spi1 + the capture IRQ) */
    multicore_launch_core1(core1_entry);
    bench_blink_n(2);

    const uint8_t *tx_active = s_pump_tx;
    {
        mutex_enter_blocking(&s_stage_mtx);
        memcpy(s_pump_tx, s_tx, sizeof(s_pump_tx));
        mutex_exit(&s_stage_mtx);
    }
    uint8_t local_rx[FH_SPI_XFER_BYTES];
    for (;;) {
        spi_txn_pump(tx_active);
        memcpy(local_rx, s_rx, FH_SPI_XFER_BYTES);
        tx_active = spi_after_txn(local_rx, s_rx_len);
    }
    return 0;
}

/* ===========================================================================
 * KNOWN RISKS (UNVERIFIED — first build + bench)
 *  R-STAGE SINGLE WRITER (fixed in multi-check): only core0 calls stage_frame.
 *          core1 computes the CC1101 RESP into s_resultbuf + s_result_ready;
 *          core0 drains it in spi_after_txn. The first draft staged from both
 *          cores (a lost-update race) — corrected. core0 never touches spi1:
 *          the PING_REQ radios_ready bit reads the cached s_radio_ready that
 *          core1 sets at init (a live cc_present() from core0 during a core1
 *          capture would be a cross-core spi1 collision).
 *  R-RAWOOK PORTED: RAW_OOK (action 2) now serializes the edge buffer and
 *          core0 paces FH_OD_DUMP_IND chunks (5 B/edge, u32 dur_us LE + u8
 *          level) ONE PER HUB TRANSACTION, summary RESP last — the arduino
 *          blade's one-chunk-per-poll model, with the pump's per-transaction
 *          cadence replacing the arduino s_tx_done gate. Also ported: ext-band
 *          CC TX (0x34, IMPLEMENTED) executed on core1; JAM/BRUTE (0x30/0x32)
 *          answer FH_OD_EXT_STUB. BENCH UNKNOWN: verify the hub ingests the
 *          chunk-then-RESP ordering and that pacing keeps up under real edges.
 *  R-SPI2  The GDO0 capture IRQ and the hub CS IRQ share ONE GPIO-bank callback
 *          (cs_gpio_isr, dispatched by pin) registered on CORE0. 2nd multi-check
 *          FOUND+FIXED a real bug: GDO0's IRQ was being enabled from core1
 *          (capture_start), but the RP2040 services a GPIO IRQ only on the core
 *          that enabled it — so GDO0 edges enabled on core1 would never reach
 *          the core0 callback, and capture would record NOTHING. GDO0 is now
 *          enabled ONCE on core0 at startup (main), and capture_start/stop just
 *          flip g_capturing (gdo0_isr no-ops outside a window). Cross-core ring
 *          state (g_capturing/g_isrHead/g_edge*) is volatile + __dmb-fenced at
 *          the start/snapshot boundaries. BENCH: the OOK edge storm still lands
 *          on core0 (shared with the pump) during a capture; tolerable because
 *          the hub polls, but if the pump starves, escalate to
 *          irq_set_exclusive_handler(IO_IRQ_BANK0) — documented, NOT done.
 *          ALSO: core0 gpio_init's GDO0 before core1's cc_init reconfigures it
 *          (the driver's setGDO); the IRQ-enable register is separate from pin
 *          function, so this is believed safe — confirm at the bench.
 *          SIDE EFFECT of the always-on GDO0 IRQ: during a REPLAY/ext-TX,
 *          core1 drives GDO0 as an OUTPUT and bit-bangs it, so each TX edge
 *          also fires core0's gdo0_isr. It no-ops (g_capturing is false during
 *          TX), so it is harmless, but it does spend core0 cycles for the TX
 *          duration. If a TX ever coincides with a hub poll that must not slip,
 *          the fix is to gate GDO0's IRQ off around cc_tx_raw — but that gate
 *          must run ON CORE0 (not core1), so it would need a flag handshake.
 *          Left as-is (simpler, and TX is brief); noted for the bench.
 *  R-MODE  Two SPI roles, two modes, both correct: hub link = spi0 slave MODE 1
 *          (RP2040 one-byte-per-CS rule); radio = spi1 master MODE 0 (CC1101 is
 *          a MODE-0 device, set inside the driver). Do NOT unify them.
 *  R-PUMP  The hub pump is copied from the bench-proven GPS blade. The radio
 *          core (capture ring, decode, transmit) and the spi1 bring-up are the
 *          unproven surface — bench them against a known OOK remote.
 *  R-LINK  main.c (C) reaches the C++ driver ONLY via cc_shim.cpp extern "C"
 *          wrappers; main.c does NOT include the C++ class header. Confirm the
 *          mixed C/C++ link on the first build.
 * ======================================================================== */
