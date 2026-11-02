/*
 * main.c — FHAdvanced-REMIX dedicated GPS blade, native Raspberry Pi Pico SDK.
 *
 * Native Raspberry Pi Pico C/C++ SDK GPS blade. Uses the SDK hardware SPI
 * slave directly (no Arduino layer).
 *
 * DUAL-CORE split (unchanged from the sketch):
 *   core1 — GPS: NMEA parse on uart1, stage the latest fh_gps_fix_t, and
 *           build the outbound frame (prepare_tx) exactly like loop1().
 *   core0 — SPI SLAVE link: full-duplex 261-byte transactions on spi0
 *           (SCK GP18 / MOSI-hub->blade GP16 / MISO-blade->hub GP19 /
 *           CS GP21), decode inbound frames (PING -> PONG), hand the staged
 *           frame to the hub on the next transaction.
 *
 * PROTOCOL (frozen, byte-identical fh_frame.c/h copied from the blade repo):
 *   - Hub is SPI MASTER @ 8 MHz, MODE1 (CPOL=0 CPHA=1), MSB first.
 *     (MODE1 is required: the RP2040 SSP slave transmits only ONE byte per CS
 *      assertion in MODE0 — datasheet §4.2.2 / pico-examples issue #115.)
 *   - Every poll = TWO transactions of exactly FH_MAX_FRAME (261) bytes:
 *     (1) hub clocks a request (body discarded — [G6] polled source),
 *     (2) hub clocks 261 bytes to read the blade's response.
 *   - The blade answers with fh_frame_encode(FH_TYPE_GPS_FIX, ...), zero-padded
 *     to 261 bytes; with no fresh fix it sends the all-zero prime buffer.
 *   - Inbound FH_TYPE_PING -> a FH_TYPE_PONG is staged and wins the next build.
 *
 * DEBUG: OFF by default. Define FH_DEBUG_UART=1 (cmake -DFH_DEBUG_UART=1) to
 * enable uart0 GP0/GP1 115200 debug output. NO USB stdio (no tinyusb needed).
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "pico/stdlib.h"
#include "pico/mutex.h"
#include "pico/multicore.h"
#include "hardware/uart.h"
#include "hardware/spi.h"
#include "hardware/resets.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

#include "fh_frame.h"
#include "fh_pico_pins_sdk.h"

#ifndef FH_DEBUG_UART
#define FH_DEBUG_UART 0
#endif

/* FH_GPS_SIM: compile-time simulated GPS. When 1, core1 skips the UART
 * entirely and injects a hardcoded, fully-valid fix into the normal
 * gps_stage_fix() path every cycle. Used for indoor bench bring-up when a
 * real sky view is impossible. 0 (default) = normal NMEA parse. */
#ifndef FH_GPS_SIM
#define FH_GPS_SIM 0
#endif

#define SPI_PORT spi0
#define GPS_PORT uart1
#define DBG_PORT uart0

/* ======================================================================== */
/* Shared staged state (both cores touch it -> ONE mutex, per [G7]).         */
/* ======================================================================== */
static mutex_t      s_stage_mtx;
static fh_gps_fix_t s_latest_fix;            /* core1 writes, core1 reads   */
static bool         s_have_fix = false;      /* core1-only                  */

/* Outbound frame: BUILT by core1 (prepare_tx, under s_stage_mtx), SNAPSHOT by
 * core0 between transactions (same mutex — the sketch's IRQ contract becomes
 * a between-transactions contract here). s_tx_ready is the publish flag. */
static uint8_t      s_tx[FH_SPI_XFER_BYTES];
static bool         s_tx_ready = false;

/* Pending-PONG flags: core0 writes (inbound decode), core1 reads+consumes.
 * Plain volatile — RP2040 aligned 32-bit accesses are atomic. */
static volatile bool    s_pong_pending = false;
static volatile uint8_t s_pong_seq = 0;

static void prepare_tx(void);
static void bench_blink_n(int n);   /* BENCH: blink narration (def below) */
static void on_in_frame(fh_type_t type, const uint8_t *payload,
                        uint8_t len, void *user);

/* ======================================================================== */
/* DEBUG (uart0, optional)                                                   */
/* ======================================================================== */
#if FH_DEBUG_UART
static void dbg_init(void) {
    uart_init(DBG_PORT, 115200);
    gpio_set_function(FH_PICO_DBG_TX, GPIO_FUNC_UART);
    gpio_set_function(FH_PICO_DBG_RX, GPIO_FUNC_UART);
}
static void dbg_puts(const char *s) { uart_puts(DBG_PORT, s); }
static void dbg_hex1(uint8_t v) {
    char b[5]; snprintf(b, sizeof b, "%02x ", v); dbg_puts(b);
}
#else
static inline void dbg_init(void) {}
static inline void dbg_puts(const char *s) { (void)s; }
static inline void dbg_hex1(uint8_t v) { (void)v; }
#endif

/* ======================================================================== */
/* GPS (core1) — NMEA parse (GGA + RMC), replacing TinyGPS++.                */
/* Same fields as the sketch: time/date validity, lat/lon, sats, hdop, fix.  */
/* ======================================================================== */
#define NMEA_BUF 128

typedef struct {
    /* GGA */
    bool     gga_valid;
    uint8_t  hh, mm, ss;
    int32_t  lat_e7, lon_e7;
    uint8_t  sats;
    uint8_t  quality;      /* GGA field 6: 0 = no fix, 1 = GPS, 2 = DGPS   */
    uint16_t hdop_x100;
    /* RMC */
    bool     rmc_valid;
    uint8_t  yy, mo, dd;   /* date from RMC                                */
} nmea_state_t;

static nmea_state_t s_nmea;

static bool nmea_field(const char *p, char *out, size_t cap) {
    while (*p == ',') { return false; }
    const char *e = strchr(p, ',');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n == 0) { return false; }
    if (n >= cap) { n = cap - 1; }
    memcpy(out, p, n); out[n] = 0;
    return true;
}
static const char *nmea_next(const char *p) {
    const char *e = strchr(p, ',');
    return e ? e + 1 : p + strlen(p);
}
/* NMEA ddmm.mmmm (lat) / dddmm.mmmm (lon) -> int32 degrees x 1e7.
 * FIX (bench 2026-11-01): was hardcoded 2-digit degrees for BOTH axes.
 * Latitude is 2-digit, but standard longitude is 3-digit (leading zero) — a
 * conforming module would have decoded as 2deg + minutes/60 (a ~17-degree
 * error). Derive the degree width from the decimal-point position instead:
 * integer digits before '.' = degrees + 2 minute digits. Handles both the
 * standard form and modules that drop the longitude's leading zero. */
static int32_t nmea_coord(const char *s, const char *hemi) {
    if (!s[0]) { return 0; }
    const char *dot = strchr(s, '.');
    if (!dot) { return 0; }
    int deglen = (int)(dot - s) - 2;      /* integer digits minus minutes */
    if (deglen < 1 || deglen > 3) { return 0; }
    char degs[4] = {0};
    memcpy(degs, s, deglen);
    double deg = (double)atoi(degs);
    const char *mins = s + deglen;
    char mbuf[16]; size_t i = 0;
    while (mins[i] && mins[i] != ',' && i < sizeof mbuf - 1) { mbuf[i] = mins[i]; ++i; }
    mbuf[i] = 0;
    deg += atof(mbuf) / 60.0;
    int32_t v = (int32_t)(deg * 1e7);
    if (hemi[0] == 'S' || hemi[0] == 'W') { v = -v; }
    return v;
}
static uint16_t nmea_u16(const char *s) { return (uint16_t)atoi(s); }
static uint8_t nmea_u8(const char *s)  { return (uint8_t)atoi(s); }

static bool nmea_checksum_ok(const char *line) {
    const char *star = strchr(line, '*');
    if (!star || star - line < 2) { return false; }
    uint8_t x = 0;
    for (const char *c = line + 1; c < star; ++c) { x ^= (uint8_t)*c; }
    unsigned int ck;
    if (sscanf(star + 1, "%2x", &ck) != 1) { return false; }
    return x == (uint8_t)ck;
}

static void nmea_process(const char *line) {
    if (line[0] != '$' || !nmea_checksum_ok(line)) { return; }
    char f[20];
    if (!strncmp(line, "$GPGGA", 6) || !strncmp(line, "$GNGGA", 6)) {
        const char *p = line + 7;
        if (nmea_field(p, f, sizeof f) && strlen(f) >= 6) {
            s_nmea.hh = (uint8_t)((f[0]-'0')*10 + (f[1]-'0'));
            s_nmea.mm = (uint8_t)((f[2]-'0')*10 + (f[3]-'0'));
            s_nmea.ss = (uint8_t)((f[4]-'0')*10 + (f[5]-'0'));
        }
        p = nmea_next(p);   /* lat ddmm.mmmm */
        const char *f_lat = p;
        p = nmea_next(p);   /* N/S */
        const char *f_nsh = p;
        p = nmea_next(p);   /* lon dddmm.mmmm */
        const char *f_lon = p;
        p = nmea_next(p);   /* E/W */
        const char *f_ewh = p;
        if (nmea_field(f_lat, f, sizeof f) && f[0]) {
            s_nmea.lat_e7 = nmea_coord(f, f_nsh);
        }
        if (nmea_field(f_lon, f, sizeof f) && f[0]) {
            s_nmea.lon_e7 = nmea_coord(f, f_ewh);
        }
        p = nmea_next(p);   /* fix quality */
        if (nmea_field(p, f, sizeof f)) { s_nmea.quality = nmea_u8(f); }
        p = nmea_next(p);   /* sats */
        if (nmea_field(p, f, sizeof f)) { s_nmea.sats = nmea_u8(f); }
        p = nmea_next(p);   /* hdop */
        if (nmea_field(p, f, sizeof f) && f[0]) {
            s_nmea.hdop_x100 = (uint16_t)(atof(f) * 100.0);
        }
        s_nmea.gga_valid = true;
    } else if (!strncmp(line, "$GPRMC", 6) || !strncmp(line, "$GNRMC", 6)) {
    const char *p = line + 7;             /* field 1: time (unused; GGA has it) */
    p = nmea_next(p);                     /* field 2: status */
    if (nmea_field(p, f, sizeof f)) { s_nmea.rmc_valid = (f[0] == 'A'); }
    /* FIX (bench 2026-11-01): was TWO nmea_next() calls, landing on field 3
     * (latitude) and reading status from its first char — rmc_valid could
     * never be true, so gps_time stayed 0 and the hub never locked its RTC.
     * One advance lands on status. The 7 following nexts now correctly
     * reach field 9 (date). */
        p = nmea_next(p); p = nmea_next(p); p = nmea_next(p); /* lat,NS,lon */
        p = nmea_next(p); p = nmea_next(p);   /* EW, speed */
        p = nmea_next(p);                     /* course */
        p = nmea_next(p);                     /* date ddmmyy */
        if (nmea_field(p, f, sizeof f) && strlen(f) >= 6) {
            s_nmea.dd = (uint8_t)((f[0]-'0')*10 + (f[1]-'0'));
            s_nmea.mo = (uint8_t)((f[2]-'0')*10 + (f[3]-'0'));
            s_nmea.yy = (uint8_t)((f[4]-'0')*10 + (f[5]-'0'));
        }
    }
}

/* UTC-safe epoch from civil date+time (pico-sdk has no setenv/tzset/TZ). */
static uint32_t utc_epoch(uint16_t y, uint8_t mo, uint8_t d,
                          uint8_t h, uint8_t mi, uint8_t s) {
    /* Howard Hinnant's days_from_civil, unsigned variant. */
    long t_y = y - (mo <= 2);
    long era = (t_y >= 0 ? t_y : t_y - 399) / 400;
    long yoe = t_y - era * 400;
    long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + doe - 719468;
    return (uint32_t)((uint64_t)days * 86400 + h * 3600 + mi * 60 + s);
}

/* Build an fh_gps_fix_t from the current parse, then STAGE it (core1). */
static void gps_stage_fix(void) {
    fh_gps_fix_t f; memset(&f, 0, sizeof(f));
    if (s_nmea.gga_valid && s_nmea.rmc_valid) {
        f.gps_time = utc_epoch((uint16_t)(2000 + s_nmea.yy),
                               s_nmea.mo, s_nmea.dd,
                               s_nmea.hh, s_nmea.mm, s_nmea.ss);
    }
    if (s_nmea.quality > 0) {
        f.lat  = s_nmea.lat_e7;
        f.lon  = s_nmea.lon_e7;
    }
    f.sats = s_nmea.sats;
    if (s_nmea.quality > 0) {
        f.fix_qual = 1;
        if (s_nmea.hdop_x100 > 0 && s_nmea.hdop_x100 < 200) { f.fix_qual = 2; }
    } else {
        f.fix_qual = 0;
    }

    mutex_enter_blocking(&s_stage_mtx);
    memcpy(&s_latest_fix, &f, sizeof(f));
    s_have_fix = true;
    mutex_exit(&s_stage_mtx);

    gpio_put(FH_PICO_IRQ_OUT, true);   /* optional fresh-fix signal */
}

/* VERIFIED UBX packets (checksums hand-verified in gen1 BUILD_PLAN App. B). */
static const uint8_t UBX_CFG_RATE_5HZ[] = {
    0xB5,0x62,0x06,0x08,0x06,0x00,0xC8,0x00,0x01,0x00,0x01,0x00,0xDE,0x6A
};
static const uint8_t UBX_CFG_PRT_38400[] = {
    0xB5,0x62,0x06,0x00,0x14,0x00,0x01,0x00,0x00,0x00,0xD0,0x08,0x00,0x00,
    0x00,0x96,0x00,0x00,0x03,0x00,0x03,0x00,0x00,0x00,0x00,0x00,0x8F,0x70
};

static bool ubx_wait_ack(uint32_t timeout_ms);   /* forward decl: used by gps_try_5hz */

static void gps_try_5hz(void) {
    /* two-stage: CFG-PRT at 9600, switch host to 38400, then CFG-RATE.
     * Best-effort; a NEO-6M clone may ignore it — NMEA still parses at 1 Hz. */
    uart_write_blocking(GPS_PORT, UBX_CFG_PRT_38400, sizeof(UBX_CFG_PRT_38400));
    sleep_ms(100);
    uart_deinit(GPS_PORT);
    uart_init(GPS_PORT, 38400);
    gpio_set_function(FH_PICO_GPS_TX, GPIO_FUNC_UART);
    gpio_set_function(FH_PICO_GPS_RX, GPIO_FUNC_UART);
    /* v20: settle long enough for the module to finish its 9600-baud ACK
     * tail and drain whatever junk is buffered before sending CFG-RATE.
     * Bench 2026-11-01: the module ACCEPTED the 38400 port switch (NMEA
     * parses), so the 1 Hz result means CFG-RATE itself was dropped in the
     * 50 ms settle window. Also verify with an ACK-ACK poll and retry. */
    sleep_ms(300);
    while (uart_is_readable(GPS_PORT)) { (void)uart_getc(GPS_PORT); }
    bool acked = false;
    for (int tries = 0; tries < 3; ++tries) {
        uart_write_blocking(GPS_PORT, UBX_CFG_RATE_5HZ, sizeof(UBX_CFG_RATE_5HZ));
        if (ubx_wait_ack(300)) { acked = true; break; }
    }
    /* v20 BENCH: report the CFG-RATE verdict on the LED so a human can read it
     * at boot (no debug UART needed).
     *   ACK received        -> 3 quick blips   (module is 5 Hz-capable)
     *   NAK / no answer     -> 1 slow blink    (rate-locked clone; stays 1 Hz)
     * Runs before core0's LED activity begins, so the pattern is unambiguous. */
    gpio_init(FH_PICO_LED); gpio_set_dir(FH_PICO_LED, GPIO_OUT);
    if (acked) {
        for (int i = 0; i < 3; ++i) {
            gpio_put(FH_PICO_LED, 1); sleep_ms(80);
            gpio_put(FH_PICO_LED, 0); sleep_ms(80);
        }
    } else {
        gpio_put(FH_PICO_LED, 1); sleep_ms(700);
        gpio_put(FH_PICO_LED, 0);
    }
    sleep_ms(200);
    gpio_deinit(FH_PICO_LED);   /* hand the pin back to core0's LED usage */
}

/* Wait for a UBX ACK-ACK (B5 62 05 01 ...) with a timeout, draining any
 * buffered bytes first. Note: an ACK-NAK (05 00) is NOT accepted — a NAK
 * means the module refused CFG-RATE, and retrying the identical packet is
 * pointless, so we fall through to the next retry only on no-answer. */
static bool ubx_wait_ack(uint32_t timeout_ms) {
    int state = 0;
    uint64_t end = time_us_64() + (uint64_t)timeout_ms * 1000;
    while (time_us_64() < end) {
        if (!uart_is_readable(GPS_PORT)) { continue; }
        uint8_t c = uart_getc(GPS_PORT);
        switch (state) {
        case 0: state = (c == 0xB5) ? 1 : 0; break;
        case 1: state = (c == 0x62) ? 2 : (c == 0xB5 ? 1 : 0); break;
        case 2: state = (c == 0x05) ? 3 : 0; break;
        case 3: return (c == 0x01);   /* 05 01 = ACK-ACK */
        }
    }
    return false;
}

void core1_entry(void) {
    /* GPS: uart1 at 9600 first, then the UBX 5 Hz two-stage bump. */
    uart_init(GPS_PORT, 9600);
    gpio_set_function(FH_PICO_GPS_TX, GPIO_FUNC_UART);
    gpio_set_function(FH_PICO_GPS_RX, GPIO_FUNC_UART);
    sleep_ms(200);
    gps_try_5hz();

#if FH_GPS_SIM
    /* Simulated GPS: inject a hardcoded, fully-valid fix into the normal
     * gps_stage_fix() path so prepare_tx() emits a real-looking GPS_FIX
     * frame. No UART activity at all — this is pure indoor bench bring-up. */
    for (;;) {
        /* Static bench coordinates (London, UK) with a stable timestamp. */
        s_nmea.lat_e7  = 515000000;   /* 51.5 N   */
        s_nmea.lon_e7  = -12000000;   /* 0.12 W   */
        s_nmea.hh = 12; s_nmea.mm = 0; s_nmea.ss = 0;
        s_nmea.dd = 15; s_nmea.mo = 6; s_nmea.yy = 26;
        s_nmea.sats = 8; s_nmea.hdop_x100 = 90;   /* 0.9 -> fix_qual=2 */
        s_nmea.quality = 1;
        s_nmea.gga_valid = true;
        s_nmea.rmc_valid = true;
        gps_stage_fix();
        prepare_tx();
        sleep_ms(200);   /* 5 Hz cadence, same as the real parser */
    }
#else
    char line[NMEA_BUF]; size_t n = 0;
    uint32_t last_stage_ms = 0;
    for (;;) {
        while (uart_is_readable(GPS_PORT)) {
            char c = (char)uart_getc(GPS_PORT);
            if (c == '\n') {
                line[n] = 0;
                if (n > 0 && line[n-1] == '\r') { line[n-1] = 0; }
                nmea_process(line);
                n = 0;
            } else if (c != '$' || n == 0) {
                if (n < NMEA_BUF - 1) { line[n++] = c; }
            } else {
                /* '$' mid-sentence: restart the buffer */
                line[0] = c; n = 1;
            }
        }
        /* Stage at 5 Hz internally so a staged fix is always fresh regardless
         * of the hub's poll rate. prepare_tx() (re)builds the outbound s_tx
         * buffer — HERE in core1 context, never in an ISR (vendor contract
         * carried over from the sketch). Rebuild promptly on a pending PONG. */
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_stage_ms >= 200) {
            gps_stage_fix(); last_stage_ms = now; prepare_tx();
        } else if (s_pong_pending) {
            prepare_tx();
        }
    }
#endif /* FH_GPS_SIM */
}

/* ======================================================================== */
/* Outbound frame builder (core1, like the sketch's prepare_tx).             */
/* ======================================================================== */
static void prepare_tx(void) {
    uint8_t local[FH_SPI_XFER_BYTES];
    size_t n = 0;

    if (s_pong_pending) {
        fh_ping_t p; p.seq = s_pong_seq;
        s_pong_pending = false;               /* consume */
        n = fh_frame_encode(FH_TYPE_PONG, (uint8_t *)&p, sizeof(p),
                            local, sizeof(local));
    } else if (s_have_fix) {
        fh_gps_fix_t fix;
        mutex_enter_blocking(&s_stage_mtx);
        memcpy(&fix, &s_latest_fix, sizeof(fix));
        mutex_exit(&s_stage_mtx);
        n = fh_frame_encode(FH_TYPE_GPS_FIX, (uint8_t *)&fix, sizeof(fix),
                            local, sizeof(local));
        gpio_put(FH_PICO_IRQ_OUT, false);     /* fresh-fix signal answered */
    } else {
        /* BENCH: nothing staged — send a recognizable heartbeat frame so the
         * hub's raw rx dump proves the transport is alive end-to-end.
         * REMOVE once a real fix flows (then n==0 sends the zero buffer). */
        fh_ping_t hb; hb.seq = 0xA5;
        n = fh_frame_encode(FH_TYPE_PONG, (uint8_t *)&hb, sizeof(hb),
                            local, sizeof(local));
    }

    /* Publish into s_tx: zero-pad then copy (core0 only snapshots s_tx under
     * the same mutex — the sketch's "IRQ only reads s_tx" contract). */
    uint8_t pad[FH_SPI_XFER_BYTES]; memset(pad, 0, sizeof(pad));
    if (n) { memcpy(pad, local, n); }
    mutex_enter_blocking(&s_stage_mtx);
    memcpy(s_tx, pad, sizeof(s_tx));
    s_tx_ready = true;
    mutex_exit(&s_stage_mtx);
}

/* ======================================================================== */
/* SPI SLAVE link (core0) — 261-byte full-duplex transactions on spi0.       */
/* ======================================================================== */
static fh_decoder_t s_in_dec;
static uint8_t      s_rx[FH_SPI_XFER_BYTES];

/* BENCH: LED = solid 1 s after a completed 261-byte transaction. */
static volatile uint32_t s_last_recv_ms = 0;
static volatile uint32_t s_cs_edges = 0;

/* CS edge tracking: core0 processes these (single consumer); the ISR only
 * sets flags. Falling edge realigns the byte counter to the CS boundary. */
static volatile bool s_cs_fell = false;
static volatile bool s_cs_rose = false;

static volatile bool s_pump_inside = false;   /* pump owns the FIFO right now */
static uint8_t s_pump_tx[FH_SPI_XFER_BYTES]; /* defined below (outbound buffer) */
static int spi_reset_and_preload(const uint8_t *tx);   /* defined below */

static void cs_gpio_isr(uint gpio, uint32_t events) {
    (void)gpio;
    if (events & GPIO_IRQ_EDGE_FALL) {
        s_cs_fell = true;
        /* v13: NO preload here. The reset+preload runs in the pump entry while
         * the bus is IDLE (after CS rise, before the next CS fall). Toggling
         * the SSP enable inside the CS-fall ISR would race the master's first
         * clocked byte — the pump-entry path guarantees fresh preload in time. */
    }
    if (events & GPIO_IRQ_EDGE_RISE) { s_cs_rose = true; s_cs_edges++; }
}

/* Full-duplex pump for exactly FH_SPI_XFER_BYTES (261) bytes. tx bytes are
 * pushed as the master clocks (FIFO-writable gated), so the FIFO never runs
 * ahead of what the hub consumes: exactly 261 tx bytes pushed per 261 clocked.
 * The idle gap between the hub's two transactions preloads the FIFO (8 deep)
 * with the first tx bytes — equivalent to the sketch's setData() priming. */
static volatile int s_rx_len = 0;    /* bytes clocked in the last transaction */
static volatile int s_pushed = 0;    /* BENCH: tx bytes pushed in the last txn */
static volatile uint32_t s_txn_count = 0;   /* BENCH: completed transactions */

/* v14: FULL SPI0 peripheral reset between transactions. Bench history:
 *   v11  plain preload  -> only byte 0 escaped (the paired push leaves ~8
 *                          unclocked bytes in the TX FIFO at CS-rise, so the
 *                          preload lands on top of residue and underruns).
 *   v13  SSE toggle     -> bit-slipped garbage (toggling the enable races
 *                          the master's clock and leaves shift-reg residue).
 * reset_block() clears BOTH FIFOs and the shift register; spi_init() re-arms
 * the SSP (slave, mode 0) and we preload a clean 8 bytes. Runs ONLY while the
 * bus is idle (after CS rise; the hub waits >= 5 ms before the next CS, the
 * whole sequence takes tens of us). Returns the number of bytes preloaded. */
static int spi_reset_and_preload(const uint8_t *tx) {
    reset_block(RESETS_RESET_SPI0_BITS);
    unreset_block_wait(RESETS_RESET_SPI0_BITS);
    spi_init(SPI_PORT, 8000000);            /* re-arm: 8-bit, mode 1, enabled */
    spi_set_slave(SPI_PORT, true);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(FH_PICO_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(FH_PICO_SPI_MOSI, GPIO_FUNC_SPI);   /* hub -> blade RX  */
    gpio_set_function(FH_PICO_SPI_MISO, GPIO_FUNC_SPI);   /* blade -> hub TX  */
    gpio_set_function(FH_PICO_SPI_CS,   GPIO_FUNC_SPI);
    int n = 0;
    while (n < 8 && spi_is_writable(SPI_PORT)) {
        spi_get_hw(SPI_PORT)->dr = tx[n++];
    }
    return n;
}

static void spi_txn_pump(const uint8_t *tx_active) {
    int tx_next = 0;
    int rx_next = 0;
    s_pump_inside = true;

    /* v14: full SPI0 reset (clears both FIFOs + shift register) then preload
     * 8 fresh bytes — runs while the bus is idle, well inside the hub's >=5 ms
     * gap between transactions. */
    tx_next = spi_reset_and_preload(tx_active);

    absolute_time_t t0 = get_absolute_time();
    for (;;) {
        /* CS edge events (single consumer).
         * COMPLETE ON CS RISE — ANY length, INCLUDING zero-byte transactions.
         * (The old code skipped 0-length completions, which could wedge the
         * pump forever if the ISR fired but no bytes were clocked.) */
        if (s_cs_fell) { s_cs_fell = false; rx_next = 0; }
        if (s_cs_rose) {
            s_cs_rose = false;
            s_last_recv_ms = to_ms_since_boot(get_absolute_time());
            s_rx_len = rx_next;
            s_pushed = tx_next;
            s_txn_count++;
            s_pump_inside = false;
            return;
        }

        if (spi_is_readable(SPI_PORT)) {
            s_rx[rx_next++] = (uint8_t)(spi_get_hw(SPI_PORT)->dr);
            /* PAIRED push: one TX byte per RX byte received. */
            if (tx_next < FH_SPI_XFER_BYTES && spi_is_writable(SPI_PORT)) {
                spi_get_hw(SPI_PORT)->dr = tx_active[tx_next++];
            }
            if (rx_next == FH_SPI_XFER_BYTES) {
                s_last_recv_ms = to_ms_since_boot(get_absolute_time());
                s_rx_len = FH_SPI_XFER_BYTES;
                s_pushed = tx_next;
                s_txn_count++;
                s_pump_inside = false;
                return;
            }
        } else if (absolute_time_diff_us(t0, get_absolute_time()) > 200000) {
            /* WATCHDOG: no transaction for 200 ms — return anyway so the pump
             * can never wedge inside (it re-enters and preloads fresh). */
            s_rx_len = rx_next;
            s_pushed = tx_next;
            s_pump_inside = false;
            return;
        } else {
            tight_loop_contents();
        }
    }
}

/* Between-transaction bookkeeping: snapshot the freshest published frame,
 * decode the inbound bytes (PING -> PONG flag). Returns the tx buffer the
 * NEXT transaction must clock out. */
static const uint8_t *spi_after_txn(const uint8_t *rx, int rx_len) {
    /* 1) decode inbound control frame (core0 context, like the sketch loop()) */
    fh_decoder_push(&s_in_dec, rx, (size_t)rx_len, on_in_frame, NULL);

    /* 2) snapshot the latest published s_tx for the next transaction */
    mutex_enter_blocking(&s_stage_mtx);
    memcpy(s_pump_tx, s_tx, sizeof(s_pump_tx));
    mutex_exit(&s_stage_mtx);

    /* v16: REAL FRAMES. The bench ramp proved the MODE 1 link streams the
     * full 261 bytes byte-identical, so the diagnostic buffer is retired and
     * the staged frame (s_tx, built by core1's prepare_tx) flows instead:
     * a fresh fix  -> fh_frame_encode(FH_TYPE_GPS_FIX, ...);
     * no fix yet   -> the all-zero prime buffer (the decoder skips pads). */
    return s_pump_tx;
}

static void on_in_frame(fh_type_t type, const uint8_t *payload,
                        uint8_t len, void *user) {
    (void)user;
    if (type == FH_TYPE_PING && len >= sizeof(fh_ping_t)) {
        s_pong_seq = payload[0];
        s_pong_pending = true;    /* core1's prepare_tx honors it next build */
    }
    /* other control (time/etc.): parked */
}

void core0_main(void) {
    fh_decoder_init(&s_in_dec);
    bench_blink_n(6);              /* step 5 done + BENCH-v10 signature blink */

    /* LED */
    gpio_init(FH_PICO_LED);
    gpio_set_dir(FH_PICO_LED, GPIO_OUT);
    gpio_put(FH_PICO_LED, false);

    /* optional IRQ OUT: idle LOW */
    gpio_init(FH_PICO_IRQ_OUT);
    gpio_set_dir(FH_PICO_IRQ_OUT, GPIO_OUT);
    gpio_put(FH_PICO_IRQ_OUT, false);

    /* bench boot blinks: 5x = new firmware is running */
    for (int i = 0; i < 5; ++i) {
        gpio_put(FH_PICO_LED, true);  sleep_ms(80);
        gpio_put(FH_PICO_LED, false); sleep_ms(80);
    }

    /* spi0 as an 8 MHz MODE1 (CPHA=1) MSB-first SLAVE, hardware CS on GP21. */
    spi_init(SPI_PORT, 8000000);
    spi_set_slave(SPI_PORT, true);
    /* MODE 1 (CPOL=0, CPHA=1) — RP2040 slave-mode datasheet requirement: with
     * SPH=0 the slave select freezes the serial peripheral register and only
     * ONE byte per CS assertion transmits (bench-proven v11-v14: byte 0 then
     * underrun). CPHA=1 streams all bytes with CS held low. Hub must match. */
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(FH_PICO_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(FH_PICO_SPI_MOSI, GPIO_FUNC_SPI);   /* hub -> blade RX */
    gpio_set_function(FH_PICO_SPI_MISO, GPIO_FUNC_SPI);   /* blade -> hub TX */
    gpio_set_function(FH_PICO_SPI_CS,   GPIO_FUNC_SPI);

    /* CS edge tracking on GP21 (bench + boundary realignment). */
    gpio_init(FH_PICO_SPI_CS);
    gpio_set_dir(FH_PICO_SPI_CS, GPIO_IN);
    gpio_set_function(FH_PICO_SPI_CS, GPIO_FUNC_SPI);
    gpio_set_irq_enabled_with_callback(FH_PICO_SPI_CS,
                                       GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                       true, cs_gpio_isr);

    /* main loop: pump transactions, bookkeep between them. The FIRST
     * transaction clocks out the all-zero prime buffer (prepare_tx in main()
     * published it). */
    uint8_t local_rx[FH_SPI_XFER_BYTES];
    const uint8_t *tx_active = s_pump_tx;
    {
        mutex_enter_blocking(&s_stage_mtx);
        memcpy(s_pump_tx, s_tx, sizeof(s_pump_tx));
        mutex_exit(&s_stage_mtx);
    }
    for (;;) {
        /* BENCH LED: ON = the pump is inside a transaction loop.
         *   Mostly dark  = transactions complete fast (healthy)
         *   Mostly lit   = the pump is STUCK waiting inside a transaction
         * (the old 3-state LED logic is parked for the bench build) */
        gpio_put(FH_PICO_LED, true);
        spi_txn_pump(tx_active);
        gpio_put(FH_PICO_LED, false);
        memcpy(local_rx, s_rx, FH_SPI_XFER_BYTES);
        tx_active = spi_after_txn(local_rx, s_rx_len);
    }
}

/* BENCH: blink narration — N fast blinks = "step N survived". */
static void bench_blink_n(int n) {
    gpio_init(FH_PICO_LED); gpio_set_dir(FH_PICO_LED, GPIO_OUT);
    for (int i = 0; i < n; ++i) {
        gpio_put(FH_PICO_LED, true);  sleep_ms(80);
        gpio_put(FH_PICO_LED, false); sleep_ms(120);
    }
    sleep_ms(600);
}

int main(void) {
    mutex_init(&s_stage_mtx);
    memset(&s_latest_fix, 0, sizeof(s_latest_fix));
    memset(s_tx, 0, sizeof(s_tx));
    bench_blink_n(1);              /* step 1: init done */
    dbg_init();
    bench_blink_n(2);              /* step 2: debug uart done */

    /* Prime the first outbound buffer (all-zero, like the sketch). */
    prepare_tx();

    /* Launch GPS on core1 (shares the mutex + pong flags). */
    bench_blink_n(3);              /* step 3: prepare_tx done */
    multicore_launch_core1(core1_entry);
    bench_blink_n(4);              /* step 4: core1 launched */

    core0_main();   /* never returns */
    return 0;
}
