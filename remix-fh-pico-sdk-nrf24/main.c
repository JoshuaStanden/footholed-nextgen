/*
 * main.c — FHAdvanced-REMIX nRF24 blade, native Raspberry Pi Pico SDK.
 *
 * UNVERIFIED FIRST DRAFT — reasoned-not-compiled. The pico-sdk port of the
 * arduino nrf24 framework blade. 2.4 GHz presence SCAN + payload SNIFF
 * (capture-to-dump) + ext-band TX. The nRF24 has NO real RSSI — only the 1-bit
 * RPD/carrier flag — so SCAN reports the active CHANNEL, not a dBm.
 *
 * TWO SPI BLOCKS (decision 2026-11-01):
 *   core0 — HUB LINK: spi0 SLAVE, MODE 1, the BENCH-PROVEN GPS pump copied
 *           verbatim (full SPI0 reset + 8-byte preload + paired RX/TX push +
 *           CS-edge realign ISR). UNLIKE the sx1262 blade, there is NO RadioLib
 *           PicoHal here, so the CS-edge GPIO IRQ is used exactly as the GPS/
 *           cc1101 blades do — no bank-IRQ conflict, no CS polling needed.
 *   core1 — RADIO: spi1 MASTER to the nRF24L01+ via the hand-written nrf_shim.c
 *           (FH-owned, permissive — the GPL AndyRids driver was rejected). Runs
 *           the fired SCAN/SNIFF op + ext-band TX off the pump.
 *
 * SINGLE-WRITER: ONLY core0 calls stage_frame(). core1 produces results +
 * (for SNIFF) a dump buffer into mailboxes; core0 drains + paces them.
 *
 * KNOWN RISKS at EOF. The hub pump is the proven part; the hand-written nRF24
 * driver and the default-address capture sniff are the bench surface.
 */
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/mutex.h"
#include "pico/multicore.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/resets.h"
#include "hardware/sync.h"

#include "fh_frame.h"
#include "fh_ondemand.h"
#include "fh_pico_pins_sdk.h"
#include "nrf_shim.h"           /* FH-owned nRF24 driver (C, same TU language) */

#define SPI_HUB   spi0
#define DUMP_CHUNK_DATA_MAX 250u
#define DUMP_REC_NRF24      40u    /* u32 t_ms LE + u8 len + payload[32] + 3 pad */
#define NRF_DUMPS_MAX       64u    /* 64 records x 40 B = 2560 B                 */

/* ======================================================================== */
/* HUB LINK — spi0 slave pump, COPIED FROM THE PROVEN GPS BLADE.             */
/* ======================================================================== */
static mutex_t  s_stage_mtx;
static uint8_t  s_tx[FH_SPI_XFER_BYTES];
static uint8_t  s_pump_tx[FH_SPI_XFER_BYTES];
static uint8_t  s_rx[FH_SPI_XFER_BYTES];
static fh_decoder_t s_in_dec;
static volatile int s_rx_len = 0;

static volatile bool s_cs_fell = false;
static volatile bool s_cs_rose = false;

static volatile bool s_radio_ready = false;   /* core1 sets after nrf_init */

/* --- fired SCAN/SNIFF: core0 (decode) -> core1 (execute) -> core0 (stage) --- */
static volatile bool    s_req_pending = false;
static fh_od_nrf24_req_t s_req_nrf;
static uint8_t          s_resultbuf[sizeof(fh_od_nrf24_resp_t)];
static volatile uint8_t s_result_len = 0;
static volatile bool    s_result_ready = false;

/* SNIFF capture-to-dump (FH_OD_DUMP_IND, radio=FH_DUMP_RADIO_NRF24). */
static uint8_t          s_dump_buf[NRF_DUMPS_MAX * DUMP_REC_NRF24];
static volatile uint16_t s_dump_len = 0;
static volatile bool    s_stream_ready = false;
static uint8_t          s_dump_chunk = 0;
static bool             s_streaming  = false;

/* --- ext-band nRF TX (0x3C): core0 latches, core1 transmits --- */
static volatile bool    s_ext_tx_pending = false;
static volatile bool    s_ext_tx_ready   = false;
static uint8_t          s_ext_resultbuf[sizeof(fh_od_ext_resp_t)];
static volatile uint8_t s_ext_result_len = 0;
static struct { uint8_t seq, chan, reps, plen; uint8_t payload[32]; } s_ext_tx;

static void stage_frame(fh_type_t type, const uint8_t *payload, uint8_t len) {
    uint8_t local[FH_SPI_XFER_BYTES];
    size_t n = fh_frame_encode(type, payload, len, local, sizeof(local));
    uint8_t pad[FH_SPI_XFER_BYTES]; memset(pad, 0, sizeof(pad));
    if (n) memcpy(pad, local, n);
    mutex_enter_blocking(&s_stage_mtx);
    memcpy(s_tx, pad, sizeof(s_tx));
    mutex_exit(&s_stage_mtx);
}

static bool radio_busy(void) {
    return s_req_pending || s_result_ready || s_stream_ready || s_streaming
        || s_ext_tx_pending || s_ext_tx_ready;
}

static void on_in_frame(fh_type_t type, const uint8_t *payload, uint8_t len, void *user) {
    (void)user;
    if (type == FH_TYPE_PING && len >= 1) {
        fh_ping_t p; p.seq = payload[0];
        stage_frame(FH_TYPE_PONG, (uint8_t*)&p, sizeof(p));
    } else if ((uint8_t)type == FH_OD_PING_REQ && len >= 1) {
        fh_od_ping_resp_t r; memset(&r, 0, sizeof(r));
        r.seq = payload[0];
        r.radios_ready = s_radio_ready ? FH_OD_RADIO_NRF24 : 0;
        stage_frame((fh_type_t)FH_OD_PING_RESP, (uint8_t*)&r, sizeof(r));
    } else if ((uint8_t)type == FH_OD_NRF24_REQ && len >= sizeof(fh_od_nrf24_req_t)) {
        if (!radio_busy()) {
            memcpy(&s_req_nrf, payload, sizeof(s_req_nrf));
            __dmb();
            s_req_pending = true;
        } else {
            fh_od_nrf24_resp_t r; memset(&r, 0, sizeof(r));
            r.seq = payload[0]; r.status = FH_OD_BUSY; r.active_chan = 0xFF;
            stage_frame((fh_type_t)FH_OD_NRF24_RESP, (uint8_t*)&r, sizeof(r));
        }
    } else if ((uint8_t)type >= FH_OD_EXT_NRF_JAM_REQ && (uint8_t)type <= 0x3F
               && (((uint8_t)type & 1u) == 0) && len >= 15) {
        /* EXTERN VERB BAND, nRF24 side (0x38..0x3F even). NRF TX (0x3C) is
         * IMPLEMENTED; JAM/PROTOKILL answer FH_OD_EXT_STUB. */
        fh_od_ext_req_t ext; memset(&ext, 0, sizeof(ext));
        memcpy(&ext, payload, len > sizeof(ext) ? sizeof(ext) : len);
        if ((uint8_t)type == FH_OD_EXT_NRF_TX_REQ) {
            uint8_t ch   = (uint8_t)(ext.p1 & 0xFF);
            uint8_t reps = (uint8_t)(ext.p2 & 0xFF); if (reps == 0) reps = 1;
            if (reps > 16) reps = 16;
            uint8_t plen = ext.tail[0];
            if (radio_busy()) {
                fh_od_ext_resp_t er; memset(&er, 0, sizeof(er));
                er.seq = ext.seq; er.status = FH_OD_BUSY;
                stage_frame((fh_type_t)FH_OD_EXT_NRF_TX_RESP, (uint8_t*)&er, sizeof(er));
            } else if (!s_radio_ready || ch > 125 || plen < 1 || plen > 32
                       || (uint8_t)len < (uint8_t)(15 + plen)) {
                fh_od_ext_resp_t er; memset(&er, 0, sizeof(er));
                er.seq = ext.seq; er.status = FH_OD_BAD_REQ;
                stage_frame((fh_type_t)FH_OD_EXT_NRF_TX_RESP, (uint8_t*)&er, sizeof(er));
            } else {
                s_ext_tx.seq = ext.seq; s_ext_tx.chan = ch; s_ext_tx.reps = reps;
                s_ext_tx.plen = plen;
                memcpy(s_ext_tx.payload, ext.tail + 1, plen);
                __dmb();
                s_ext_tx_pending = true;
            }
        } else {
            fh_od_ext_resp_t er; memset(&er, 0, sizeof(er));
            er.seq = ext.seq; er.status = FH_OD_EXT_STUB;
            stage_frame((fh_type_t)((uint8_t)type + 1), (uint8_t*)&er, sizeof(er));
        }
    }
}

static void cs_gpio_isr(uint gpio, uint32_t events) {
    (void)gpio;
    if (events & GPIO_IRQ_EDGE_FALL) s_cs_fell = true;
    if (events & GPIO_IRQ_EDGE_RISE) s_cs_rose = true;
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

    /* Single-writer staging. SNIFF dump stream (if active) paces one chunk per
     * transaction, summary RESP last; then plain SCAN/SNIFF result; then ext TX.
     * (No unsolicited frames on this blade, so a simple priority order.) */
    if (s_stream_ready && !s_streaming) {
        __dmb(); s_streaming = true; s_dump_chunk = 0; s_stream_ready = false;
    }
    if (s_streaming) {
        uint16_t off = (uint16_t)s_dump_chunk * (uint16_t)DUMP_CHUNK_DATA_MAX;
        if (off >= s_dump_len) {
            stage_frame((fh_type_t)FH_OD_NRF24_RESP, s_resultbuf, s_result_len);
            s_streaming = false; s_dump_len = 0; s_dump_chunk = 0;
            s_req_pending = false;
        } else {
            fh_od_dump_ind_t ind; memset(&ind, 0, sizeof(ind));
            ind.seq   = s_resultbuf[0];
            ind.radio = FH_DUMP_RADIO_NRF24;
            ind.chunk = s_dump_chunk;
            uint16_t n = (uint16_t)(s_dump_len - off);
            if (n > DUMP_CHUNK_DATA_MAX) n = DUMP_CHUNK_DATA_MAX;
            memcpy(ind.data, s_dump_buf + off, n);
            ind.len  = (uint8_t)n;
            ind.more = (uint8_t)((off + n) < s_dump_len);
            stage_frame((fh_type_t)FH_OD_DUMP_IND, (uint8_t*)&ind, sizeof(ind));
            s_dump_chunk++;
        }
    } else if (s_result_ready) {
        __dmb();
        stage_frame((fh_type_t)FH_OD_NRF24_RESP, s_resultbuf, s_result_len);
        s_result_ready = false;
    } else if (s_ext_tx_ready) {
        __dmb();
        stage_frame((fh_type_t)FH_OD_EXT_NRF_TX_RESP, s_ext_resultbuf, s_ext_result_len);
        s_ext_tx_ready = false;
    }

    mutex_enter_blocking(&s_stage_mtx);
    memcpy(s_pump_tx, s_tx, sizeof(s_pump_tx));
    mutex_exit(&s_stage_mtx);
    return s_pump_tx;
}

/* ======================================================================== */
/* core1 — RADIO op executor (nRF24 on spi1, via nrf_shim.c).                */
/* ======================================================================== */
void core1_entry(void) {
    bool ok = nrf_init(76);                /* default channel 76 */
    s_radio_ready = ok;

    for (;;) {
        /* ext-band nRF TX (0x3C) */
        if (s_ext_tx_pending) {
            __dmb();
            fh_od_ext_resp_t er; memset(&er, 0, sizeof(er));
            er.seq = s_ext_tx.seq;
            if (!nrf_present()) { er.status = FH_OD_RADIO_FAIL; }
            else {
                nrf_set_channel(s_ext_tx.chan);
                uint8_t okN = 0;
                for (uint8_t r = 0; r < s_ext_tx.reps; r++) {
                    if (nrf_tx_payload(s_ext_tx.payload, s_ext_tx.plen)) okN++;
                }
                nrf_set_channel(76);
                nrf_idle();
                er.status = FH_OD_OK;
                er.data[0] = s_ext_tx.plen;
                memcpy(er.data + 1, s_ext_tx.payload, s_ext_tx.plen);
                er.data[1 + s_ext_tx.plen] = okN;
                er.len = (uint8_t)(1 + s_ext_tx.plen + 1);
            }
            memcpy(s_ext_resultbuf, &er, sizeof(er));
            s_ext_result_len = (uint8_t)sizeof(er);
            __dmb();
            s_ext_tx_ready = true;
            s_ext_tx_pending = false;
            continue;
        }

        if (!s_req_pending) { sleep_ms(2); continue; }

        fh_od_nrf24_req_t req = s_req_nrf;
        __dmb();
        fh_od_nrf24_resp_t resp; memset(&resp, 0, sizeof(resp));
        resp.seq = req.seq; resp.active_chan = 0xFF; resp.rssi = 0;

        if (!nrf_present()) {
            resp.status = FH_OD_RADIO_FAIL;
            memcpy(s_resultbuf, &resp, sizeof(resp));
            s_result_len = (uint8_t)sizeof(resp);
            __dmb(); s_result_ready = true; s_req_pending = false;
            continue;
        }

        if (req.mode == FH_OD_NRF24_MODE_SNIFF) {
            uint8_t ch = req.chan_lo; if (ch > 125) ch = 125;
            uint32_t window = req.dwell_ms ? req.dwell_ms : 5000;
            if (window > 30000) window = 30000;
            nrf_set_channel(ch);
            nrf_rx_mode(true);             /* open capture (default-addr, not true promisc) */
            uint16_t dl = 0;
            uint32_t t0 = to_ms_since_boot(get_absolute_time());
            while ((to_ms_since_boot(get_absolute_time()) - t0) < window
                   && dl + DUMP_REC_NRF24 <= (uint16_t)sizeof(s_dump_buf)) {
                uint8_t pb[32];
                int plen = nrf_rx_payload(pb, sizeof(pb));
                if (plen >= 1 && plen <= 32) {
                    uint32_t t = to_ms_since_boot(get_absolute_time()) - t0;
                    uint8_t *r = s_dump_buf + dl;
                    r[0]=(uint8_t)(t&0xFF); r[1]=(uint8_t)((t>>8)&0xFF);
                    r[2]=(uint8_t)((t>>16)&0xFF); r[3]=(uint8_t)((t>>24)&0xFF);
                    r[4]=(uint8_t)plen;
                    memcpy(r + 5, pb, plen);
                    memset(r + 5 + plen, 0, 32 + 3 - plen);
                    dl += DUMP_REC_NRF24;
                } else {
                    sleep_ms(1);
                }
            }
            nrf_idle();
            resp.active_chan = ch;
            resp.status = (dl > 0) ? FH_OD_OK : FH_OD_NO_RESULT;
            if (resp.status == FH_OD_OK) {
                memcpy(s_resultbuf, &resp, sizeof(resp));
                s_result_len = (uint8_t)sizeof(resp);
                s_dump_len = dl;
                __dmb(); s_stream_ready = true; s_req_pending = false;
                continue;
            }
        } else {
            /* SCAN: RPD/carrier presence sweep across [chan_lo..chan_hi]. */
            uint8_t lo = req.chan_lo, hi = req.chan_hi;
            if (hi > 125) hi = 125;
            if (lo > hi) { uint8_t t = lo; lo = hi; hi = t; }
            uint32_t dwell = req.dwell_ms ? req.dwell_ms : 2;
            if (dwell > 10) dwell = 10;
            bool found = false;
            for (uint8_t ch = lo; ch <= hi && !found; ch++) {
                nrf_set_channel(ch);
                nrf_rx_mode(false);
                sleep_ms(dwell);
                if (nrf_rpd()) { resp.active_chan = ch; found = true; }
                nrf_idle();
            }
            nrf_set_channel(76);
            resp.status = found ? FH_OD_OK : FH_OD_NO_RESULT;
        }

        memcpy(s_resultbuf, &resp, sizeof(resp));
        s_result_len = (uint8_t)sizeof(resp);
        __dmb(); s_result_ready = true; s_req_pending = false;
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

    gpio_init(FH_PICO_IRQ_OUT); gpio_set_dir(FH_PICO_IRQ_OUT, GPIO_OUT);
    gpio_put(FH_PICO_IRQ_OUT, 0);

    spi_init(SPI_HUB, 8000000);
    spi_set_slave(SPI_HUB, true);
    spi_set_format(SPI_HUB, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(FH_PICO_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(FH_PICO_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(FH_PICO_SPI_MISO, GPIO_FUNC_SPI);
    gpio_set_function(FH_PICO_SPI_CS,   GPIO_FUNC_SPI);

    /* CS-edge ISR (hub). No RadioLib here, so the standard GPS/cc1101 CS-edge
     * callback is used directly — no bank-IRQ conflict (contrast sx1262). */
    gpio_init(FH_PICO_SPI_CS); gpio_set_dir(FH_PICO_SPI_CS, GPIO_IN);
    gpio_set_function(FH_PICO_SPI_CS, GPIO_FUNC_SPI);
    gpio_set_irq_enabled_with_callback(FH_PICO_SPI_CS,
        GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, cs_gpio_isr);

    multicore_launch_core1(core1_entry);   /* owns spi1 + the nRF24 */
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
 * (Plain-English version of every R-* tag below is in README.md →
 *  "Known limitations (plain English)" — for a human or AI at the bench.)
 *  R-DRIVER  nrf_shim.c is HAND-WRITTEN against the Nordic nRF24L01+ datasheet
 *            register map (NOT a vendored lib — the clean pico-sdk driver,
 *            AndyRids/pico-nrf24, is GPL-2.0 and was rejected to keep the
 *            project permissive). Register addrs/bits are datasheet facts, but
 *            this code is unproven on silicon — bench it first against a known
 *            nRF24 transmitter. 2nd multi-check FOUND+FIXED a real config bug:
 *            the sniff path had enabled dynamic payloads (DPL) with auto-ack
 *            OFF, which is contradictory (DPL requires auto-ack) and would have
 *            made SNIFF receive nothing. Now uses the datasheet-correct capture
 *            recipe (Nordic-confirmed): clear FEATURE/EN_AA, CRC off, STATIC
 *            32-byte RX_PW_P0, read a fixed 32 bytes. Watch remaining timing
 *            (Tstby2a / CE pulse) at the bench.
 *  R-PROMISC SNIFF is NOT true promiscuous capture, AND (consequence of the
 *            static-width fix) it has NO per-packet length: the nRF24 in
 *            capture mode reads a FIXED 32 bytes per packet, so every dump
 *            record's len = 32 even if fewer bytes were sent (the trailing
 *            bytes are FIFO content, not real payload). This is a hardware
 *            truth — DPL (which carries length) needs auto-ack, which defeats
 *            capture. Also NOT promiscuous: RX_ADDR_P0 stays at the power-on
 *            default (0xE7E7E7E7E7), so it captures packets sent TO THAT
 *            ADDRESS (which this blade's own ext-band TX uses, so a TX->RX
 *            self-test works) — not arbitrary 2.4 GHz traffic. For real
 *            promiscuity + true lengths, extend nrf_shim with the 2-byte-
 *            address SETUP_AW hack; Medusa should treat each record as "up to
 *            32 bytes", not a precise length.
 *  R-FIFO    SNIFF polls nrf_rx_payload in a loop with a 1 ms sleep on an empty
 *            FIFO. The nRF24 RX FIFO is only 3 deep; at 1 Mbps a burst can
 *            arrive faster than the poll + multi-SPI-transaction drain, so some
 *            frames may be DROPPED under heavy traffic. Acceptable for a
 *            presence/sample capture; if loss-free capture is needed, wire the
 *            nRF24 IRQ and drain on the data-ready interrupt (core1). Bench to
 *            see if it matters for your use.
 *  R-RSSI    The nRF24 has NO dBm RSSI — only the 1-bit RPD. SCAN reports the
 *            active CHANNEL (resp.active_chan); resp.rssi stays 0 by design.
 *  R-SPI2    core1 owns spi1 + the nRF24 exclusively; core0 never touches it
 *            (PING_REQ reads cached s_radio_ready). No shared IRQ: the hub CS
 *            edge ISR is core0's; the nRF24 has no IRQ pin wired. Simpler than
 *            the sx1262 blade (no RadioLib bank-IRQ to reconcile).
 *  R-MODE    hub link = spi0 slave MODE 1 (RP2040 one-byte-per-CS rule);
 *            radio = spi1 master MODE 0 (nRF24; nrf_shim sets CPOL0/CPHA0).
 *  R-PUMP    The hub pump is the GPS blade's proven design (CS-edge ISR form,
 *            same as cc1101). The nRF24 driver + SNIFF/SCAN are the unproven
 *            surface — bench against a known nRF24 transmitter.
 * ======================================================================== */
