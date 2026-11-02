/*
 * fh_c5_main.c — FHAdvanced-REMIX XIAO-C5 firmware (5 GHz Wi-Fi sniffer).
 *
 * ############################################################################
 * #  UNVERIFIED FIRST DRAFT — written WITHOUT a compiler (project rule).     #
 * #  PORT of the gen1 UART sniffer to a SPI SLAVE on the P4 hub's Bus A.     #
 * #  The CAPTURE PATH IS UNCHANGED (promiscuous cb -> seen-table -> queue);  #
 * #  ONLY the transport moved (UART writer -> SPI-slave link + IRQ). EXPECT  #
 * #  a compile-fix pass on first build. KNOWN RISKS at the bottom.           #
 * ############################################################################
 *
 * ROLE: pure 5 GHz Wi-Fi promiscuous sniffer. Identity dedup, CAPTURE on a
 * new identity, SSID mapping on beacons/probe-resps (concern #3: one
 * FH_TYPE_SSID per BSSID, on first sight + on SSID change), periodic SUMMARY,
 * always-on HEARTBEAT — fh_protocol frames to the hub. src_id =
 * FH_SRC_C5_WIFI5 (1), band nibble 0x5, 5 GHz channels.
 *
 * TRANSPORT (REMIX): the hub is the SPI MASTER; this board is a SPI SLAVE. A
 * slave cannot push — so the TX task STAGES the next queued frame into the
 * slave's transaction buffer, raises FH_C5_IRQ_OUT ("come clock me"), and the
 * hub's transaction clocks that frame out (and any control frame in). The IRQ
 * is lowered once the queue drains. See fh_c5_link.c.
 *
 * CONCURRENCY MODEL (UNCHANGED from gen1 — load-bearing):
 *   - Wi-Fi promiscuous callback NEVER blocks: 0-tick try-lock on the seen-
 *     table + 0-tick enqueue (drop+count on contention/full). No transport I/O.
 *   - The SPI-slave link task is the ONLY transport path; it drains the queue.
 *   - summary/heartbeat run in app_main and enqueue (short wait), never I/O.
 */
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "fh_frame.h"
#include "fh_c5_pins.h"
#include "fh_c5_link.h"      /* SPI-slave transport (replaces the UART path)  */

static const char *TAG = "fh_c5";

/* ---- Tunables (capture side UNCHANGED from gen1) ------------------------- */
#define C5_RELOG_WINDOW_MS    60000u
#define C5_SUMMARY_PERIOD_MS  8000u
#define C5_HEARTBEAT_MS       1500u
#define C5_SEEN_MAX           512u

#define C5_CB_LOCK_TICKS      0
#define C5_APP_LOCK_TICKS     pdMS_TO_TICKS(50)

#define C5_TXQ_DEPTH          64
#define C5_TX_FRAME_MAX       64u

#define C5_DEBUG_CONSOLE      1

/* 5 GHz bring-up: START at 20 MHz / WIFI_SECOND_CHAN_NONE, verify HE (11ax)
 * reception, THEN widen. Non-DFS subset to start with. VERIFY this list is
 * legal for your regulatory domain. (The C5 is NEW SILICON — IDF 6.1 needed.) */
static const uint8_t k5ghz_channels[] = { 36, 40, 44, 48, 149, 153, 157, 161 };
#define C5_N_CHANNELS  (sizeof(k5ghz_channels)/sizeof(k5ghz_channels[0]))
#define C5_DWELL_MS    110u

/* ---- TX queue record (UNCHANGED) ---------------------------------------- */
typedef struct {
    uint16_t len;
    uint8_t  bytes[C5_TX_FRAME_MAX];
} tx_msg_t;
static QueueHandle_t s_txq = NULL;

/* ---- Seen-table (UNCHANGED) --------------------------------------------- */
typedef struct {
    uint8_t  mac[6];
    uint8_t  used;
    uint8_t  channel;
    uint16_t count;
    uint32_t last_seen_ms;
} seen_entry_t;

static seen_entry_t      s_seen[C5_SEEN_MAX];
static SemaphoreHandle_t s_seen_mtx = NULL;

static uint32_t s_dropped_keys  = 0;
static uint32_t s_dropped_tx    = 0;
static uint32_t s_capture_count = 0;

/* ---- Producer enqueue (UNCHANGED): encode, bounds-check, 0-tick send ----- */
static bool txq_try_send(fh_type_t type, const uint8_t *payload, uint8_t len)
{
    if (!s_txq) { return false; }
    uint8_t scratch[FH_MAX_FRAME];
    size_t n = fh_frame_encode(type, payload, len, scratch, sizeof(scratch));
    if (n == 0 || n > C5_TX_FRAME_MAX) { return false; }
    tx_msg_t msg;
    msg.len = (uint16_t)n;
    memcpy(msg.bytes, scratch, n);
    return xQueueSend(s_txq, &msg, 0) == pdTRUE;
}

/* ---- band_chan helper (UNCHANGED) --------------------------------------- */
static inline uint8_t make_band_chan(uint8_t chan_index)
{
    return (uint8_t)((0x5u << 4) | (chan_index & 0x0Fu));
}

/* ---- Seen-table ops (UNCHANGED) ----------------------------------------- */
static seen_entry_t *seen_find(const uint8_t mac[6])
{
    for (uint32_t i = 0; i < C5_SEEN_MAX; ++i) {
        if (s_seen[i].used && memcmp(s_seen[i].mac, mac, 6) == 0) { return &s_seen[i]; }
    }
    return NULL;
}
static seen_entry_t *seen_insert(const uint8_t mac[6], uint32_t now_ms)
{
    seen_entry_t *free_slot = NULL;
    for (uint32_t i = 0; i < C5_SEEN_MAX; ++i) {
        if (!s_seen[i].used) { free_slot = &s_seen[i]; break; }
    }
    if (!free_slot) {
        uint32_t oldest_age = 0;
        for (uint32_t i = 0; i < C5_SEEN_MAX; ++i) {
            uint32_t age = now_ms - s_seen[i].last_seen_ms;
            if (age >= C5_RELOG_WINDOW_MS && age >= oldest_age) {
                oldest_age = age; free_slot = &s_seen[i];
            }
        }
        if (!free_slot) { return NULL; }
        s_dropped_keys++;
    }
    memset(free_slot, 0, sizeof(*free_slot));
    memcpy(free_slot->mac, mac, 6);
    free_slot->used = 1;
    free_slot->last_seen_ms = now_ms;
    return free_slot;
}

/* ---- SSID extraction (concern #3, 2026-10-31) ----------------------------
 * The hub + Medusa are ready for FH_TYPE_SSID (0x04) but no sniffer sent one.
 * For beacons (subtype 0x8) and probe responses (0x5) we walk the tagged
 * parameters after the 24-byte MAC header, find tag 0 (SSID), bounds-check
 * EVERY length against the packet, and send ONE fh_ssid_t (39 B) per BSSID:
 * once on first sight, again ONLY if the SSID changed (FNV-1a hash compare).
 * Non-blocking: 0-tick lock + 0-tick enqueue, same posture as CAPTURE. */
#define C5_SSID_MAX          128u
typedef struct {
    uint8_t  mac[6];
    uint8_t  used;
    uint8_t  ssid_len;        /* length of the LAST SENT SSID (0 = hidden)     */
    uint32_t ssid_hash;       /* FNV-1a of the last sent SSID bytes            */
    uint32_t last_ms;         /* for oldest-entry reuse when the table is full */
} ssid_entry_t;
static ssid_entry_t      s_ssid[C5_SSID_MAX];
static SemaphoreHandle_t s_ssid_mtx = NULL;
static uint32_t s_dropped_ssid = 0;

static uint32_t fnv1a(const uint8_t *d, uint8_t n)
{
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < n; ++i) { h ^= d[i]; h *= 16777619u; }
    return h;
}

/* payload = the MAC header of a MGMT frame; plen = sig_len - 4 (FCS stripped).
 * Walks the tagged parameters (start at offset 24) for tag 0 = SSID. */
static void ssid_maybe_send(const uint8_t *payload, int plen, uint32_t now_ms)
{
    const uint8_t subtype = (uint8_t)((payload[1] >> 4) & 0x0Fu);
    if (subtype != 0x8 && subtype != 0x5) { return; }   /* beacon / probe resp */
    /* 802.11: MAC header 24 B, then fixed fields — timestamp (8) + beacon
     * interval (2) + capability (2) = 12 B — THEN the tagged parameters.
     * For these two subtypes the IEs start at offset 36, not 24 (24 is only
     * right for probe requests, which we do not parse). */
    if (plen < 38) { return; }                          /* 24 + 12 + tag hdr   */

    const uint8_t *bssid = &payload[16];                /* addr3 = BSSID here  */
    const uint8_t *p     = &payload[36];
    const uint8_t *end   = payload + plen;
    const uint8_t *ssid  = NULL;
    uint8_t ssid_len = 0;
    while (p + 2 <= end) {
        uint8_t tag_no = p[0], tag_len = p[1];
        if (p + 2 + (int)tag_len > end) { break; }      /* truncated tag: stop */
        if (tag_no == 0) {                              /* the SSID tag        */
            ssid = &p[2];
            ssid_len = (tag_len > 32) ? 32 : tag_len;
            break;
        }
        p += 2 + tag_len;
    }
    if (!ssid) { return; }  /* no SSID tag present (tag 0 len 0 = hidden:
                             * we DO send that, as an ssid_len=0 mapping) */

    uint32_t hash = fnv1a(ssid, ssid_len);
    bool send = false;
    if (s_ssid_mtx && xSemaphoreTake(s_ssid_mtx, C5_CB_LOCK_TICKS) == pdTRUE) {
        ssid_entry_t *e = NULL;
        for (uint32_t i = 0; i < C5_SSID_MAX; ++i) {
            if (s_ssid[i].used && memcmp(s_ssid[i].mac, bssid, 6) == 0) {
                e = &s_ssid[i]; break;
            }
        }
        bool changed;
        if (e) {
            changed = (e->ssid_len != ssid_len) || (e->ssid_hash != hash);
        } else {
            /* new BSSID: take a free slot, else reuse the OLDEST entry */
            for (uint32_t i = 0; i < C5_SSID_MAX; ++i) {
                if (!s_ssid[i].used) { e = &s_ssid[i]; break; }
            }
            if (!e) {
                uint32_t best = 0, best_age = 0;
                for (uint32_t i = 0; i < C5_SSID_MAX; ++i) {
                    uint32_t age = now_ms - s_ssid[i].last_ms;
                    if (age >= best_age) { best_age = age; best = i; }
                }
                e = &s_ssid[best];
            }
            changed = true;
        }
        if (changed) {
            e->used      = 1;
            memcpy(e->mac, bssid, 6);
            e->ssid_len  = ssid_len;
            e->ssid_hash = hash;
            e->last_ms   = now_ms;
            send = true;
        }
        xSemaphoreGive(s_ssid_mtx);
    }
    if (!send) { return; }

    fh_ssid_t sd = {0};
    memcpy(sd.mac, bssid, 6);
    sd.ssid_len = ssid_len;
    memcpy(sd.ssid, ssid, ssid_len);
    if (txq_try_send(FH_TYPE_SSID, (const uint8_t *)&sd, (uint8_t)sizeof(fh_ssid_t))) {
        return;
    }
    /* Queue full: the entry above was recorded as sent, so revert it so the
     * next beacon retries (drop-newest on the TX queue, our standing rule). */
    if (s_ssid_mtx && xSemaphoreTake(s_ssid_mtx, C5_CB_LOCK_TICKS) == pdTRUE) {
        ssid_entry_t *e = NULL;
        for (uint32_t i = 0; i < C5_SSID_MAX; ++i) {
            if (s_ssid[i].used && memcmp(s_ssid[i].mac, bssid, 6) == 0) {
                e = &s_ssid[i]; break;
            }
        }
        if (e) { e->ssid_hash = 0; e->ssid_len = 0; }   /* force a re-send     */
        xSemaphoreGive(s_ssid_mtx);
    }
    s_dropped_ssid++;
}

/* ---- Promiscuous RX callback (UNCHANGED — non-blocking by construction) -- */
static volatile uint8_t  s_cur_chan_index = 0;
static volatile uint32_t s_now_ms = 0;

static void wifi_promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) { return; }
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 16) { return; }

    const uint8_t *payload = pkt->payload;
    const int8_t   rssi    = pkt->rx_ctrl.rssi;
    const uint8_t *mac     = &payload[10];

    uint32_t now_ms   = s_now_ms;
    uint8_t  ch_index = s_cur_chan_index;
    uint8_t  ch_real  = k5ghz_channels[ch_index % C5_N_CHANNELS];

    /* SSID beacon/probe-resp extraction (concern #3) — MGMT frames only, and
     * only for beacons/probe-resps inside the helper. -4 strips the FCS. */
    if (type == WIFI_PKT_MGMT) {
        ssid_maybe_send(payload, pkt->rx_ctrl.sig_len - 4, now_ms);
    }

    fh_capture_t cap = {0};
    bool emit = false;

    if (s_seen_mtx && xSemaphoreTake(s_seen_mtx, C5_CB_LOCK_TICKS) == pdTRUE) {
        seen_entry_t *e = seen_find(mac);
        if (e) {
            if (e->count != 0xFFFF) { e->count++; }
            e->last_seen_ms = now_ms;
        } else {
            e = seen_insert(mac, now_ms);
            if (!e) {
                s_dropped_keys++;
            } else {
                e->channel = ch_real;
                e->count   = 1;
                memset(&cap, 0, sizeof(cap));
                cap.src_id     = FH_SRC_C5_WIFI5;
                cap.band_chan  = make_band_chan(ch_index);
                cap.frame_type = (type == WIFI_PKT_MGMT) ? 0x00 : 0x20;
                cap.rssi       = rssi;
                memcpy(cap.mac, mac, 6);
                cap.count      = 1;
                cap.extra[0]   = ch_real;
                s_capture_count++;
                emit = true;
            }
        }
        xSemaphoreGive(s_seen_mtx);
    }

    if (emit) {
        if (!txq_try_send(FH_TYPE_CAPTURE, (const uint8_t *)&cap,
                          (uint8_t)(FH_CAPTURE_FIXED + 1u))) {
            if (s_seen_mtx && xSemaphoreTake(s_seen_mtx, C5_CB_LOCK_TICKS) == pdTRUE) {
                s_dropped_tx++;
                xSemaphoreGive(s_seen_mtx);
            }
        }
    }
}

/* ---- Periodic SUMMARY (UNCHANGED) --------------------------------------- */
static void summary_tick(uint32_t now_ms)
{
    for (uint32_t base = 0; base < C5_SEEN_MAX; ) {
        fh_summary_t batch[16];
        uint32_t nb = 0;
        if (!(s_seen_mtx && xSemaphoreTake(s_seen_mtx, C5_APP_LOCK_TICKS) == pdTRUE)) {
            return;
        }
        uint32_t i = base;
        for (; i < C5_SEEN_MAX && nb < 16; ++i) {
            seen_entry_t *e = &s_seen[i];
            if (!e->used || e->count == 0) { continue; }
            fh_summary_t *sum = &batch[nb++];
            memset(sum, 0, sizeof(*sum));
            sum->src_id      = FH_SRC_C5_WIFI5;
            sum->band_chan   = (uint8_t)((0x5u << 4) | 0x0u);
            memcpy(sum->mac, e->mac, 6);
            sum->count       = e->count;
            sum->last_seen_s = e->last_seen_ms / 1000u;
            e->count = 0;
            if ((now_ms - e->last_seen_ms) >= C5_RELOG_WINDOW_MS) { e->used = 0; }
        }
        base = i;
        xSemaphoreGive(s_seen_mtx);

        for (uint32_t k = 0; k < nb; ++k) {
            if (!txq_try_send(FH_TYPE_SUMMARY, (const uint8_t *)&batch[k],
                              (uint8_t)sizeof(fh_summary_t))) {
                if (s_seen_mtx && xSemaphoreTake(s_seen_mtx, C5_APP_LOCK_TICKS) == pdTRUE) {
                    s_dropped_tx++;
                    xSemaphoreGive(s_seen_mtx);
                }
            }
        }
    }
}

/* ---- HEARTBEAT (UNCHANGED) ---------------------------------------------- */
static void heartbeat_tick(void)
{
    uint32_t cap_count = 0, dropped = 0;
    if (s_seen_mtx && xSemaphoreTake(s_seen_mtx, C5_APP_LOCK_TICKS) == pdTRUE) {
        cap_count = s_capture_count;
        dropped   = s_dropped_keys + s_dropped_tx + s_dropped_ssid;
        xSemaphoreGive(s_seen_mtx);
    }
    fh_heartbeat_t hb;
    memset(&hb, 0, sizeof(hb));
    hb.src_id        = FH_SRC_C5_WIFI5;
    hb.uptime_s      = (uint32_t)(esp_timer_get_time() / 1000000);
    hb.capture_count = cap_count;
    hb.dropped_keys  = (uint16_t)(dropped & 0xFFFF);
    hb.task_alive    = FH_TASK_CAPTURE;
    hb.free_heap_kb  = (uint16_t)(esp_get_free_heap_size() / 1024u);
    txq_try_send(FH_TYPE_HEARTBEAT, (const uint8_t *)&hb, (uint8_t)sizeof(hb));

#if C5_DEBUG_CONSOLE
    static uint32_t s_beat = 0;
    ESP_LOGI(TAG, "up=%lus ch=%u caps=%lu dropped=%lu heap=%uKB",
             (unsigned long)hb.uptime_s,
             (unsigned)k5ghz_channels[s_cur_chan_index % C5_N_CHANNELS],
             (unsigned long)cap_count, (unsigned long)dropped,
             (unsigned)hb.free_heap_kb);
    (void)s_beat;
#endif
}

/* ---- Wi-Fi init: 5 GHz promiscuous (band-mode 5G_ONLY — the C5 delta) ----
 * This is the ONE real capture-path difference from the XIAO-S3: the C5 is a
 * dual-band part and MUST be told to run 5 GHz only. Start at 20 MHz /
 * WIFI_SECOND_CHAN_NONE and verify HE (11ax) RX before widening (IDF 6.0 had an
 * HE/secondary-channel quirk; 6.1 status unconfirmed — bench item). */
static void wifi_start_5_promisc(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY));  /* C5 delta */
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&wifi_promisc_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    s_cur_chan_index = 0;
    ESP_ERROR_CHECK(esp_wifi_set_channel(k5ghz_channels[0], WIFI_SECOND_CHAN_NONE));
    ESP_LOGI(TAG, "5GHz promiscuous up; ch %u (20MHz, verify HE/11ax RX)",
             k5ghz_channels[0]);
}

/* ---- control-frame handler: frames the hub clocked IN to this slave ------
 * The SPI-slave link calls this for each validated inbound frame (PING, time,
 * future control). Keep it short + non-blocking. PONG replies are enqueued. */
static void on_control_frame(fh_type_t type, const uint8_t *payload, uint8_t len)
{
    switch (type) {
        case FH_TYPE_PING: {
            if (len >= sizeof(fh_ping_t)) {
                fh_ping_t p; memcpy(&p, payload, sizeof(p));
                txq_try_send(FH_TYPE_PONG, (const uint8_t *)&p, sizeof(p));
            }
            break;
        }
        default: break;   /* time/other control: parked until needed */
    }
}

/* ---- queue drain hook for the SPI-slave link ----------------------------
 * The link task calls this to pull the next frame to stage into the slave TX
 * buffer. Returns the byte count (0 if the queue is empty -> link lowers IRQ).
 * `out` must hold >= FH_MAX_FRAME. */
static size_t txq_pop(uint8_t *out, size_t cap)
{
    tx_msg_t msg;
    if (s_txq && xQueueReceive(s_txq, &msg, 0) == pdTRUE) {
        size_t n = msg.len <= cap ? msg.len : cap;
        memcpy(out, msg.bytes, n);
        return n;
    }
    return 0;
}

/* ---- Main --------------------------------------------------------------- */
void app_main(void)
{
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    memset(s_seen, 0, sizeof(s_seen));
    memset(s_ssid, 0, sizeof(s_ssid));
    s_seen_mtx = xSemaphoreCreateMutex();
    s_ssid_mtx = xSemaphoreCreateMutex();
    s_txq      = xQueueCreate(C5_TXQ_DEPTH, sizeof(tx_msg_t));
    if (s_seen_mtx == NULL || s_ssid_mtx == NULL || s_txq == NULL) {
        ESP_LOGE(TAG, "alloc failed (mtx=%p txq=%p)", (void *)s_seen_mtx, (void *)s_txq);
        abort();
    }

    /* SPI-slave transport up BEFORE the Wi-Fi callback can fire (the callback
     * enqueues; the link drains). The link owns FH_C5_IRQ_OUT and the slave
     * bus pins (fh_c5_pins.h). It calls txq_pop() to fetch frames and
     * on_control_frame() for inbound control. */
    fh_c5_link_cfg_t lc = {
        .pop        = txq_pop,
        .on_control = on_control_frame,
    };
    if (!fh_c5_link_start(&lc)) {
        ESP_LOGE(TAG, "SPI-slave link start failed");
        abort();
    }

    s_now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    wifi_start_5_promisc();

    uint32_t last_hop_ms = 0, last_summary_ms = 0, last_heartbeat_ms = 0;
    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_now_ms = now_ms;
        if (now_ms - last_hop_ms >= C5_DWELL_MS) {
            last_hop_ms = now_ms;
            s_cur_chan_index = (uint8_t)((s_cur_chan_index + 1) % C5_N_CHANNELS);
            esp_wifi_set_channel(k5ghz_channels[s_cur_chan_index], WIFI_SECOND_CHAN_NONE);
        }
        if (now_ms - last_summary_ms >= C5_SUMMARY_PERIOD_MS) {
            last_summary_ms = now_ms;
            summary_tick(now_ms);
        }
        if (now_ms - last_heartbeat_ms >= C5_HEARTBEAT_MS) {
            last_heartbeat_ms = now_ms;
            heartbeat_tick();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ============================================================================
 * KNOWN COMPILE RISKS / FIRST-BUILD CHECKLIST (UNVERIFIED)
 * ----------------------------------------------------------------------------
 * 1. CAPTURE PATH UNCHANGED from the proven gen1 firmware — the risk surface is
 *    the SPI-slave transport in fh_c5_link.c, not here.
 * 2. main/CMakeLists REQUIRES: esp_wifi esp_event esp_netif nvs_flash esp_timer
 *    esp_driver_spi (spi_slave_hd) driver + fh_protocol.
 * 3. The callback still NEVER blocks (0-tick lock + 0-tick enqueue). The link
 *    task is the only transport path. Do NOT do SPI I/O from the callback.
 * 4. Control-frame PONG: the hub's health monitor PINGs this slave; we reply
 *    PONG via the same queue, clocked out on the next transaction.
 * 5. 5 GHz BAND-MODE: esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY) is the one
 *    real capture-path delta vs the XIAO-S3. Confirm the symbol exists on YOUR
 *    IDF 6.1 and that 5 GHz promiscuous RX actually works (the C5 is new
 *    silicon). Start 20 MHz / WIFI_SECOND_CHAN_NONE; verify HE (11ax) RX before
 *    widening (IDF 6.0 HE/secondary-channel quirk; 6.1 status unconfirmed).
 * 6. CS/IRQ are on GPIO12/11 (C5 silkscreen D7/D6) — NOT the S3's 43/44. No
 *    UART0 console-pin collision here; still VERIFY free on the board.
 * 7. VERIFY the 5 GHz channel list for your regulatory domain; the starter set
 *    { 36,40,44,48,149,153,157,161 } is a non-DFS subset — DFS channels need
 *    radar-detect handling not done here.
 * 8. SSID path (concern #3, added 2026-10-31): an fh_ssid_t frame is 45 B on
 *    the wire (2 hdr + 2 len + 39 payload + 2 crc) — under C5_TX_FRAME_MAX (64),
 *    so the queue depth is unchanged. Every beacon from a KNOWN BSSID does one
 *    hash compare per packet (cheap); the SSID tag walk is bounded by the
 *    packet length, so a malformed/truncated beacon can't overrun the buffer.
 *    A 128-BSSID SSID table shares nothing with the seen-table (separate
 *    mutex), so the capture path is unaffected.
 * ========================================================================== */
