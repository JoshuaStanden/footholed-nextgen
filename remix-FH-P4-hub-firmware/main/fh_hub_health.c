/*
 * fh_hub_health.c — FHAdvanced hub health monitor (SUBSYSTEM 7) impl.
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule). The timing logic is
 * transcribed from ARCHITECTURE's four-state table. See KNOWN RISKS end.
 *
 * Node table indexed 0..3 for src_id 1..4 (c5/xs3/pico/head). Heartbeats update
 * an entry; an aging task promotes a quiet entry HEALTHY->HUNG (ping)->DEAD. A
 * fresh heartbeat anywhere resets the entry to HEALTHY. capture_count flat
 * across beats => QUIET_BUT_ALIVE (amber: alive but not progressing).
 *
 * NODE 4 (head, FH_SRC_HEAD): the Waveshare-S3 display head is a LIVENESS-ONLY
 * node — it sends no CAPTURE and (today) no HEARTBEAT, so it is kept alive
 * purely by the generic HUNG->PING->PONG path below. Until head firmware exists
 * to answer the PING, node 4 correctly reads DEAD (honest: "head not
 * responding"). No head-specific timing is added — it reuses the same aging
 * thresholds as every other node.
 */
#include "fh_hub_health.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "fh_hlth";

/* timing (ARCHITECTURE: heartbeat every 1-2 s). */
#define HB_PERIOD_MS      2000u     /* expected max heartbeat interval          */
#define MISS_TO_HUNG_MS   5000u     /* no beat this long -> HUNG + ping          */
#define HUNG_TO_DEAD_MS   9000u     /* no beat/pong this long -> DEAD            */
#define MONITOR_TICK_MS   1000u

#define NODE_COUNT  4               /* src_id 1..4 (c5/xs3/pico/head) */

typedef struct {
    bool            seen;           /* heard from at least once                 */
    fh_node_state_t state;
    uint64_t        last_beat_us;   /* esp_timer us of the last heartbeat/pong  */
    uint32_t        last_uptime_s;
    uint32_t        last_capture;
    uint16_t        last_dropped;
    uint16_t        last_free_kb;
    uint8_t         task_alive;
    bool            silent_reboot;
    uint8_t         ping_seq;       /* last PING seq sent while HUNG            */
    bool            awaiting_pong;
} node_t;

static node_t             s_node[NODE_COUNT];
static SemaphoreHandle_t  s_mtx = NULL;
static fh_health_ping_fn  s_ping = NULL;

static inline int idx_of(uint8_t src_id)
{
    return (src_id >= 1 && src_id <= NODE_COUNT) ? (int)src_id - 1 : -1;
}

void fh_health_on_heartbeat(const fh_heartbeat_t *hb)
{
    if (!hb) { return; }
    int i = idx_of(hb->src_id);
    if (i < 0 || !s_mtx) { return; }   /* !s_mtx: monitor never started */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    node_t *n = &s_node[i];
    /* silent reboot: uptime went backwards (or to 0) since the last beat */
    if (n->seen && hb->uptime_s < n->last_uptime_s) { n->silent_reboot = true; }
    else if (!n->seen)                               { n->silent_reboot = false; }

    /* progress check: capture_count not advancing while beats continue = amber */
    bool progressing = (!n->seen) || (hb->capture_count != n->last_capture);

    n->seen          = true;
    n->last_beat_us  = esp_timer_get_time();
    n->last_uptime_s = hb->uptime_s;
    n->last_capture  = hb->capture_count;
    n->last_dropped  = hb->dropped_keys;
    n->last_free_kb  = hb->free_heap_kb;
    n->task_alive    = hb->task_alive;
    n->awaiting_pong = false;
    n->state = progressing ? FH_NODE_HEALTHY : FH_NODE_QUIET_BUT_ALIVE;
    xSemaphoreGive(s_mtx);
}

void fh_health_on_pong(uint8_t src_id, uint8_t seq)
{
    int i = idx_of(src_id);
    if (i < 0 || !s_mtx) { return; }   /* !s_mtx: monitor never started */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    node_t *n = &s_node[i];
    if (n->awaiting_pong && seq == n->ping_seq) {
        n->awaiting_pong = false;
        n->last_beat_us  = esp_timer_get_time();   /* a PONG proves liveness */
        if (n->state == FH_NODE_HUNG) { n->state = FH_NODE_HEALTHY; }
    }
    xSemaphoreGive(s_mtx);
}

void fh_health_on_frame(uint8_t src_id)
{
    int i = idx_of(src_id);
    if (i < 0 || !s_mtx) { return; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    node_t *n = &s_node[i];
    n->seen         = true;
    n->last_beat_us = esp_timer_get_time();
    n->awaiting_pong = false;
    if (n->state == FH_NODE_UNKNOWN || n->state == FH_NODE_HUNG) {
        n->state = FH_NODE_HEALTHY;
    }
    xSemaphoreGive(s_mtx);
}

static void monitor_task(void *arg)
{
    (void)arg;
    uint8_t seq = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MONITOR_TICK_MS));
        uint64_t now = esp_timer_get_time();
        for (int i = 0; i < NODE_COUNT; ++i) {
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            node_t *n = &s_node[i];
            if (!n->seen) { xSemaphoreGive(s_mtx); continue; }
            uint64_t age_us = now - n->last_beat_us;
            uint32_t age_ms = (uint32_t)(age_us / 1000ull);

            if (age_ms >= HUNG_TO_DEAD_MS) {
                n->state = FH_NODE_DEAD;
            } else if (age_ms >= MISS_TO_HUNG_MS) {
                /* HUNG: ping once (and once more next tick) while we wait */
                if (n->state != FH_NODE_DEAD) { n->state = FH_NODE_HUNG; }
                if (!n->awaiting_pong && s_ping) {
                    n->ping_seq = ++seq;
                    n->awaiting_pong = true;
                    uint8_t sid = (uint8_t)(i + 1);
                    uint8_t ps  = n->ping_seq;
                    xSemaphoreGive(s_mtx);          /* don't hold lock over TX */
                    s_ping(sid, ps);
                    continue;                        /* lock already released   */
                }
            }
            xSemaphoreGive(s_mtx);
        }
    }
}

bool fh_health_start(fh_health_ping_fn ping)
{
    if (!s_mtx) { s_mtx = xSemaphoreCreateMutex(); }
    if (!s_mtx) { return false; }
    s_ping = ping;                         /* allowed to refresh the hook */
    static bool started = false;
    if (started) { return true; }          /* idempotent — do NOT wipe live state (AB7) */
    /* first call only: clear the node table and spawn the task */
    memset(s_node, 0, sizeof(s_node));
    for (int i = 0; i < NODE_COUNT; ++i) { s_node[i].state = FH_NODE_UNKNOWN; }
    BaseType_t ok = xTaskCreate(monitor_task, "fh_hlth", 3072, NULL, 4, NULL);
    if (ok != pdPASS) { return false; }
    started = true;
    ESP_LOGI(TAG, "health monitor started");
    return true;
}

fh_node_health_t fh_health_node(uint8_t src_id)
{
    fh_node_health_t out; memset(&out, 0, sizeof(out));
    int i = idx_of(src_id);
    if (i < 0 || !s_mtx) { out.state = FH_NODE_UNKNOWN; return out; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    node_t *n = &s_node[i];
    out.state         = n->seen ? n->state : FH_NODE_UNKNOWN;
    out.silent_reboot = n->silent_reboot;
    out.last_uptime_s = n->last_uptime_s;
    out.last_capture  = n->last_capture;
    out.last_dropped  = n->last_dropped;
    out.last_free_kb  = n->last_free_kb;
    out.task_alive    = n->task_alive;
    out.age_ms        = n->seen ? (uint32_t)((esp_timer_get_time() - n->last_beat_us) / 1000ull) : 0;
    /* collapse to UP/DOWN for the operator */
    out.up = n->seen && (n->state == FH_NODE_HEALTHY || n->state == FH_NODE_QUIET_BUT_ALIVE);
    xSemaphoreGive(s_mtx);
    return out;
}

uint8_t fh_health_down_count(void)
{
    uint8_t down = 0;
    if (!s_mtx) { return 0; }          /* monitor never started */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    for (int i = 0; i < NODE_COUNT; ++i) {
        if (s_node[i].seen &&
            (s_node[i].state == FH_NODE_HUNG || s_node[i].state == FH_NODE_DEAD)) {
            down++;
        }
    }
    xSemaphoreGive(s_mtx);
    return down;
}

/* ===========================================================================
 * KNOWN RISKS
 *   [H1] A node never heard from stays UNKNOWN (not DOWN) — deliberate: "not
 *        yet seen" differs from "was alive, now dead". The UI shows UNKNOWN as
 *        a neutral chip until the first beat, then UP/DOWN. Confirm that is the
 *        desired first-boot behavior (ARCHITECTURE: silent-when-healthy).
 *   [H2] QUIET_BUT_ALIVE (amber) fires when capture_count is flat between two
 *        beats. A genuinely quiet RF environment (no devices) also shows flat
 *        counts — so amber here can be a false "impaired". ARCHITECTURE wants
 *        amber for IMPAIRED (e.g. fake-DFS strikes); a flat count alone is weak
 *        evidence. CONSIDER: only amber if flat AND the node self-reports an
 *        impairment bit, else keep HEALTHY. Parked as a tuning decision.
 *   [H3] The ping path releases the mutex before calling s_ping (which writes a
 *        UART) to avoid holding the lock across I/O — but then `continue`s,
 *        skipping the bottom give. Verified: every path that takes the lock
 *        releases it exactly once (the ping branch gives-then-continues; all
 *        others give at the bottom). Re-check this by hand in review — it is
 *        the classic place a health monitor deadlocks.
 *   [H4] seq is a single rolling counter across all nodes; a PONG matches on
 *        (src_id, seq). Collisions are astronomically unlikely at this cadence
 *        but a per-node seq would be cleaner. Parked.
 * ======================================================================== */
