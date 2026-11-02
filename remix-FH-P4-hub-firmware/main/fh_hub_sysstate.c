/*
 * fh_hub_sysstate.c — builds the FH_OD_SYS_STATE_RESP payload (0x2E).
 *
 * UNVERIFIED FIRST DRAFT by the project rule (Kiro never compiles): expect a
 * compile-fix pass. Written against:
 *   - fh_ondemand.h   (fh_od_sys_state_resp_t, 74 B, seq-first rule)
 *   - fh_hub_logger.h (fh_log_stats)
 *   - fh_hub_req.h    (fh_req_stats)
 *   - fh_hub_config.h (fh_cfg_get, fh_cfg_status)
 *   - fh_hub_time.h   (fh_time_last_fix)
 *   - fh_hub_board.h  (fh_board_sd_mounted / fh_board_sd_space)
 *
 * WHY A HUB-ANSWERED VERB: SYS_STATE asks about the HUB ITSELF (SD, logger,
 * requester, config, uptime, fix cache) — data only the hub holds. The bridge
 * calls fh_sysstate_build() directly in its host-frame handler; no blade is
 * involved, so the one-at-a-time blade BUSY contract does NOT apply here.
 */
#include "fh_hub_sysstate.h"
#include "fh_hub_logger.h"
#include "fh_hub_req.h"
#include "fh_hub_config.h"
#include "fh_hub_time.h"
#include "fh_hub_bridge.h"
#include "fh_hub_board.h"
#include "fh_frame.h"
#include "fh_ondemand.h"
#include "esp_timer.h"
#include <string.h>       /* memset / memcpy */

void fh_sysstate_build(uint8_t seq, uint8_t *out, uint8_t *out_len)
{
    fh_od_sys_state_resp_t r;
    memset(&r, 0, sizeof(r));
    r.seq    = seq;
    r.status = FH_OD_OK;

    r.uptime_s    = (uint32_t)(esp_timer_get_time() / 1000000ull);
    r.interactive = fh_bridge_is_interactive() ? 1u : 0u;

    r.sd_mounted = fh_board_sd_mounted() ? 1u : 0u;
    uint32_t free_mb = 0xFFFFFFFFu, used_mb = 0xFFFFFFFFu;
    if (fh_board_sd_space(&free_mb, &used_mb)) {
        r.sd_free_mb = free_mb;
        r.sd_used_mb = used_mb;
    } else {
        r.sd_free_mb = 0xFFFFFFFFu;
        r.sd_used_mb = 0xFFFFFFFFu;
    }

    fh_log_stats_t lg = fh_log_stats();
    r.rows_staged     = lg.rows_staged;
    r.rows_committed  = (uint32_t)lg.bytes_committed;   /* 32-bit view (honest: wraps >4 GiB) */
    r.files_rolled    = lg.files_rolled;
    r.dropped_queue   = lg.dropped_queue;
    r.dropped_sd_down = lg.dropped_sd_down;
    r.res_written     = lg.res_written;

    fh_req_stats_t rq = fh_req_stats();
    r.jobs_done    = rq.jobs_done;
    r.job_timeouts = rq.timeouts;
    r.job_busy     = rq.busy_resps;

    const fh_hub_cfg_t *cfg = fh_cfg_get();
    fh_cfg_status_t      st = fh_cfg_status();
    r.gps_rate       = (uint8_t)cfg->gps_rate;
    r.brightness_pct = cfg->brightness_pct;
    r.ble_scan_mode  = (uint8_t)cfg->ble_scan_mode;
    r.relog_mode     = (uint8_t)cfg->relog_mode;
    r.relog_window_s = cfg->relog_window_s;
    r.band_mask      = (uint8_t)((cfg->band_24_en  ? 0x01u : 0u)
                               | (cfg->band_5_en   ? 0x02u : 0u)
                               | (cfg->band_ble_en ? 0x04u : 0u));
    r.dwell_ms       = cfg->dwell_ms;
    r.cfg_modified   = st.modified ? 1u : 0u;
    r.override_count = st.override_count;

    fh_fix_t fix = fh_time_last_fix();
    r.fix_valid = fix.valid ? 1u : 0u;
    r.lat_e7    = fix.lat_e7;
    r.lon_e7    = fix.lon_e7;
    r.sats      = fix.sats;   /* cached by fh_hub_time from each 0x02 GPS_FIX */

    memcpy(out, &r, sizeof(r));
    *out_len = (uint8_t)sizeof(r);
}

/* ============================================================================
 * KNOWN RISKS (UNVERIFIED — first build + bench)
 *  R1  RESOLVED (multi-check 2026-11-01): sats WAS hardcoded 0 on the false
 *      belief that fh_fix_t had no satellite count. It does (fh_hub_time.h),
 *      and fh_hub_time caches it from every 0x02 GPS_FIX; SYS_STATE now reports
 *      fix.sats. (The head + Medusa display this field.)
 *  R2  fh_log_stats() and fh_req_stats() are called from the bridge task while
 *      the logger/requester tasks increment those counters — reads of uint32_t
 *      counters are atomic-enough on Xtensa for a REPORT (worst case a value
 *      is one increment stale). No lock added by design.
 *  R3  statvfs on the mounted FATFS VFS: confirm f_bsize semantics on IDF 6.1
 *      (fh_hub_board.c carries the same note).
 * ========================================================================== */
