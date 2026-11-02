/*
 * fh_hub_time.c — FHAdvanced hub time + position base (SUBSYSTEM 2) impl.
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule). See KNOWN RISKS end.
 *
 * ARCHITECTURE CHANGE (2026-10-22): GPS is NO LONGER wired to the hub. GPS now
 * lives on the Pico node (see FH-Advanced-Pico-rescue-gps.zip), which parses the NEO-6M and
 * forwards fixes to the hub as the FROZEN FH_TYPE_GPS_FIX (0x02) frame. This
 * removed the entire RMT/software-UART/NMEA/UBX transport that used to live here
 * (the old AB3 "inert RMT stub" is GONE, not patched) — a credible, reliable,
 * no-hack result: GPS parsing happens on the Pico where arduino-pico + TinyGPS++
 * already live, on its own pins with no contention, and reaches the hub over the
 * same proven link the Pico already uses.
 *
 * This module is now a PURE CONSUMER:
 *   - the aggregator decodes a 0x02 GPS_FIX frame and calls fh_time_on_gps_fix();
 *   - that caches the fix, projects time forward, and sets the RTC on first fix;
 *   - now_utc()/last_fix() serve the logger exactly as before.
 * No hardware is touched here anymore except the RTC (via the board layer).
 *
 * now_utc() resolution chain (unchanged contract):
 *   RTC-trusted  ->  GPS-fix time  ->  monotonic boot clock (flagged !from_rtc)
 */
#include "fh_hub_time.h"
#include "fh_hub_board.h"
#include "fh_frame.h"          /* fh_gps_fix_t (0x02 payload, frozen 16 B) */

#include <string.h>
#include <stdio.h>            /* snprintf */
#include <time.h>             /* time_t, gmtime_r */
#include <inttypes.h>
#include <stdatomic.h>        /* CD5: atomic_bool for the lock-free s_rtc_valid read */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "fh_time";

/* ---- shared state -------------------------------------------------------- */
static SemaphoreHandle_t s_mtx = NULL;
static fh_fix_t          s_fix;                 /* last-known fix (valid=false until lock) */
static uint64_t          s_fix_taken_us = 0;    /* esp_timer us when the fix was cached   */
/* CD5: s_rtc_valid is read on the hot path (now_utc, every logged record) with
 * NO mutex, and written from fh_time_on_gps_fix. Making it atomic_bool turns the
 * "a bool is atomic on this MCU" ASSUMPTION into a type-system GUARANTEE — the
 * concurrent lock-free access is now correct by construction on any target, with
 * zero lock on the hot path (adding a mutex there would risk a self-deadlock and
 * is the WRONG fix). Plain =/if still work; the atomicity is in the type. */
static atomic_bool       s_rtc_valid = false;   /* RTC holds trusted UTC */
static bool              s_rtc_set_from_gps = false;  /* only touched under s_mtx */

bool fh_time_start(fh_gps_rate_t want_rate)
{
    (void)want_rate;   /* GPS rate is now the Pico's concern (it owns the NEO-6M) */
    if (!s_mtx) { s_mtx = xSemaphoreCreateMutex(); }
    if (!s_mtx) { return false; }
    memset(&s_fix, 0, sizeof(s_fix));

    /* Seed RTC-valid from a battery-backed RTC that is already running (survives
     * cold swaps). A plausible post-2020 timestamp => trust it as the time base
     * until the first GPS fix arrives from the Pico. */
    uint32_t rtc_now = 0;
    if (fh_board_rtc_ok() && fh_board_rtc_get(&rtc_now) && rtc_now > 1600000000u) {
        s_rtc_valid = true;
    }
    ESP_LOGI(TAG, "time base ready (rtc_valid=%d; GPS arrives via Pico 0x02 frames)",
             (int)s_rtc_valid);
    return true;
}

/* Called by the aggregator when it decodes a FH_TYPE_GPS_FIX (0x02) frame off
 * the Pico link. Caches the fix, stamps the capture time, and sets the RTC from
 * the first good GPS UTC. Non-blocking (short mutex hold, no I/O except the
 * one-time RTC write on the first fix). */
void fh_time_on_gps_fix(const fh_gps_fix_t *fix)
{
    if (!fix) { return; }
    /* A 0x02 with epoch 0 AND no position is a "no fix yet" heartbeat from the
     * Pico — nothing to record. */
    if (fix->gps_time == 0 && fix->lat == 0 && fix->lon == 0) { return; }

    /* CD4: a fix can carry a valid TIME before POSITION has locked (and null-
     * island 0,0 is a real-but-usually-bogus position). So gate POSITION
     * validity on the frozen fix_qual field (>0 = a real positional fix), while
     * still letting a time-only fix advance the clock. The logger reports
     * lat/lon only when s_fix.valid, so a time-only fix won't emit a false 0,0. */
    bool pos_valid = (fix->fix_qual > 0);

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (pos_valid) {
        s_fix.valid  = true;
        s_fix.lat_e7 = fix->lat;
        s_fix.lon_e7 = fix->lon;
        s_fix.sats   = fix->sats;
    }
    /* time can update regardless of position lock, as long as it's plausible */
    if (fix->gps_time > 1600000000u) {
        s_fix.epoch_utc = fix->gps_time;
        s_fix_taken_us  = esp_timer_get_time();
    }
    bool set_rtc = (!s_rtc_set_from_gps && fix->gps_time > 1600000000u);
    uint32_t ep  = fix->gps_time;
    xSemaphoreGive(s_mtx);

    /* RTC write outside the lock (I2C I/O); first good time only. */
    if (set_rtc) {
        if (fh_board_rtc_set(ep)) {
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_rtc_valid = true;
            s_rtc_set_from_gps = true;
            xSemaphoreGive(s_mtx);
            ESP_LOGI(TAG, "RTC set from GPS: %" PRIu32, ep);
        }
    }
}

uint32_t fh_time_now_utc(bool *from_rtc)
{
    /* 1. RTC if trusted. NOTE (CD5): s_rtc_valid is an atomic_bool, read here
     * WITHOUT the mutex by design — the atomic type guarantees a clean load with
     * zero hot-path lock (now_utc runs on every logged record). Adding a mutex
     * here would risk a self-deadlock and is the wrong fix; the atomicity lives
     * in the type. */
    if (s_rtc_valid && fh_board_rtc_ok()) {
        uint32_t ep = 0;
        if (fh_board_rtc_get(&ep) && ep > 1600000000u) {
            if (from_rtc) { *from_rtc = true; }
            return ep;
        }
    }
    /* 2. GPS fix time projected forward by elapsed seconds. Gated on a cached
     * plausible EPOCH (not on positional validity — a time-only fix still gives
     * real UTC; CD4). */
    if (s_mtx) { xSemaphoreTake(s_mtx, portMAX_DELAY); }
    if (s_fix.epoch_utc > 1600000000u) {
        uint64_t elapsed = (esp_timer_get_time() - s_fix_taken_us) / 1000000ull;
        uint32_t ep = s_fix.epoch_utc + (uint32_t)elapsed;
        if (s_mtx) { xSemaphoreGive(s_mtx); }
        if (from_rtc) { *from_rtc = true; }   /* GPS is a real UTC source */
        return ep;
    }
    if (s_mtx) { xSemaphoreGive(s_mtx); }
    /* 3. monotonic boot clock (seconds since boot) — rows still sort */
    if (from_rtc) { *from_rtc = false; }
    return (uint32_t)(esp_timer_get_time() / 1000000ull);
}

fh_fix_t fh_time_last_fix(void)
{
    fh_fix_t copy;
    if (s_mtx) { xSemaphoreTake(s_mtx, portMAX_DELAY); }
    copy = s_fix;
    if (copy.valid) {
        copy.age_ms = (uint32_t)((esp_timer_get_time() - s_fix_taken_us) / 1000ull);
    }
    if (s_mtx) { xSemaphoreGive(s_mtx); }
    return copy;
}

bool fh_time_rtc_valid(void) { return s_rtc_valid; }

size_t fh_time_iso8601(uint32_t epoch, char *buf, size_t cap)
{
    if (!buf || cap < 21) { if (buf && cap) buf[0] = '\0'; return 0; }
    time_t t = (time_t)epoch;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    int n = snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return (n < 0) ? 0 : (size_t)n;
}

/* ===========================================================================
 * KNOWN RISKS (post GPS-on-Pico rearchitecture)
 *   [T1] GPS is now entirely on the Pico (FH-Advanced-Pico-rescue-gps.zip). The hub only
 *        consumes 0x02 GPS_FIX frames. If the Pico link is down, the hub falls
 *        back RTC -> boot-clock; capture is NEVER gated on a fix (unchanged).
 *   [T2] fh_time_on_gps_fix sets the RTC on the FIRST fix with a plausible epoch
 *        (>2020). If the Pico ever sends a bogus early epoch, the >1.6e9 guard
 *        rejects it. The RTC write is one-time (s_rtc_set_from_gps latch).
 *   [T3] snprintf/gmtime_r need <stdio.h>/<time.h>; both come via the standard
 *        headers pulled by esp_log/FreeRTOS, but add <stdio.h> + <time.h>
 *        explicitly on the first build to be safe. (time.h is used for time_t.)
 *   [T4] The 0x02 frame is the FROZEN fh_gps_fix_t (16 B): gps_time/lat/lon/
 *        sats/fix_qual/reserved. The Pico MUST fill it per that layout — the
 *        contract is pinned in FH-Advanced-Pico-rescue-gps.zip's GPS_PICO.md.
 *   [T5] (CD4 FIXED) POSITION validity (s_fix.valid) is gated on fix_qual>0, so
 *        a time-only early fix (or null-island 0,0) advances the clock but is
 *        NOT reported as a valid lat/lon. now_utc() path 2 projects time off a
 *        cached plausible epoch independent of positional lock.
 *   [T6] (CD5 FIXED in code) s_rtc_valid is an atomic_bool — its concurrent
 *        lock-free access (hot-path read in now_utc, write in on_gps_fix) is now
 *        GUARANTEED by the type system on any target, not assumed from the MCU.
 *        No mutex on the hot path (that would risk a self-deadlock). The fix is
 *        the atomic type, not a lock.
 * ======================================================================== */
