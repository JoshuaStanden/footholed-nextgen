/*
 * fh_hub_health.h — FHAdvanced hub health / node-liveness monitor (SUBSYSTEM 7)
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule).
 *
 * Tracks HEARTBEAT (0x7E) / PING (0x7D) / PONG (0x7C) from the four nodes
 * (c5=1, xs3=2, pico=3, head=4 — FROZEN src_id). head is liveness-only (PING/
 * PONG, no heartbeat yet). Four internal states collapse to a
 * single UP/DOWN for the operator (ARCHITECTURE "silent when healthy"):
 *   HEALTHY        — recent heartbeat, counters advancing
 *   QUIET_BUT_ALIVE— heartbeat present but capture_count flat (impaired/amber)
 *   HUNG           — missed beats; PING sent, awaiting PONG
 *   DEAD           — PING unanswered -> declare DOWN (red)
 * uptime_s reset to 0 between beats = a silent reboot (flagged).
 *
 * The aggregator feeds this monitor on every HEARTBEAT/PONG it decodes; the
 * monitor task ages entries and issues PINGs via a TX callback the aggregator
 * registers (the aggregator owns the UARTs). The UI reads fh_health_node().
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "fh_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FH_NODE_HEALTHY = 0,
    FH_NODE_QUIET_BUT_ALIVE,   /* alive but not making progress (amber)        */
    FH_NODE_HUNG,              /* missed beats; pinging                         */
    FH_NODE_DEAD,              /* unanswered; DOWN                              */
    FH_NODE_UNKNOWN,           /* never heard from since boot                   */
} fh_node_state_t;

typedef struct {
    fh_node_state_t state;
    bool     up;               /* collapsed UP/DOWN for the operator            */
    bool     silent_reboot;    /* uptime_s went backwards since last beat       */
    uint32_t last_uptime_s;    /* last reported uptime                          */
    uint32_t last_capture;     /* last reported capture_count                   */
    uint16_t last_dropped;     /* last reported dropped_keys (saturation)       */
    uint16_t last_free_kb;     /* last reported free heap KB (leak watch)       */
    uint32_t age_ms;           /* ms since the last heartbeat                   */
    uint8_t  task_alive;       /* last task_alive bitmask                       */
} fh_node_health_t;

/* TX hook the aggregator provides so the monitor can emit a PING down a link.
 * src_id selects which node (1=c5,2=xs3,3=pico,4=head). Returns true if sent. */
typedef bool (*fh_health_ping_fn)(uint8_t src_id, uint8_t seq);

/* Start the monitor task. `ping` is called when a node misses its beats.
 * Returns false on a resource failure. */
bool fh_health_start(fh_health_ping_fn ping);

/* Feed a decoded HEARTBEAT payload (fh_heartbeat_t) to the monitor. Called by
 * the aggregator frame handler. Copies what it needs; non-blocking. */
void fh_health_on_heartbeat(const fh_heartbeat_t *hb);

/* Feed a decoded PONG (echoes a PING seq) for the given node. */
void fh_health_on_pong(uint8_t src_id, uint8_t seq);

/* Any validated frame from a satellite also proves liveness. The Pico blade
 * never sends HEARTBEATs (it is polled/fired), so without this its entry is
 * never marked seen and the monitor never pings it -> permanently "dn" even
 * while GPS_FIX frames flow. Marks seen, refreshes the beat clock, promotes
 * UNKNOWN/HUNG -> HEALTHY. Does NOT touch heartbeat metrics (uptime/capture),
 * so heartbeat-driven progress checks stay authoritative for heartbeating
 * nodes. Must be called with a src_id carried by the link (0 = no owner). */
void fh_health_on_frame(uint8_t src_id);

/* Snapshot of one node's health (src_id 1..4). Out-of-range => UNKNOWN. */
fh_node_health_t fh_health_node(uint8_t src_id);

/* Count of nodes currently DOWN (for the status bar's single red summary). */
uint8_t fh_health_down_count(void);

#ifdef __cplusplus
}
#endif
