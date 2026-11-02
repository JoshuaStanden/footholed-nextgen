/*
 * fh_hub_time.h — FHAdvanced hub time + position base (SUBSYSTEM 2)
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule).
 *
 * Load-bearing but SMALL: the logger hits now_utc() and last_known_fix() on
 * EVERY record. That is the entire public surface — two reads.
 *
 * ARCHITECTURE (2026-10-22): GPS is NOT wired to the hub. The Pico node
 * (FH-Advanced-Pico-rescue-gps.zip) parses the NEO-6M and sends the hub FROZEN
 * FH_TYPE_GPS_FIX (0x02) frames; the aggregator decodes them and calls
 * fh_time_on_gps_fix(). This module therefore owns only:
 *   - the PCF85063 RTC (battery-backed UTC; survives cold swaps), via the board;
 *   - caching the last GPS fix the Pico reports + setting the RTC on first fix;
 *   - the now_utc() resolution chain for the logger.
 * No GPS UART/RMT/NMEA lives here — that moved to the Pico (where arduino-pico
 * + TinyGPS++ already run), which is the credible, reliable, no-hack result.
 *
 * This header is framework-agnostic in signature; the .c is ESP-IDF.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "fh_hub_config.h"   /* fh_gps_rate_t (kept in the signature for compat) */
#include "fh_frame.h"        /* fh_gps_fix_t — the 0x02 payload the Pico sends */

#ifdef __cplusplus
extern "C" {
#endif

/* A cached position fix. All-zero (valid=false) until the first GPS lock. */
typedef struct {
    bool     valid;       /* true once at least one real fix was seen         */
    uint32_t epoch_utc;   /* GPS UTC, epoch seconds (0 if never)              */
    int32_t  lat_e7;      /* degrees x 1e7 (frozen CAPTURE schema unit)       */
    int32_t  lon_e7;      /* degrees x 1e7                                    */
    uint8_t  sats;        /* satellites in the last fix                       */
    uint32_t age_ms;      /* ms since this fix was taken (esp_timer based)    */
} fh_fix_t;

/* Start the time base. Seeds trust from a battery-backed RTC (if already
 * running) and prepares the fix cache. `want_rate` is retained for signature
 * compatibility but is now the Pico's concern (it owns the NEO-6M), so it is
 * ignored here. The board layer must have brought I2C up first (for the RTC).
 * Returns false only on a resource failure. A capture is NEVER gated on a fix. */
bool fh_time_start(fh_gps_rate_t want_rate);

/* Consume a GPS fix the Pico reported (FROZEN FH_TYPE_GPS_FIX 0x02 payload).
 * The aggregator calls this when it decodes a 0x02 frame off the Pico link:
 * it caches the fix, stamps the capture time, and sets the RTC on the first
 * good fix. Non-blocking (short mutex; one-time RTC write on first fix). */
void fh_time_on_gps_fix(const fh_gps_fix_t *fix);

/* Current UTC epoch seconds. Resolution order:
 *   1. RTC if it has been set (from GPS UTC on first lock, or battery-backed);
 *   2. else GPS time if a fix exists but RTC write failed;
 *   3. else a monotonic boot clock (seconds since boot) so rows still sort —
 *      flagged by *from_rtc=false so the logger/UI can show "no time source".
 * Never blocks; safe from any task. If `from_rtc` is non-NULL it is set true
 * only when the value came from a real UTC source (RTC or GPS), false when it
 * is the boot fallback. */
uint32_t fh_time_now_utc(bool *from_rtc);

/* Snapshot of the last-known GPS fix (copy; caller owns it). valid=false and
 * lat/lon=0 until the first lock — matching the frozen CAPTURE "0 if no fix". */
fh_fix_t fh_time_last_fix(void);

/* True once the RTC holds a trusted UTC (set from GPS, or already running from
 * the battery). The UI shows a clock-status chip off this. */
bool fh_time_rtc_valid(void);

/* Format an ISO-8601 UTC string "YYYY-MM-DDTHH:MM:SSZ" for `epoch` into buf
 * (needs >= 21 bytes). Used for the SD header `start=` field and row `time`
 * column. Returns bytes written (excluding NUL), 0 on error. */
size_t fh_time_iso8601(uint32_t epoch, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
