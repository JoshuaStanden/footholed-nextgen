/*
 * fh_hub_ui.c — FHAdvanced-REMIX hub UI/status layer impl. (P4 / headless)
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule). See KNOWN RISKS end.
 *
 * ON THE P4 THE HUB HAS NO PANEL. The Waveshare-S3 HEAD owns the LCD + touch
 * now; the P4 hub EMITS display data to the head (a Bus-A peer) and the head
 * renders it. So this layer is PURELY the status MODEL + a headless USB status
 * line. The same model that prints here is what the hub will serialize to the
 * head as a render feed (a later pass wires fh_link_send_peer(FH_PEER_HEAD,...)).
 * No board LCD/touch/backlight calls exist here anymore — those left with the
 * panel.
 */
#include "fh_hub_ui.h"
#include "fh_hub_board.h"
#include "fh_hub_health.h"
#include "fh_hub_logger.h"
#include "fh_hub_ble.h"
#include "fh_hub_time.h"
#include "fh_hub_config.h"
#include "fh_frame.h"

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "fh_ui";

#define UI_TICK_MS         1000u

/* Activity hook kept in the public API for callers (e.g. a future head touch
 * event relayed up Bus A). On the headless P4 it just timestamps; there is no
 * local screen to sleep/wake. */
static volatile uint64_t s_last_activity_us = 0;
void fh_ui_note_activity(void) { s_last_activity_us = esp_timer_get_time(); }

/* Overall status color: red if any node DOWN or SD down; amber if any node
 * QUIET_BUT_ALIVE; else green. "Silent when healthy." (Carried from gen1.) */
static const char *overall_status(void)
{
    if (fh_health_down_count() > 0) { return "RED"; }
    if (!fh_board_sd_ok())          { return "RED"; }
    fh_node_health_t c5  = fh_health_node(FH_SRC_C5_WIFI5);
    fh_node_health_t xs3 = fh_health_node(FH_SRC_XS3_WIFI24);
    fh_node_health_t pic = fh_health_node(FH_SRC_PICO);
    if (c5.state == FH_NODE_QUIET_BUT_ALIVE ||
        xs3.state == FH_NODE_QUIET_BUT_ALIVE ||
        pic.state == FH_NODE_QUIET_BUT_ALIVE) { return "AMBER"; }
    return "GREEN";
}

static void ui_task(void *arg)
{
    (void)arg;
    s_last_activity_us = esp_timer_get_time();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(UI_TICK_MS));

        /* headless status line. The SAME model will drive the head's render
         * feed (a later pass serializes this over Bus A to FH_PEER_HEAD). */
        fh_log_stats_t  ls = fh_log_stats();
        fh_ble_stats_t  bs = fh_ble_stats();
        fh_node_health_t c5  = fh_health_node(FH_SRC_C5_WIFI5);
        fh_node_health_t xs3 = fh_health_node(FH_SRC_XS3_WIFI24);
        fh_node_health_t pic = fh_health_node(FH_SRC_PICO);
        bool from_rtc = false;
        uint32_t now = fh_time_now_utc(&from_rtc);
        char iso[24]; fh_time_iso8601(now, iso, sizeof(iso));

        /* When a TUI (Medusa) is attached, the bridge's esp_log_set_vprintf hook
         * suppresses ALL log output centrally, so this prints in monitor mode
         * and is swallowed in interactive mode — one guard, one place. */
        ESP_LOGI(TAG,
            "[%s] %s | staged=%" PRIu32 " onSD=%" PRIu64 "B dropQ=%" PRIu32
            " dropSD=%" PRIu32 " files=%" PRIu32 " | C5=%s XS3=%s PICO=%s"
            " | BLE rows=%" PRIu32 " | SD=%s RTC=%s",
            overall_status(), iso,
            ls.rows_staged, ls.bytes_committed, ls.dropped_queue,
            ls.dropped_sd_down, ls.files_rolled,
            c5.up ? "UP" : "dn", xs3.up ? "UP" : "dn", pic.up ? "UP" : "dn",
            bs.rows_emitted,
            fh_board_sd_ok() ? "ok" : "DOWN",
            from_rtc ? "ok" : "boot");
    }
}

bool fh_ui_start(void)
{
    BaseType_t ok = xTaskCreate(ui_task, "fh_ui", 4096, NULL, 3, NULL);
    if (ok != pdPASS) { return false; }
    ESP_LOGI(TAG, "UI/status task started (headless; display is the head's job)");
    return true;
}

/* ===========================================================================
 * KNOWN RISKS
 *   [U1] NO LOCAL DISPLAY ON THE P4 — by design. The head (Waveshare-S3) owns
 *        the panel + touch. This layer is the status MODEL only; a later pass
 *        serializes it to the head over Bus A as a render feed. Headless USB
 *        status line is the correct, intended state for pass 1.
 *   [U2] overall_status treats "node never seen (UNKNOWN)" as NOT red — correct
 *        for first boot before peers/blades report. Once seen, HUNG/DEAD -> red.
 *   [U3] fh_ui_note_activity is retained in the API for a future head touch
 *        event relayed upstream; on the headless hub it only timestamps.
 * ======================================================================== */
