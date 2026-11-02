/*
 * fh_hub_req.h — the hub's ON-DEMAND REQUESTER (the blade-answer tracker).
 *
 * THE GAP THIS CLOSES (Joshua concern #1, 2026-10-31): before this module the
 * hub fired a blade REQ and read ONE reply — then forgot the blade. A long job
 * (15 s CC1101 capture) staged its answer long after the hub stopped listening,
 * so Medusa timed out and NOTHING reached the SD log. This module owns every
 * on-demand job from fire to finish:
 *
 *   submit  -> the requester sends the REQ (one exchange via fh_link_blade_xfer)
 *   track   -> a flight record {slot, req type, resp type, seq, deadline}
 *   poll    -> the task polls the blade slot with IDLE clocked frames every
 *              FH_REQ_POLL_MS until a RESP with the matching seq arrives
 *   finish  -> RESP matched  : result file written to SD (fh_log_submit_result)
 *                              + flight cleared (+ Medusa sees the RESP via the
 *                                route_frame mirror, unchanged)
 *              BUSY RESP     : keep polling (the blade is mid-op; it will stage
 *                              the real RESP when it finishes)
 *              deadline pass : timeout result file + a synthetic RESP with
 *                              status FH_OD_TIMEOUT sent to Medusa so the host
 *                              gets an explicit answer instead of hanging
 *
 * WHY IDLE POLLS, NOT RE-FIRING THE REQ: the blade firmware stages its reply
 * for the NEXT transaction and keeps re-presenting it (remix-fh-pico-cc1101
 * hub_on_sent re-presents the staged frame). Re-sending the REQ would restart
 * a completed job (the blade would run the capture a SECOND time). An idle
 * all-zero frame decodes to nothing on the blade and just clocks in whatever
 * is staged. Verified against the blade source, not assumed.
 *
 * CONCURRENCY: one requester task owns ALL Bus-B on-demand traffic through
 * fh_link_blade_xfer (which takes s_busB_mtx), so the GPS poll and requester
 * polls serialize on the same mutex — neither starves the other. RESPs are
 * matched inside the requester task: a radio-blade RESP can only be clocked
 * in by the requester's own xfers (each blade slot has its own decoder), so
 * the match is single-threaded by construction. See KNOWN RISKS at EOF of the
 * .c for the one exception.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Start the requester task. Call once from app_main AFTER fh_link_start()
 * (it needs the SPI buses) and BEFORE fh_bridge_start() (the bridge submits
 * jobs). Returns false only if the task/queue could not be created. */
bool fh_req_start(void);

/* Submit one on-demand job. `frame` is the full pre-encoded REQ frame
 * (fh_frame_encode'd, as fh_hub_bridge.c builds it from the host).
 * Returns false if:
 *   - the requester is not started, or
 *   - `slot` already has a job in flight (the frozen one-at-a-time BUSY
 *     contract, per blade). The caller (bridge) then answers the host with a
 *     synthetic FH_OD_BUSY RESP — the host never waits on a dead REQ.
 * `deadline_ms` = 0 means "derive from the REQ payload" (dwell_ms etc.). */
bool fh_req_submit(uint8_t slot, const uint8_t *frame, size_t n,
                   uint32_t deadline_ms);

/* True while `slot` has a job in flight. Advisory for the bridge's fast
 * reject; the requester task re-checks authoritatively on dequeue. */
bool fh_req_slot_busy(uint8_t slot);

/* Called from fh_hub_link.c route_frame() for EVERY decoded frame. For an
 * on-demand *_RESP type it matches the seq against the flight table and
 * completes the job (result file + flight clear). Radio-blade RESPs are only
 * ever clocked in by the requester's own polls, so this runs in the
 * requester task context; for any other frame it is a cheap no-op. */
void fh_req_on_frame(uint8_t type, const uint8_t *payload, uint8_t len);

/* The matching RESP TYPE for an on-demand REQ TYPE (frozen band, 0x20..0x2C:
 * every REQ's RESP is REQ+1 — PING 0x20->0x21, CC1101 0x22->0x23, NRF24
 * 0x24->0x25, NFC 0x26->0x27, LORA_TX 0x28->0x29, LORA_LISTEN 0x2B->0x2C).
 * Returns 0xFF for a type that is not an on-demand REQ. */
uint8_t fh_req_resp_type_for(uint8_t req_type);

/* Job counters for the SYS_STATE report (FH_OD_SYS_STATE_REQ, 0x2D). The
 * requester task increments these; the sysstate builder only reads them. */
typedef struct {
    uint32_t jobs_done;      /* completed on-demand jobs (RESP matched)       */
    uint32_t timeouts;       /* deadlines expired (synthetic TIMEOUT RESP)    */
    uint32_t busy_resps;     /* BUSY RESPs the blade staged during polling    */
} fh_req_stats_t;
fh_req_stats_t fh_req_stats(void);
