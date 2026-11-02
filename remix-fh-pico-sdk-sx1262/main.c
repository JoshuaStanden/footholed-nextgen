/*
 * main.c — FHAdvanced-REMIX SX1262 LoRa blade, native Raspberry Pi Pico SDK.
 *
 * UNVERIFIED FIRST DRAFT — reasoned-not-compiled. Pilot's sibling: the LoRa
 * pico-sdk port. LoRa is the COMMS link to the sensor stick (TX is CORE), not
 * a capture radio.
 *
 * TWO SPI BLOCKS (decision 2026-11-01):
 *   core0 — HUB LINK: spi0 SLAVE, MODE 1, the BENCH-PROVEN GPS pump copied
 *           verbatim (full SPI0 reset + 8-byte preload + paired RX/TX push +
 *           CS realign). ONE DIFFERENCE from the GPS blade: CS is driven by
 *           POLLING the pin level, NOT a GPIO edge IRQ — because RadioLib's
 *           PicoHal (core1) owns the IO_IRQ_BANK0 raw handler for DIO1, and the
 *           RP2040 will not let gpio_set_irq_enabled_with_callback coexist with
 *           it. core0 therefore registers NO bank IRQ. (See R-IRQBANK.)
 *   core1 — RADIO: spi1 MASTER to the Wio-SX1262 via RadioLib + PicoHal, in the
 *           sx_shim.cpp extern-"C" wrappers. Runs fired LoRa TX, the listen
 *           window (polls for RX -> unsolicited RX_IND), and LISTEN on/off.
 *
 * SINGLE-WRITER: ONLY core0 calls stage_frame(). core1 produces results into
 * mailboxes; core0 drains + stages them (same discipline as the cc1101 port).
 *
 * KNOWN RISKS at EOF. The hub pump is the proven part; the RadioLib-on-SDK
 * radio and the bank-IRQ/core split are the bench surface.
 */
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/mutex.h"
#include "pico/multicore.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/resets.h"
#include "hardware/sync.h"      /* __dmb() */

#include "fh_frame.h"
#include "fh_ondemand.h"
#include "fh_pico_pins_sdk.h"

/* ---- C shim over the C++ RadioLib SX1262 driver (sx_shim.cpp). main.c is C
 * and MUST NOT include RadioLib's C++ headers; reach the radio ONLY here. ---- */
extern bool sx_init(float mhz);
extern bool sx_present(void);
extern int  sx_tx(const uint8_t *data, int len, int *out_status);
extern int  sx_listen(int enable);
extern int  sx_rx_poll(uint8_t *data, int cap, int *rssi, int *snr);

#define SPI_HUB   spi0

/* ======================================================================== */
/* HUB LINK — spi0 slave pump, COPIED FROM THE PROVEN GPS BLADE.             */
/* (CS polled, not IRQ — the one change from GPS; see header + R-IRQBANK.)   */
/* ======================================================================== */
static mutex_t  s_stage_mtx;
static uint8_t  s_tx[FH_SPI_XFER_BYTES];      /* published outbound frame     */
static uint8_t  s_pump_tx[FH_SPI_XFER_BYTES]; /* core0 snapshot for the pump  */
static uint8_t  s_rx[FH_SPI_XFER_BYTES];
static fh_decoder_t s_in_dec;
static volatile int s_rx_len = 0;

/* core1 init handshake: core1 sets this once the radio is up (core0 reads it
 * for the PING_REQ radios_ready bit; core0 never touches spi1). */
static volatile bool s_radio_ready = false;

/* --- fired LoRa TX: core0 (decode) -> core1 (execute) -> core0 (stage) --- */
static volatile bool    s_tx_pending = false;   /* core0 sets, core1 clears   */
static volatile bool    s_tx_ready   = false;   /* core1 sets, core0 clears   */
static struct { uint8_t seq, flags, len; uint8_t data[FH_OD_LORA_PAYLOAD_MAX]; } s_tx_req;
static uint8_t          s_tx_respbuf[sizeof(fh_od_lora_tx_resp_t)];
static volatile uint8_t s_tx_resp_len = 0;

/* --- LISTEN enable/disable: core0 -> core1 -> core0 stages the RESP --- */
static volatile bool    s_lis_pending = false;
static volatile bool    s_lis_ready   = false;
static volatile uint8_t s_lis_seq     = 0;
static volatile uint8_t s_lis_enable  = 0;
static volatile uint32_t s_lis_window_ms = 0;
static uint8_t          s_lis_respbuf[sizeof(fh_od_lora_listen_resp_t)];
static volatile uint8_t s_lis_resp_len = 0;

/* --- unsolicited RX_IND: core1 captures a frame, core0 stages it --- */
static uint8_t          s_rxind_buf[sizeof(fh_od_lora_rx_ind_t)];
static volatile uint8_t s_rxind_len   = 0;
static volatile bool    s_rxind_ready = false;  /* core1 sets, core0 clears   */

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
    return s_tx_pending || s_tx_ready || s_lis_pending || s_lis_ready;
}

static void on_in_frame(fh_type_t type, const uint8_t *payload, uint8_t len, void *user) {
    (void)user;
    if (type == FH_TYPE_PING && len >= 1) {
        fh_ping_t p; p.seq = payload[0];
        stage_frame(FH_TYPE_PONG, (uint8_t*)&p, sizeof(p));
    } else if ((uint8_t)type == FH_OD_PING_REQ && len >= 1) {
        fh_od_ping_resp_t r; memset(&r, 0, sizeof(r));
        r.seq = payload[0];
        r.radios_ready = s_radio_ready ? FH_OD_RADIO_SX1262 : 0;
        stage_frame((fh_type_t)FH_OD_PING_RESP, (uint8_t*)&r, sizeof(r));
    } else if ((uint8_t)type == FH_OD_LORA_LISTEN_REQ
               && len >= sizeof(fh_od_lora_listen_req_t)) {
        fh_od_lora_listen_req_t q; memcpy(&q, payload, sizeof(q));
        if (!s_radio_ready) {
            fh_od_lora_listen_resp_t r; memset(&r, 0, sizeof(r));
            r.seq = q.seq; r.status = FH_OD_RADIO_FAIL; r.listening = 0;
            stage_frame((fh_type_t)FH_OD_LORA_LISTEN_RESP, (uint8_t*)&r, sizeof(r));
        } else if (radio_busy()) {
            fh_od_lora_listen_resp_t r; memset(&r, 0, sizeof(r));
            r.seq = q.seq; r.status = FH_OD_BUSY; r.listening = 0;
            stage_frame((fh_type_t)FH_OD_LORA_LISTEN_RESP, (uint8_t*)&r, sizeof(r));
        } else {
            s_lis_seq = q.seq; s_lis_enable = q.enable;
            s_lis_window_ms = q.window_ms;
            __dmb();
            s_lis_pending = true;   /* core1 applies the state change */
        }
    } else if ((uint8_t)type == FH_OD_LORA_TX_REQ && len >= 3) {
        fh_od_lora_tx_req_t q; memset(&q, 0, sizeof(q));
        memcpy(&q, payload, len > sizeof(q) ? sizeof(q) : len);
        if (radio_busy()) {
            fh_od_lora_tx_resp_t r; memset(&r, 0, sizeof(r));
            r.seq = q.seq; r.status = FH_OD_BUSY; r.toa_ms = 0;
            stage_frame((fh_type_t)FH_OD_LORA_TX_RESP, (uint8_t*)&r, sizeof(r));
        } else {
            /* Clamp real data length to what the frame carried AND the bound
             * (never trust q.len blindly). seq+flags+len header = 3 bytes. */
            uint8_t avail = (uint8_t)(len - 3);
            uint8_t n = q.len < avail ? q.len : avail;
            if (n > FH_OD_LORA_PAYLOAD_MAX) n = FH_OD_LORA_PAYLOAD_MAX;
            memcpy(s_tx_req.data, q.data, n);
            s_tx_req.len = n; s_tx_req.flags = q.flags; s_tx_req.seq = q.seq;
            __dmb();
            s_tx_pending = true;    /* core1 transmits */
        }
    }
}

/* CS level now (polled). The hub asserts CS low during a transaction. */
static inline bool cs_asserted(void) { return gpio_get(FH_PICO_SPI_CS) == 0; }

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

/* One full-duplex transaction, CS tracked by POLLING (no edge IRQ). The pump
 * resets+preloads while the bus is idle, waits for CS to assert, then clocks
 * paired RX/TX until 261 bytes or CS de-asserts. Mirrors the GPS pump's
 * structure; only the CS-edge source changed from ISR flags to level polls. */
static void spi_txn_pump(const uint8_t *tx_active) {
    int tx_next = spi_reset_and_preload(tx_active);
    int rx_next = 0;
    /* wait out any in-progress assertion, then for a fresh CS assert (idle
     * gap is tens of us..ms; bounded so the pump can never wedge). */
    absolute_time_t tw = get_absolute_time();
    while (cs_asserted()) {
        if (absolute_time_diff_us(tw, get_absolute_time()) > 200000) break;
        tight_loop_contents();
    }
    tw = get_absolute_time();
    while (!cs_asserted()) {
        if (absolute_time_diff_us(tw, get_absolute_time()) > 200000) { s_rx_len = 0; return; }
        tight_loop_contents();
    }
    /* CS asserted: clock the transaction. */
    absolute_time_t t0 = get_absolute_time();
    for (;;) {
        if (!cs_asserted()) { s_rx_len = rx_next; return; }   /* CS rose: done */
        if (spi_is_readable(SPI_HUB)) {
            s_rx[rx_next++] = (uint8_t)(spi_get_hw(SPI_HUB)->dr);
            if (tx_next < FH_SPI_XFER_BYTES && spi_is_writable(SPI_HUB)) {
                spi_get_hw(SPI_HUB)->dr = tx_active[tx_next++];
            }
            if (rx_next == FH_SPI_XFER_BYTES) { s_rx_len = FH_SPI_XFER_BYTES; return; }
        } else if (absolute_time_diff_us(t0, get_absolute_time()) > 200000) {
            s_rx_len = rx_next; return;   /* watchdog */
        } else {
            tight_loop_contents();
        }
    }
}

static const uint8_t *spi_after_txn(const uint8_t *rx, int rx_len) {
    fh_decoder_push(&s_in_dec, rx, (size_t)rx_len, on_in_frame, NULL);

    /* Single-writer staging, priority order. SOLICITED responses first: the
     * hub issued a TX/LISTEN and is actively awaiting its RESP, so those must
     * not starve behind a stream of unsolicited RX_INDs during a busy listen
     * window. The RX_IND is staged only when no solicited reply is waiting.
     * Only one frame per transaction — the hub polls, the rest wait one. */
    if (s_tx_ready) {
        __dmb();
        stage_frame((fh_type_t)FH_OD_LORA_TX_RESP, s_tx_respbuf, s_tx_resp_len);
        s_tx_ready = false;
    } else if (s_lis_ready) {
        __dmb();
        stage_frame((fh_type_t)FH_OD_LORA_LISTEN_RESP, s_lis_respbuf, s_lis_resp_len);
        s_lis_ready = false;
    } else if (s_rxind_ready) {
        __dmb();
        stage_frame((fh_type_t)FH_OD_LORA_RX_IND, s_rxind_buf, s_rxind_len);
        s_rxind_ready = false;
    }

    mutex_enter_blocking(&s_stage_mtx);
    memcpy(s_pump_tx, s_tx, sizeof(s_pump_tx));
    mutex_exit(&s_stage_mtx);
    return s_pump_tx;
}

/* ======================================================================== */
/* core1 — RADIO op executor (RadioLib SX1262 on spi1, via sx_shim.cpp).     */
/* ======================================================================== */
static uint8_t s_rx_seq = 0;             /* our own RX_IND running counter    */
static bool    s_listening = false;      /* core1-only                        */
static uint32_t s_listen_until = 0;      /* 0 = continuous; else ms deadline  */

void core1_entry(void) {
    bool ok = sx_init(915.0f);           /* North America ISM band (915 MHz) */
    s_radio_ready = ok;                  /* publish for core0's PING_REQ       */

    for (;;) {
        /* 1) LISTEN state change */
        if (s_lis_pending) {
            __dmb();
            fh_od_lora_listen_resp_t r; memset(&r, 0, sizeof(r));
            r.seq = s_lis_seq;
            if (!sx_present()) { r.status = FH_OD_RADIO_FAIL; r.listening = 0; }
            else {
                int st = sx_listen(s_lis_enable);
                s_listening = (st != 0);
                s_listen_until = (s_lis_enable && s_lis_window_ms)
                               ? (to_ms_since_boot(get_absolute_time()) + s_lis_window_ms)
                               : 0;
                r.status = FH_OD_OK; r.listening = (uint8_t)(s_listening ? 1 : 0);
            }
            memcpy(s_lis_respbuf, &r, sizeof(r));
            s_lis_resp_len = (uint8_t)sizeof(r);
            __dmb();
            s_lis_ready   = true;
            s_lis_pending = false;
            continue;
        }

        /* 2) fired LoRa TX */
        if (s_tx_pending) {
            __dmb();
            fh_od_lora_tx_resp_t r; memset(&r, 0, sizeof(r));
            r.seq = s_tx_req.seq;
            if (!sx_present()) { r.status = FH_OD_RADIO_FAIL; r.toa_ms = 0; }
            else {
                int status = 0;
                int toa = sx_tx(s_tx_req.data, s_tx_req.len, &status);
                r.status = status ? FH_OD_OK : FH_OD_RADIO_FAIL;
                r.toa_ms = (uint16_t)toa;
            }
            memcpy(s_tx_respbuf, &r, sizeof(r));
            s_tx_resp_len = (uint8_t)sizeof(r);
            __dmb();
            s_tx_ready   = true;
            s_tx_pending = false;
            continue;
        }

        /* 3) listen window: end a bounded window, else poll for a frame */
        if (s_listening && sx_present()) {
            if (s_listen_until != 0 &&
                (int32_t)(to_ms_since_boot(get_absolute_time()) - s_listen_until) >= 0) {
                sx_listen(0);
                s_listening = false;
            } else if (!s_rxind_ready) {   /* don't overwrite an undrained one */
                uint8_t buf[FH_OD_LORA_PAYLOAD_MAX];
                int rssi = 0, snr = 0;
                int n = sx_rx_poll(buf, sizeof(buf), &rssi, &snr);
                if (n > 0) {
                    fh_od_lora_rx_ind_t ind; memset(&ind, 0, sizeof(ind));
                    ind.seq  = s_rx_seq++;
                    ind.rssi = (int8_t)rssi;
                    ind.snr  = (int8_t)snr;
                    ind.len  = (n > (int)sizeof(ind.data)) ? (uint8_t)sizeof(ind.data) : (uint8_t)n;
                    memcpy(ind.data, buf, ind.len);
                    memcpy(s_rxind_buf, &ind, sizeof(ind));
                    /* RX_IND wire length = seq+rssi+snr+len + data */
                    s_rxind_len = (uint8_t)(4 + ind.len);
                    __dmb();
                    s_rxind_ready = true;
                }
            }
        }

        sleep_ms(2);
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

    /* optional IRQ-out to hub, idle low */
    gpio_init(FH_PICO_IRQ_OUT); gpio_set_dir(FH_PICO_IRQ_OUT, GPIO_OUT);
    gpio_put(FH_PICO_IRQ_OUT, 0);

    /* hub-link spi0 slave, MODE 1 (same as GPS). CS as a PLAIN INPUT we poll
     * (NOT a GPIO IRQ) so IO_IRQ_BANK0 is free for RadioLib's PicoHal on
     * core1. See R-IRQBANK. */
    spi_init(SPI_HUB, 8000000);
    spi_set_slave(SPI_HUB, true);
    spi_set_format(SPI_HUB, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(FH_PICO_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(FH_PICO_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(FH_PICO_SPI_MISO, GPIO_FUNC_SPI);
    gpio_set_function(FH_PICO_SPI_CS,   GPIO_FUNC_SPI);
    gpio_init(FH_PICO_SPI_CS); gpio_set_dir(FH_PICO_SPI_CS, GPIO_IN);
    gpio_set_function(FH_PICO_SPI_CS, GPIO_FUNC_SPI);

    /* launch the radio executor on core1 (owns spi1 + RadioLib + DIO1 IRQ) */
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
 *  R-IRQBANK  THE central design call. RadioLib's PicoHal installs a shared
 *             IO_IRQ_BANK0 raw handler (gpio_add_raw_irq_handler_masked64) for
 *             the SX1262 DIO1 line. The RP2040 will NOT let that coexist with
 *             gpio_set_irq_enabled_with_callback (the GPS pump's CS-edge API) —
 *             one asserts over the other. RESOLUTION: RadioLib+PicoHal (incl.
 *             its DIO1 bank IRQ) run ENTIRELY on core1; core0's hub pump drives
 *             CS by POLLING the pin level (cs_asserted()), registering NO bank
 *             IRQ. So IO_IRQ_BANK0 is PicoHal's alone. BENCH: confirm CS
 *             polling keeps up with the hub's 8 MHz clock at 261 bytes (the
 *             GPS pump used an edge ISR only to realign; the full-reset+preload
 *             per transaction is what actually makes the stream work, and that
 *             is preserved). If polling slips, the fallback is to move the hub
 *             pump's CS realign to a core0 PIO program (no bank IRQ) — NOT done.
 *  R-TONE     PicoHal::noTone() calls multicore_reset_core1(). This blade never
 *             calls tone()/noTone(); do NOT add them — it would reset the radio
 *             core. (RadioLib's SX1262 path does not call them.)
 *  R-RLIB     RadioLib SX1262 API via sx_shim.cpp. VERSION FORK CONFIRMED in
 *             the multi-check: this shim calls the POSITIONAL begin(freq,bw,sf,
 *             cr,sync,power,preamble,tcxo,ldo). That overload is present but
 *             DEPRECATED in 7.8.1 (the project's pin) and REMOVED on RadioLib
 *             master (replaced by begin(const ConfigLoRa_t&), which the
 *             NonArduino/Pico example uses as begin({})). The shim has a
 *             compile-time guard (#error if RADIOLIB_VERSION_MAJOR > 7). Build
 *             against 7.8.1; to move to 8.x, port begin() to the ConfigLoRa_t
 *             designated-initializer form. Other calls used: transmit(),
 *             startReceive(), standby(), getPacketLength(), readData() (returns
 *             a STATUS code, length comes from getPacketLength), getRSSI(),
 *             getSNR(). begin() -707 = TCXO; -2 = SPI/DIO wiring.
 *  R-LINK     main.c is C; RadioLib + PicoHal are C++. The radio is reached
 *             ONLY via sx_shim.cpp extern "C". main.c does NOT include any
 *             RadioLib header. Confirm the mixed C/C++ link on the first build.
 *  R-MODE     hub link = spi0 slave MODE 1 (RP2040 one-byte-per-CS rule);
 *             radio = spi1 master MODE 0 (SX1262; PicoHal sets CPOL0/CPHA0). Do
 *             NOT unify them.
 *  R-RFSW     RF_SW1 (GP6) sense: HIGH=TX / LOW=RX assumed (gen1 idled LOW).
 *             VERIFY on your Wio module rev. Antenna attached before any TX.
 *  R-PUMP     The hub pump is the GPS blade's proven design MINUS the CS edge
 *             ISR (now polled). The RadioLib-on-SDK radio + the LoRa verbs are
 *             the unproven surface — bench against your sensor stick.
 *  R-RXPOLL   The listen window polls getPacketLength() on core1; PicoHal's
 *             DIO1 bank IRQ drives RadioLib's internal packet flag. Confirm a
 *             received frame actually sets it (vs needing an explicit IRQ-flag
 *             read) on your RadioLib version — the arduino blade used
 *             checkDataReady(); if getPacketLength() alone does not latch, add
 *             the explicit IRQ-flag gate in sx_rx_poll.
 *  R-LISTEN   Listen state is tracked in TWO places — sx_shim.cpp's s_listening
 *             (drives whether sx_tx resumes RX after a transmit) and main.c
 *             core1's s_listening (drives the poll + window deadline). They are
 *             kept in sync because EVERY transition goes through sx_listen().
 *             Correct today, but fragile: do not set either copy directly —
 *             always call sx_listen().
 * ======================================================================== */
