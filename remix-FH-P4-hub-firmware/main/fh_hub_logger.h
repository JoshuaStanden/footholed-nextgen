/*
 * fh_hub_logger.h — FHAdvanced hub logger / SD writer (SUBSYSTEM 5)
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule).
 *
 * THE SINGLE SOURCE OF TRUTH / THE ONLY SD WRITER. Highest LOGIC risk in the
 * hub (most state: open file, rotation, staging buffer, "can't drop the ball").
 * Built FIRST (after board/time) against a FAKE record feed so its state
 * machine gets the most bake time before real sources exist (HUB_PLAN order).
 *
 * THE SEAM (HUB_PLAN): subsystems 2,3,4 never talk to each other — they all
 * produce records and hand them to THIS logger via fh_log_submit(). The logger
 * stamps time+fix (subsystem 2), expands binary -> CSV, batches writes with the
 * reboot-logging RTC-staging crash-survival pattern, rolls at a FIXED 128 MB,
 * and writes the provenance header per file.
 *
 * CSV schema (FROZEN — CONFIG_MENU + ARCHITECTURE, do not drift):
 *   source,band,channel,type,mac,rssi,lat,lon,time,extra
 * UTC only. Binary on the wire; the hub expands to CSV here.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "fh_frame.h"   /* fh_capture_t, fh_src_id_t, FH_SRCSTR_* */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the unified log record ----------------------------------------------
 * Every source (UART sniffers, hub BLE, Pico on-demand results) fills ONE of
 * these and submits it. The logger owns the mapping to CSV columns. A source
 * sets `source_str` from the FROZEN FH_SRCSTR_* vocabulary — the single place
 * those spellings may come from. Fields a source cannot fill are left 0/"" and
 * the logger stamps time+fix itself (sources send lat/lon/time = 0, per the
 * CAPTURE contract — the hub geotags). */
typedef struct {
    const char *source_str;   /* FH_SRCSTR_* (points to a static literal)     */
    uint8_t     band;         /* 0=n/a, 2=2.4GHz, 5=5GHz, 1=BLE (display only)*/
    uint16_t    channel;      /* radio channel / BLE adv channel (0 = n/a)    */
    uint8_t     frame_type;   /* protocol-specific subtype (mgmt/data/etc.)   */
    uint8_t     mac[6];       /* device identity (0s if not applicable)       */
    bool        has_mac;      /* false => mac column blank (e.g. NFC UID row) */
    int8_t      rssi;         /* dBm                                          */
    bool        has_rssi;     /* false => rssi column blank                   */
    uint16_t    count;        /* dedup occurrences carried in `extra`/summary */
    char        extra[48];    /* already-rendered, CSV-safe extra text        */
} fh_log_record_t;

/* Start the logger task + queue. Opens the first log file on SD (if mounted);
 * if SD is down it buffers into the RTC-staging RAM region and opens the file
 * when SD returns. Returns false only on a resource (queue/task) failure.
 * `fake_feed` true spawns a synthetic record generator (HUB_PLAN: exercise the
 * state machine — CSV, staging, rotation, header — on fabricated records before
 * any real source exists). Leave false in the integrated build. */
bool fh_log_start(bool fake_feed);

/* Submit a finished record. NON-BLOCKING (zero-tick enqueue; drop-newest +
 * counter on a full queue, same posture as the sniffers' TX path). Safe from
 * any task, including a BLE/Wi-Fi callback. Returns true if queued. The logger
 * task stamps time/fix and writes CSV; the caller must NOT touch SD. */
bool fh_log_submit(const fh_log_record_t *rec);

/* Convenience: turn a FROZEN fh_capture_t (as decoded off a UART) into a
 * log record and submit it. Maps src_id -> source_str, band_chan -> band/
 * channel, copies mac/rssi/count, renders `extra` as hex. One place that
 * knows the CAPTURE->CSV mapping so the aggregator and BLE paths agree. */
bool fh_log_submit_capture(const fh_capture_t *cap);

/* ---- on-demand results (concern #1, 2026-10-31) ---------------------------
 * A completed on-demand job lands in TWO places:
 *   1. a clean row in /sdcard/logs/results.csv (radio, seq, status text —
 *      NO binary blobs, NO weird fields),
 *   2. for CC1101 jobs ONLY: its own file under /sdcard/ondemand/ named
 *      cc1101_<uptime_s>u_seq<N>.log — the RELATIVE timestamp (uptime
 *      seconds; the hub has no wall clock until the GPS blade is wired) is
 *      the filename, so a capture is its own file and never drowns a shared
 *      CSV in a hex payload.
 * Non-blocking, same drop-newest posture as fh_log_submit(). */
typedef struct {
    uint8_t  radio_slot;       /* blade slot 0..5 (FH_BLADE_SLOT_*)          */
    uint8_t  req_type;         /* FH_OD_*_REQ                                */
    uint8_t  seq;              /* echoed seq                                 */
    uint32_t uptime_s;         /* relative timestamp (seconds since boot)    */
    bool     timed_out;
    bool     saw_busy;         /* the blade answered BUSY during polling     */
    uint8_t  resp_len;         /* bytes in resp_payload (0 when timed out)   */
    uint8_t  resp_payload[64]; /* raw RESP payload (largest RESP is 36 B)    */
    char     summary[96];      /* short CSV-safe status text                 */
} fh_log_result_t;
bool fh_log_submit_result(const fh_log_result_t *r);

/* Diagnostics for the health UI. */
typedef struct {
    uint32_t rows_staged;      /* rows accepted into the stage (NOT yet proven  */
                               /* on SD — some may be dropped if SD stays down) */
    uint64_t bytes_committed;  /* bytes successfully fwritten+flushed to SD     */
                               /* (CD3: the honest "data is on the card" measure)*/
    uint32_t dropped_queue;    /* submit() drops on a full queue                */
    uint32_t dropped_sd_down;  /* rows lost because SD was down + staging full  */
    uint32_t files_rolled;     /* 128 MB rotations                              */
    uint64_t cur_file_bytes;   /* bytes in the currently-open file              */
    bool     sd_writing;       /* true when actively writing to SD              */
    /* on-demand results (concern #1) */
    uint32_t res_written;      /* result rows/files written to SD               */
    uint32_t res_dropped;      /* submit_result() drops on a full queue         */
    uint32_t res_sd_down;      /* results skipped because the card was down     */
} fh_log_stats_t;
fh_log_stats_t fh_log_stats(void);

/* Force a flush of the staging buffer to SD (maintenance #3 "SD-only format"
 * and "safe battery swap" call this before parking). Returns true if the
 * buffer is empty/flushed. */
bool fh_log_flush(void);

#ifdef __cplusplus
}
#endif
