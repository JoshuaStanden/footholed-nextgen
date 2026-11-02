/*
 * fh_hub_ble.c - FHAdvanced-REMIX hub BLE sniffer (P4 onboard C6) - PASS 2
 *
 * UNVERIFIED FIRST DRAFT (no compiler run on the authoring machine).
 *
 * What it does: the P4 has no BLE radio of its own. The BLE controller is the
 * onboard ESP32-C6, reached over SDIO through ESP-Hosted (VHCI transport). The
 * NimBLE HOST runs here on the P4; the controller runs on the C6. This file:
 *   1. connects to the C6 (esp_hosted_connect_to_slave) and brings up its BT
 *      controller (esp_hosted_bt_controller_init/enable),
 *   2. starts the NimBLE host,
 *   3. on sync, starts a GAP discovery (PASSIVE by default, ACTIVE if
 *      cfg->ble_scan_mode says so),
 *   4. dedups advertisers by (address type + address) - "seen is seen" - and
 *      submits one ws-ble row per NEW advertiser to the logger.
 *
 * Bring-up runs in its own task so a slow/absent C5 never blocks hub boot.
 * Failure is non-fatal: running stays false, the other sources keep logging.
 *
 * Counters are written only from the NimBLE host task (and the bring-up task
 * before the host runs); fh_ble_stats() copies them without a lock. Each field
 * is a naturally-aligned 32-bit/8-bit value, so a torn read is not possible,
 * but the set of fields is not a consistent snapshot. Fine for a status chip.
 */
#include "fh_hub_ble.h"
#include "fh_hub_config.h"
#include "fh_hub_logger.h"
#include "fh_hub_bridge.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_hosted.h"           /* esp_hosted_connect_to_slave()             */
#include "esp_hosted_misc.h"      /* esp_hosted_bt_controller_init/enable()    */

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "fh_ble";

/* ---- tunables ------------------------------------------------------------ */
/* Re-log window. 0 = "seen is seen": an advertiser is logged ONCE per boot.
 * Any other value (ms) logs it again if it is heard after that much time. */
#define FH_BLE_RELOG_WINDOW_MS   0u

#define FH_BLE_SEEN_SLOTS        512u   /* power of two                        */
#define FH_BLE_PROBE_MAX         8u     /* linear-probe depth before eviction  */

/* Display band code for BLE rows (see fh_log_record_t.band). */
#define FH_BLE_BAND_CODE         1u

/* ---- seen table ---------------------------------------------------------- */
typedef struct {
    uint8_t  addr[6];     /* NimBLE little-endian address bytes                */
    uint8_t  type;        /* BLE_ADDR_PUBLIC / RANDOM / ...                    */
    bool     used;
    uint32_t last_ms;     /* when it was last LOGGED                           */
    int8_t   rssi;        /* latest RSSI heard                                 */
    uint8_t  ev;          /* latest GAP event type                             */
    /* advertisement metadata, sticky (a scan response may lack what the adv
     * carried, so a field is only overwritten when a report has it). */
    uint8_t  meta_have;   /* FH_BLE_HAVE_* bits                                */
    uint16_t company;     /* manufacturer-data company ID (Bluetooth SIG)      */
    uint8_t  adflags;     /* AD Flags byte                                     */
    uint16_t svc16;       /* first advertised 16-bit service UUID              */
    int8_t   txp;         /* advertised TX power, dBm                          */
} seen_t;

/* CAPTURE extra[] layout for src 5 (BLE). extra is opaque/type-specific in the
 * frozen protocol, so this needs NO protocol change; LEN = FIXED + 10.
 *   [0] channel (0 = unknown)   [1] addr type   [2] GAP event type
 *   [3..4] company ID, LE       [5] AD flags    [6..7] 16-bit service UUID, LE
 *   [8] TX power (int8)         [9] FH_BLE_HAVE_* presence bits
 * Medusa decodes this in tui.py (_ble_meta). Keep the two in step. */
#define FH_BLE_HAVE_COMPANY   0x01u
#define FH_BLE_HAVE_FLAGS     0x02u
#define FH_BLE_HAVE_SVC16     0x04u
#define FH_BLE_HAVE_TXP       0x08u
#define FH_BLE_HAVE_NAME      0x10u
#define FH_BLE_EXTRA_LEN      10u

/* BENCH FIX (2026-10-30): Medusa got nothing after ATTACH. The SD log is
 * "seen is seen", and the C6's old controller firmware appears to report each
 * advertiser only ~once (no repeat DISC events), so a mirror driven by new
 * events never fired again. Instead a feed task sends the WHOLE seen table to
 * Medusa once per attach (owner's choice, 2026-10-30: a 5 s resend flooded the
 * stream). Worst case 512 x 40 B = ~20 KB, ~0.2 s at 921600 baud. */
#define FH_BLE_FEED_POLL_MS      250u   /* how often to check for a new attach */
#define FH_BLE_RESCAN_MS       10000u   /* restart scan => fresh report per device */

static seen_t           s_seen[FH_BLE_SEEN_SLOTS];
static portMUX_TYPE     s_seen_mux = portMUX_INITIALIZER_UNLOCKED;  /* host task vs feed task */
static fh_ble_stats_t   s_stats;      /* all-zero at boot                      */
static uint8_t          s_own_addr_type;
static bool             s_passive = true;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint32_t hash_key(const uint8_t addr[6], uint8_t type)
{
    uint32_t h = 2166136261u;                       /* FNV-1a */
    for (int i = 0; i < 6; i++) { h ^= addr[i]; h *= 16777619u; }
    h ^= type; h *= 16777619u;
    return h;
}

/* Records this advertiser and returns its table entry. *log_now is true if it
 * should be LOGGED to SD now (new, or past the re-log window). Evicts the
 * stalest probed slot if the probe window is full. */
static seen_t *seen_check_and_mark(const uint8_t addr[6], uint8_t type, bool *log_now)
{
    const uint32_t t = now_ms();
    const uint32_t base = hash_key(addr, type) & (FH_BLE_SEEN_SLOTS - 1u);
    seen_t *victim = NULL;
    *log_now = false;

    for (uint32_t i = 0; i < FH_BLE_PROBE_MAX; i++) {
        seen_t *e = &s_seen[(base + i) & (FH_BLE_SEEN_SLOTS - 1u)];
        if (!e->used) {
            victim = e;                              /* free slot: take it    */
            break;
        }
        if (e->type == type && memcmp(e->addr, addr, 6) == 0) {
#if FH_BLE_RELOG_WINDOW_MS != 0
            if ((uint32_t)(t - e->last_ms) >= FH_BLE_RELOG_WINDOW_MS) {
                e->last_ms = t;
                *log_now = true;
            }
#endif
            return e;                                /* seen is seen          */
        }
        if (victim == NULL || (uint32_t)(t - e->last_ms) > (uint32_t)(t - victim->last_ms)) {
            victim = e;                              /* track the stalest     */
        }
    }

    if (victim->used) { s_stats.dropped_keys++; }    /* evicting a live entry */
    memcpy(victim->addr, addr, 6);
    victim->type    = type;
    victim->used    = true;
    victim->last_ms = t;
    victim->meta_have = 0;                           /* no stale meta from an evicted device */
    *log_now = true;
    return victim;
}

/* Medusa feed: send one advertiser as a CAPTURE, src_id = FH_SRC_WS_BLE (5),
 * band nibble 1 (BLE). Hub-local source, so it skips route_frame and goes
 * straight to the bridge (no-op unless a host is attached). extra: [0]=channel
 * (0, unknown), [1]=addr type, [2]=event type; LEN = FH_CAPTURE_FIXED + 3 per
 * the sender invariant. Position stays 0: the hub geotags, not the source. */
static void mirror_entry(const seen_t *e)
{
    fh_capture_t cap;
    memset(&cap, 0, sizeof cap);
    cap.src_id     = FH_SRC_WS_BLE;
    cap.band_chan  = (uint8_t)(FH_BLE_BAND_CODE << 4);
    cap.frame_type = e->ev;
    cap.rssi       = e->rssi;
    for (int i = 0; i < 6; i++) { cap.mac[i] = e->addr[5 - i]; }  /* MSB first */
    cap.count      = 1;
    cap.extra[0]   = 0;
    cap.extra[1]   = e->type;
    cap.extra[2]   = e->ev;
    cap.extra[3]   = (uint8_t)(e->company & 0xFF);
    cap.extra[4]   = (uint8_t)(e->company >> 8);
    cap.extra[5]   = e->adflags;
    cap.extra[6]   = (uint8_t)(e->svc16 & 0xFF);
    cap.extra[7]   = (uint8_t)(e->svc16 >> 8);
    cap.extra[8]   = (uint8_t)e->txp;
    cap.extra[9]   = e->meta_have;
    fh_bridge_mirror_frame(FH_TYPE_CAPTURE, (const uint8_t *)&cap,
                           (uint8_t)(FH_CAPTURE_FIXED + FH_BLE_EXTRA_LEN));
}

/* Pull the metadata Medusa shows out of one advertising report. Runs outside
 * the seen-table lock; the caller merges the result into the entry. */
typedef struct {
    uint8_t have; uint16_t company; uint8_t adflags; uint16_t svc16; int8_t txp;
} ble_meta_t;

static ble_meta_t parse_meta(const struct ble_gap_disc_desc *d)
{
    ble_meta_t m = {0};
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) { return m; }
    if (f.mfg_data != NULL && f.mfg_data_len >= 2) {
        m.company = (uint16_t)(f.mfg_data[0] | (f.mfg_data[1] << 8));
        m.have |= FH_BLE_HAVE_COMPANY;
    }
    if (f.flags != 0) { m.adflags = f.flags; m.have |= FH_BLE_HAVE_FLAGS; }
    if (f.num_uuids16 > 0 && f.uuids16 != NULL) {
        m.svc16 = f.uuids16[0].value;
        m.have |= FH_BLE_HAVE_SVC16;
    } else if (f.svc_data_uuid16 != NULL && f.svc_data_uuid16_len >= 2) {
        m.svc16 = (uint16_t)(f.svc_data_uuid16[0] | (f.svc_data_uuid16[1] << 8));
        m.have |= FH_BLE_HAVE_SVC16;
    }
    if (f.tx_pwr_lvl_is_present) { m.txp = f.tx_pwr_lvl; m.have |= FH_BLE_HAVE_TXP; }
    if (f.name != NULL && f.name_len > 0) { m.have |= FH_BLE_HAVE_NAME; }
    return m;
}

static void rescan(void);   /* defined with the GAP code below */

/* Sends the whole seen table ONCE each time a host attaches (monitor ->
 * interactive edge). After that, only new devices go out (live, from
 * on_gap_event). Re-attach (Medusa restart, hub reboot) replays it again. */
static void feed_task(void *arg)
{
    (void)arg;
    bool was_attached = false;
    uint32_t since_rescan_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FH_BLE_FEED_POLL_MS));
        since_rescan_ms += FH_BLE_FEED_POLL_MS;
        if (since_rescan_ms >= FH_BLE_RESCAN_MS) {
            since_rescan_ms = 0;
            rescan();
        }
        bool now_attached = fh_bridge_is_interactive();
        bool just_attached = now_attached && !was_attached;
        was_attached = now_attached;
        if (!just_attached) { continue; }
        for (uint32_t i = 0; i < FH_BLE_SEEN_SLOTS; i++) {
            seen_t copy;
            taskENTER_CRITICAL(&s_seen_mux);
            copy = s_seen[i];
            taskEXIT_CRITICAL(&s_seen_mux);
            if (copy.used) { mirror_entry(&copy); }
        }
    }
}

/* ---- row emit ------------------------------------------------------------ */
static void emit_row(const struct ble_gap_disc_desc *d)
{
    fh_log_record_t r;
    memset(&r, 0, sizeof r);

    r.source_str = FH_SRCSTR_WS_BLE;
    r.band       = FH_BLE_BAND_CODE;
    r.channel    = 0;                     /* adv channel not reported by host  */
    r.frame_type = d->event_type;         /* ADV_IND / SCAN_RSP / ...          */
    for (int i = 0; i < 6; i++) { r.mac[i] = d->addr.val[5 - i]; }  /* MSB first */
    r.has_mac    = true;
    r.rssi       = d->rssi;
    r.has_rssi   = true;
    r.count      = 1;

    /* extra: address type, event type, and (if advertised) a sanitized name.
     * CSV-safe: only [A-Za-z0-9 _.-] survive from the name. */
    char name[24] = {0};
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 &&
        f.name != NULL && f.name_len > 0) {
        size_t n = f.name_len < (sizeof name - 1) ? f.name_len : (sizeof name - 1);
        for (size_t i = 0; i < n; i++) {
            char c = (char)f.name[i];
            bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
                      c == '.' || c == '-';
            name[i] = ok ? c : '_';
        }
    }
    if (name[0]) {
        snprintf(r.extra, sizeof r.extra, "ty=%u ev=%u n=%s",
                 (unsigned)d->addr.type, (unsigned)d->event_type, name);
    } else {
        snprintf(r.extra, sizeof r.extra, "ty=%u ev=%u",
                 (unsigned)d->addr.type, (unsigned)d->event_type);
    }

    if (fh_log_submit(&r)) { s_stats.rows_emitted++; }
}

/* ---- GAP discovery ------------------------------------------------------- */
static int on_gap_event(struct ble_gap_event *ev, void *arg);

static void start_scan(void)
{
    struct ble_gap_disc_params p;
    memset(&p, 0, sizeof p);
    p.passive           = s_passive ? 1 : 0;
    p.filter_duplicates = 0;     /* we dedupe ourselves (needed for re-log)    */
    p.filter_policy     = BLE_HCI_SCAN_FILT_NO_WL;
    /* itvl/window 0 => controller defaults. */

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, on_gap_event, NULL);
    if (rc == BLE_HS_EALREADY) { s_stats.running = true; return; }  /* raced a restart */
    if (rc != 0) {
        s_stats.running = false;
        ESP_LOGE(TAG, "ble_gap_disc failed rc=%d", rc);
        return;
    }
    static bool s_logged = false;          /* rescans every 10 s: log once */
    if (!s_stats.running && !s_logged) {
        ESP_LOGI(TAG, "BLE scan started (%s)", s_passive ? "passive" : "active");
        s_logged = true;
    }
    s_stats.running = true;
}

/* BENCH FIX (2026-10-30): the C6's old controller firmware reports each
 * advertiser only ~once per scan (filter_duplicates=0 is not honored), so
 * Medusa saw every device once and then nothing: no live RSSI, no "seen"
 * growth. Restarting the scan clears the controller's duplicate list, so each
 * device is reported again. VERIFY on the bench: "seen" in Medusa should climb
 * by about 1 per device every cycle. */
static void rescan(void)
{
    if (!s_stats.running) { return; }      /* not synced yet / scan failed */
    if (ble_gap_disc_active()) { ble_gap_disc_cancel(); }
    start_scan();
}

static int on_gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        s_stats.adv_seen++;
        {
            bool log_now = false;
            seen_t snap;
            const ble_meta_t m = parse_meta(&ev->disc);
            taskENTER_CRITICAL(&s_seen_mux);
            seen_t *e = seen_check_and_mark(ev->disc.addr.val, ev->disc.addr.type, &log_now);
            e->rssi = ev->disc.rssi;
            e->ev   = ev->disc.event_type;
            if (m.have & FH_BLE_HAVE_COMPANY) { e->company = m.company; }
            if (m.have & FH_BLE_HAVE_FLAGS)   { e->adflags = m.adflags; }
            if (m.have & FH_BLE_HAVE_SVC16)   { e->svc16 = m.svc16; }
            if (m.have & FH_BLE_HAVE_TXP)     { e->txp = m.txp; }
            e->meta_have |= m.have;
            snap = *e;
            taskEXIT_CRITICAL(&s_seen_mux);
            if (log_now) { emit_row(&ev->disc); }   /* SD: once per device */
            /* Medusa: every report. The C6 controller reports each device
             * about once per scan, and feed_task restarts the scan every
             * FH_BLE_RESCAN_MS, so this is ~1 frame per device per cycle. */
            mirror_entry(&snap);
        }
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGW(TAG, "discovery ended (reason=%d) - restarting", ev->disc_complete.reason);
        start_scan();
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) { rc = ble_hs_id_infer_auto(0, &s_own_addr_type); }
    if (rc != 0) {
        s_stats.running = false;
        ESP_LOGE(TAG, "no usable BLE address rc=%d - scan not started", rc);
        return;
    }
    start_scan();
}

static void on_reset(int reason)
{
    s_stats.running = false;
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d (will rescan on resync)", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();                 /* returns only on nimble_port_stop()   */
    nimble_port_freertos_deinit();
}

/* ---- bring-up task ------------------------------------------------------- */
static void bringup_task(void *param)
{
    (void)param;

    if (esp_hosted_connect_to_slave() != ESP_OK) {
        ESP_LOGE(TAG, "cannot reach the onboard C6 over SDIO - BLE off");
        vTaskDelete(NULL);
        return;
    }
    /* BENCH FACT (2026-10-30): the onboard C6 runs OLD ESP-Hosted firmware
     * (reports version 0.0.0). It advertises "HCI over SDIO, BLE only", but
     * does not answer the newer Req_FeatureControl RPC these two calls use, so
     * they time out. Old firmware brings its BT controller up by itself at
     * boot, so treat a failure here as a warning and try NimBLE anyway. If
     * NimBLE then never syncs, the C6 firmware needs upgrading. */
    if (esp_hosted_bt_controller_init() != ESP_OK) {
        ESP_LOGW(TAG, "C6 BT controller init RPC failed (old co-proc fw?) - trying NimBLE anyway");
    } else if (esp_hosted_bt_controller_enable() != ESP_OK) {
        ESP_LOGW(TAG, "C6 BT controller enable RPC failed - trying NimBLE anyway");
    }
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s - BLE off", esp_err_to_name(e));
        vTaskDelete(NULL);
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;
    nimble_port_freertos_init(host_task);

    if (xTaskCreate(feed_task, "fh_ble_feed", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Medusa BLE feed task create failed - only new devices will reach Medusa");
    }

    vTaskDelete(NULL);
}

/* ---- public API ---------------------------------------------------------- */
bool fh_ble_start(void)
{
    const fh_hub_cfg_t *cfg = fh_cfg_get();
    if (cfg && !cfg->band_ble_en) {
        ESP_LOGI(TAG, "BLE disabled in config - no-op (ok)");
        return true;
    }
    s_passive = !(cfg && cfg->ble_scan_mode == FH_BLE_ACTIVE);

    if (xTaskCreate(bringup_task, "fh_ble_up", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not create BLE bring-up task");
        return false;
    }
    return true;
}

fh_ble_stats_t fh_ble_stats(void) { return s_stats; }
