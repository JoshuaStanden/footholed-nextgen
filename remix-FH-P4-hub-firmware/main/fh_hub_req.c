/*
 * fh_hub_req.c — the hub's ON-DEMAND REQUESTER impl. See fh_hub_req.h.
 *
 * Written 2026-10-31 (Joshua concern #1). UNVERIFIED FIRST DRAFT by the
 * project rule (Kiro never compiles): expect a compile-fix pass like the rest
 * of the hub. Design verified against:
 *   - fh_ondemand.h  (seq-first payload rule, one-at-a-time BUSY rule)
 *   - fh_link.c      (two-phase blade_xfer: phase 1 delivers, phase 2 reads
 *                     whatever the blade staged between the transactions)
 *   - remix-fh-pico-cc1101.ino (the blade stages its reply for the NEXT
 *                     transaction and keeps re-presenting it; a REQ re-send
 *                     would RESTART a completed job — hence idle polls only)
 */
#include "fh_hub_req.h"
#include "fh_hub_link.h"
#include "fh_hub_bridge.h"     /* fh_bridge_emit: synthetic BUSY/timeout RESP */
#include "fh_hub_logger.h"     /* fh_log_submit_result                        */
#include "fh_hub_time.h"
#include "fh_hub_pins.h"
#include "fh_frame.h"
#include "fh_ondemand.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "fh_req";

/* ---- tunables ------------------------------------------------------------ */
#define FH_REQ_POLL_MS        250u   /* how often an in-flight blade is polled  */
#define FH_REQ_MARGIN_MS     2000u   /* added on top of the job's own duration  */
#define FH_REQ_STREAM_MS     3500u   /* SNIFF dump stream: up to 11 chunks x    */
                                     /* 250 ms poll cadence (~2.75 s) AFTER the */
                                     /* dwell ends; the margin alone under-     */
                                     /* budgets it and would false-TIMEOUT.     */
#define FH_REQ_DEADLINE_CAP 45000u   /* hard cap (blade clamps dwell to 30 s)   */
#define FH_REQ_DEADLINE_MIN  3000u   /* floor (PING-class instant jobs)         */
#define FH_REQ_QUEUE_DEPTH     8u    /* pending submits (host fires slowly)     */
#define FH_REQ_TASK_STACK   4096u
#define FH_REQ_TASK_PRIO       8u    /* above the GPS poll (7): a fired job     */
                                    /* must not wait behind a GPS poll         */

/* ---- flight table --------------------------------------------------------- */
typedef struct {
    bool     active;
    uint8_t  slot;
    uint8_t  req_type;        /* FH_OD_*_REQ                                       */
    uint8_t  resp_type;       /* FH_OD_*_RESP (req + 1, frozen band)               */
    uint8_t  seq;             /* echoed by every RESP                              */
    uint64_t deadline_us;     /* esp_timer at which we declare a timeout           */
    uint32_t dwell_hint_ms;   /* the job's own duration (for the result summary)   */
    bool     saw_busy;        /* the blade answered BUSY at least once             */
} fh_flight_t;

static fh_flight_t s_flight[FH_BLADE_COUNT];
static volatile bool s_slot_busy[FH_BLADE_COUNT];  /* advisory fast reject      */
static QueueHandle_t s_q = NULL;
static TaskHandle_t  s_task = NULL;
static uint32_t s_jobs_done = 0;
static uint32_t s_timeouts = 0;
static uint32_t s_busy_resps = 0;

/* A queued submit: the frame bytes + the deadline the bridge asked for. */
typedef struct {
    uint8_t  slot;
    uint32_t deadline_ms;      /* 0 = derive from the REQ payload               */
    uint8_t  frame[FH_MAX_FRAME];
    uint16_t frame_len;
} fh_req_job_t;

/* ---- per-type deadline derivation (parse the frozen REQ payload) ---------- */
static uint32_t job_duration_ms(uint8_t req_type, const uint8_t *pl, uint8_t len)
{
    /* All REQ payloads start with seq; the fields that carry the job's own
     * duration follow per the frozen structs in fh_ondemand.h. Little-endian. */
    switch (req_type) {
    case FH_OD_CC1101_REQ:        /* seq, action, freq_khz u32, dwell_ms u16   */
        if (len >= 8) {
            if (pl[1] == FH_OD_CC1101_ACT_REPLAY) {
                /* A TX job ignores dwell_ms: ~3 repeats of a ≤32-bit OOK code
                 * (proto pulse ≤ ~6.5 ms per bit worst case) — well under 2 s.
                 * Give it the floor deadline; the REQ margin still applies. */
                return FH_REQ_DEADLINE_MIN;
            }
            return (uint32_t)pl[6] | ((uint32_t)pl[7] << 8);
        }
        break;
    case FH_OD_NRF24_REQ:         /* seq, mode, chan_lo, chan_hi, dwell u16    */
        if (len >= 6) {
            uint32_t dwell = (uint32_t)pl[4] | ((uint32_t)pl[5] << 8);
            if (pl[1] == FH_OD_NRF24_MODE_SNIFF) {
                /* SNIFF is capture-to-dump on ONE channel: duration = dwell,
                 * PLUS the chunk stream that follows (up to 64x40 B = 2560 B
                 * = 11 chunks, one per 250 ms poll = ~2.75 s). Without this
                 * the margin alone under-budgets a full capture and the hub
                 * would false-TIMEOUT mid-stream. */
                return dwell + FH_REQ_STREAM_MS;
            }
            /* Span = |hi - lo| + 1, mirroring the blade, which SWAPS a reversed
             * range (nrf24 main.c SCAN). The old `(uint32_t)(pl[3]-pl[2]) + 1`
             * went negative for hi<lo; the cast made it ~4e9 and chans*dwell
             * wrapped mod 2^32 — possibly to a TOO-SHORT deadline (false
             * TIMEOUT on a valid job). Multi-check 2026-11-01 fix. */
            uint32_t chans = (uint32_t)((pl[3] >= pl[2]) ? (pl[3] - pl[2])
                                                         : (pl[2] - pl[3])) + 1u;
            return chans * dwell;          /* max 126 x 65535 — no overflow  */
        }
        break;
    case FH_OD_NFC_REQ:           /* seq, timeout_ms u16 (3 B total)           */
        if (len >= 3) return (uint32_t)pl[1] | ((uint32_t)pl[2] << 8);
        break;
    case FH_OD_LORA_TX_REQ:       /* seq, flags, len, data[] — a TX is short   */
        return 1500u;
    case FH_OD_LORA_LISTEN_REQ:   /* seq, enable, window_ms u16                */
        if (len >= 4) {
            uint32_t w = (uint32_t)pl[2] | ((uint32_t)pl[3] << 8);
            return w ? w : 1000u;
        }
        break;
    default:
        /* EXTERN band (Tier 3, 0x30..0x3E even = REQ): the frozen generic
         * struct is seq, verb_id, p1 u32, p2 u32, p3 u32 — p3 (bytes 10..13,
         * LE) is the duration_ms convention for timed ops (0 = instant).
         * The hub can't know an author-defined verb's real duration, so the
         * convention IS the contract: the external author sets p3. Clamped
         * to the same 30 s ceiling the blades use. */
        if (req_type >= FH_OD_EXT_CC_JAM_REQ && req_type <= 0x3F
            && (req_type & 1u) == 0 && len >= 14) {
            uint32_t d = (uint32_t)pl[10] | ((uint32_t)pl[11] << 8)
                       | ((uint32_t)pl[12] << 16) | ((uint32_t)pl[13] << 24);
            if (d == 0u) { return FH_REQ_DEADLINE_MIN; }   /* instant */
            if (d > 30000u) { d = 30000u; }
            return d;
        }
        break;
    }
    return 0u;
}

uint8_t fh_req_resp_type_for(uint8_t req_type)
{
    /* Frozen band: RESP = REQ + 1 (verified against fh_ondemand.h — do not
     * renumber the protocol; this mapping only reads it). SYS_STATE (0x2D) is
     * NOT a blade REQ — the hub answers it itself, so it returns 0xFF here.
     * EXTERN band (Tier 3): same rule — RESP = REQ + 1, REQs are EVEN
     * (0x30..0x3E). FH_OD_DUMP_IND (0x2F) is unsolicited, never a REQ. */
    if (req_type >= FH_OD_EXT_CC_JAM_REQ && req_type <= 0x3F) {
        if ((req_type & 1u) == 0) { return (uint8_t)(req_type + 1); }  /* even = REQ  */
        return 0xFF;                                                   /* odd = RESP  */
    }
    if (req_type < FH_OD_PING_REQ || req_type > FH_OD_LORA_LISTEN_REQ) return 0xFF;
    if (req_type == FH_OD_LORA_RX_IND) return 0xFF;   /* 0x2A is unsolicited    */
    return (uint8_t)(req_type + 1);
}

fh_req_stats_t fh_req_stats(void)
{
    fh_req_stats_t s;
    s.jobs_done  = s_jobs_done;
    s.timeouts   = s_timeouts;
    s.busy_resps = s_busy_resps;
    return s;
}

bool fh_req_slot_busy(uint8_t slot)
{
    if (slot >= FH_BLADE_COUNT) return true;
    return s_slot_busy[slot];
}

bool fh_req_submit(uint8_t slot, const uint8_t *frame, size_t n,
                   uint32_t deadline_ms)
{
    if (!s_q || !s_task) return false;
    if (slot >= FH_BLADE_COUNT) return false;
    if (!frame || n == 0 || n > FH_MAX_FRAME) return false;
    if (fh_req_slot_busy(slot)) return false;      /* advisory; task re-checks */
    fh_req_job_t j;
    memset(&j, 0, sizeof(j));
    j.slot = slot;
    j.deadline_ms = deadline_ms;
    memcpy(j.frame, frame, n);
    j.frame_len = (uint16_t)n;
    return xQueueSend(s_q, &j, 0) == pdTRUE;
}

/* ---- completion: write the result file ------------------------------------ */
static void flight_summary(char *out, size_t cap, const fh_flight_t *f,
                           const uint8_t *pl, uint8_t len, bool timed_out)
{
    /* One CSV-safe human line describing the result. Kept SHORT and honest. */
    if (timed_out) {
        snprintf(out, cap, "status=TIMEOUT after poll window (no RESP, seq=%u)", f->seq);
        return;
    }
    uint8_t status = (len >= 2) ? pl[1] : 0xFF;
    switch (f->req_type) {
    case FH_OD_CC1101_REQ: {          /* RESP: seq, status, rssi i8, len u8, data[] */
        if (len >= 4 && status == FH_OD_OK)
            snprintf(out, cap, "status=OK rssi=%d captured=%u B dwell=%" PRIu32 " ms",
                     (int)(int8_t)pl[2], pl[3], f->dwell_hint_ms);
        else
            snprintf(out, cap, "status=0x%02X", status);
        break;
    }
    case FH_OD_NRF24_REQ: {           /* RESP: seq, status, active_chan u8, rssi i8 */
        if (len >= 4 && status == FH_OD_OK)
            snprintf(out, cap, "status=OK active_chan=%u rssi=%d",
                     pl[2], (int)(int8_t)pl[3]);
        else
            snprintf(out, cap, "status=0x%02X", status);
        break;
    }
    case FH_OD_NFC_REQ:               /* RESP: seq, status, uid_len, uid[], tag_type */
    case FH_OD_LORA_TX_REQ:           /* RESP: seq, status, toa_ms u16               */
    case FH_OD_LORA_LISTEN_REQ:
    default:
        snprintf(out, cap, "status=0x%02X len=%u", status, len);
        break;
    }
}

static void finish_flight(fh_flight_t *f, const uint8_t *pl, uint8_t len,
                          bool timed_out)
{
    char summary[96];
    flight_summary(summary, sizeof(summary), f, pl, len, timed_out);

    fh_log_result_t r;
    memset(&r, 0, sizeof(r));
    r.radio_slot   = f->slot;
    r.req_type     = f->req_type;
    r.seq          = f->seq;
    r.uptime_s     = (uint32_t)(esp_timer_get_time() / 1000000ull);
    r.timed_out    = timed_out;
    r.saw_busy     = f->saw_busy;
    if (!timed_out && len > 0) {
        size_t cp = (len < sizeof(r.resp_payload)) ? len : sizeof(r.resp_payload);
        memcpy(r.resp_payload, pl, cp);
        r.resp_len = (uint8_t)cp;
    }
    snprintf(r.summary, sizeof(r.summary), "%s", summary);
    fh_log_submit_result(&r);

    if (timed_out) { s_timeouts++; }
    else           { s_jobs_done++; }
    ESP_LOGI(TAG, "job done slot=%u req=0x%02X seq=%u %s",
             f->slot, f->req_type, f->seq, summary);

    f->active = false;
    s_slot_busy[f->slot] = false;
}

/* ---- the requester task ---------------------------------------------------- */
static void requester_task(void *arg)
{
    (void)arg;
    fh_req_job_t j;
    for (;;) {
        /* Wait for a submit OR the poll tick (250 ms), whichever first. */
        if (xQueueReceive(s_q, &j, pdMS_TO_TICKS(FH_REQ_POLL_MS)) == pdTRUE) {
            /* A submit arrived. Authoritative busy check (the bridge's check
             * was advisory — two hosts could race the same slot). */
            if (fh_req_slot_busy(j.slot)) {
                /* Internal race (two submits to one slot): drop with a log.
                 * The bridge synthesizes a BUSY RESP on its own reject, so a
                 * host only reaches this path if it raced itself. */
                ESP_LOGW(TAG, "slot %u busy on dequeue — submit dropped", j.slot);
                continue;
            }
            fh_flight_t *f = &s_flight[j.slot];
            memset(f, 0, sizeof(*f));
            /* frame layout: SYNC0(0) SYNC1(1) TYPE(2) LEN(3) payload(4..) CRC.
             * payload[0] is the seq (frozen fh_ondemand.h rule). */
            f->active    = true;
            f->slot      = j.slot;
            f->req_type  = j.frame[2];
            f->resp_type = fh_req_resp_type_for(f->req_type);
            f->seq       = j.frame[4];
            uint32_t dur = j.deadline_ms ? j.deadline_ms
                                         : job_duration_ms(f->req_type,
                                                           j.frame + 4,
                                                           (uint8_t)(j.frame_len - 4));
            if (dur == 0) { dur = FH_REQ_DEADLINE_MIN; }
            f->dwell_hint_ms = dur;                 /* the job's OWN duration  */
            dur += FH_REQ_MARGIN_MS;
            if (dur > FH_REQ_DEADLINE_CAP) { dur = FH_REQ_DEADLINE_CAP; }
            f->deadline_us = (uint64_t)esp_timer_get_time()
                           + (uint64_t)dur * 1000ull;
            s_slot_busy[j.slot] = true;

            ESP_LOGI(TAG, "job fired slot=%u req=0x%02X seq=%u deadline=%" PRIu32 " ms",
                     j.slot, f->req_type, f->seq, dur);
            /* FIRE the REQ (one exchange; the RESP for an instant job comes
             * back in phase 2 of this very xfer and completes the flight
             * inside fh_req_on_frame — before the line above may have been
             * printed by a slow serial, so the log is truthful by ordering). */
            fh_link_blade_xfer(j.slot, j.frame, j.frame_len);
        }

        /* Poll every in-flight slot (idle frame: clock in whatever is staged),
         * and sweep deadlines. The xfer routes the RESP through this task's
         * fh_req_on_frame (see below) which completes the flight. */
        uint64_t now = (uint64_t)esp_timer_get_time();
        for (int slot = 0; slot < FH_BLADE_COUNT; ++slot) {
            fh_flight_t *f = &s_flight[slot];
            if (!f->active) { continue; }
            if (now >= f->deadline_us) {
                /* Deadline passed: synthetic RESP to the host + timeout file. */
                uint8_t t_pl[2] = { f->seq, FH_OD_TIMEOUT };
                fh_bridge_emit(f->resp_type, t_pl, 2);
                finish_flight(f, NULL, 0, true);
                continue;
            }
            /* Idle poll: all-zero MOSI; the blade ignores it and presents
             * whatever it has staged. Its phase-2 bytes are decoded and
             * pushed through blade_cb -> route_frame -> fh_req_on_frame. */
            fh_link_blade_xfer((uint8_t)slot, NULL, 0);
        }
    }
}

void fh_req_on_frame(uint8_t type, const uint8_t *payload, uint8_t len)
{
    /* Called from route_frame for every decoded frame. Cheap exit for the
     * 99% case (a capture/gps/heartbeat frame, not an on-demand RESP).
     * EXTERN band RESPs (0x31..0x3F odd) match the same flight table. */
    bool is_od_resp = (type >= FH_OD_PING_RESP && type <= FH_OD_LORA_LISTEN_RESP);
    bool is_ext_resp = (type >= FH_OD_EXT_CC_JAM_RESP && type <= 0x3F)
                    && ((type & 1u) == 1);              /* RESPs are ODD */
    if (!(is_od_resp || is_ext_resp) || len < 2) { return; }

    uint8_t seq = payload[0];
    for (int slot = 0; slot < FH_BLADE_COUNT; ++slot) {
        fh_flight_t *f = &s_flight[slot];
        if (!f->active) { continue; }
        if ((uint8_t)type != f->resp_type || seq != f->seq) { continue; }
        uint8_t status = payload[1];
        if (status == FH_OD_BUSY) {
            /* The blade is mid-op: it staged BUSY for our earlier REQ. Keep
             * polling — do NOT re-send the REQ (that would re-run a completed
             * job on the blade). The deadline still applies. */
            f->saw_busy = true;
            s_busy_resps++;
            ESP_LOGD(TAG, "slot %u BUSY while polling (seq %u)", slot, seq);
            return;
        }
        finish_flight(f, payload, len, false);
        return;
    }
    /* No flight matches: a stray/duplicate RESP (e.g. the reply staged on the
     * blade AFTER a timeout was declared, clocked in by a later job's first
     * poll). It is already mirrored to Medusa by route_frame; nothing to do. */
}

bool fh_req_start(void)
{
    if (s_task) { return true; }
    memset(s_flight, 0, sizeof(s_flight));
    memset((void *)s_slot_busy, 0, sizeof(s_slot_busy));
    s_q = xQueueCreate(FH_REQ_QUEUE_DEPTH, sizeof(fh_req_job_t));
    if (!s_q) { return false; }
    if (xTaskCreate(requester_task, "fh_req", FH_REQ_TASK_STACK, NULL,
                    FH_REQ_TASK_PRIO, &s_task) != pdPASS) {
        vQueueDelete(s_q);
        s_q = NULL;
        return false;
    }
    ESP_LOGI(TAG, "on-demand requester up (poll %u ms, margin %u ms)",
             FH_REQ_POLL_MS, FH_REQ_MARGIN_MS);
    return true;
}

/* ============================================================================
 * KNOWN RISKS / NOTES (pass 1, write-by-reading — NOT bench-proven)
 *  R1  SINGLE-DRIVER INVARIANT: radio-blade RESPs reach fh_req_on_frame only
 *      via the requester's own fh_link_blade_xfer polls (each blade slot has
 *      its own fh_decoder_t). The bridge no longer drives radio blades
 *      directly for on-demand REQs — every REQ goes through fh_req_submit, so
 *      the invariant holds by construction. If a future module drives a blade
 *      directly it MUST go through this requester, or the flight table will
 *      silently miss RESPs.
 *  R2  GPS SLOT: FH_BLADE_SLOT_GPS is polled by gps_poll_task (link.c), NOT
 *      tracked here. GPS fixes are unsolicited 0x02 frames; DECISION CLOSED
 *      (2026-10-31): no on-demand GPS REQ frame — the periodic poll already
 *      requests on demand by the hub (cadence from cfg.gps_rate) and a new
 *      frozen frame would return the same fix through a slower path. If a
 *      bench scenario ever needs sub-cadence latency, fire the existing GPS
 *      poll frame through fh_req_submit() instead (no header change).
 *  R3  STALE STAGED REPLY: if a job times out, the blade may stage the RESP
 *      afterwards; the next job's FIRST poll clocks it in. fh_req_on_frame
 *      drops it (no flight matches that seq) but route_frame still mirrors it
 *      to Medusa, where Medusa's own seq matching ignores it. Harmless.
 *  R4  BUSY REJECT PATH: the task-side double-submit branch (internal race)
 *      currently logs and drops. The bridge synthesizes a BUSY RESP on its own
 *      advisory reject, so a host never hangs in this path — but if the race
 *      ever fires, the host's REQ dies silently. Verify on the bench that the
 *      bridge reject is the only reachable path.
 *  R5  NRF24 span: RESOLVED (multi-check 2026-11-01). chan_hi < chan_lo used to
 *      underflow, and chans*dwell could wrap mod 2^32 to a TOO-SHORT deadline
 *      (a false timeout, not just log noise). The span is now |hi-lo|+1,
 *      matching the blade's swap. Note the blade also clamps SCAN dwell to
 *      10 ms, so the hub's chans*dwell is a deliberate over-estimate (capped 45 s).
 *  R6  HEALTH PING (0x7D) CANNOT COLLIDE: the health monitor's PING/PONG live
 *      in the 0x7C..0x7D band, and the on-demand RESP window is 0x21..0x2C, so
 *      a peer's PONG never enters fh_req_on_frame's matching. Verified against
 *      fh_frame.h (do not renumber either band). The only shared type-family
 *      is 0x21 (FH_OD_PING_RESP): a peer that emits 0x21 with a colliding seq
 *      would complete a flight — no such peer exists in this codebase; noted
 *      so a future Bus-A peer does not grow one.
 *  R7  RESULTS CSV + PER-JOB FILES: results.csv rows never enter the main
 *      stage buffer (the main CSV schema is FROZEN). The CC1101 per-job file
 *      (the user requirement: a CC1101 read creates its OWN file named by a
 *      RELATIVE timestamp, uptime seconds) is written directly by the logger
 *      task; SD-down results are counted in res_sd_down, not staged.
 * ========================================================================== */
