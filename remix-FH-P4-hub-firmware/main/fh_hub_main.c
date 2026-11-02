/*
 * fh_hub_main.c — FHAdvanced-REMIX hub (Waveshare ESP32-P4-WIFI6-DB)
 *                 composition root / app_main.
 *
 * ############################################################################
 * #  UNVERIFIED FIRST DRAFT — written WITHOUT a compiler (project rule).      #
 * #  PASS 1 = FRAMEWORK-FIRST: bring up the two SPI buses, prove fh_protocol  #
 * #  frames move, write the unified log to SD, keep Medusa working. NO Wi-Fi  #
 * #  capture interpretation, NO blade radio logic — those plug in later.      #
 * #  Built against the FROZEN fh_protocol. Expect a compile-fix pass on the   #
 * #  first real ESP-IDF 6.1 build. Each module lists KNOWN RISKS at EOF.      #
 * ############################################################################
 *
 * THE HUB'S THREE JOBS (owner decision 2026-10-25):
 *   1. COORDINATE the peripherals over two SPI buses (fh_hub_link):
 *        Bus A quad-SPI, 3 ESP32 peers, IRQ-driven (head + XIAO-S3 + XIAO-C5).
 *        Bus B conventional SPI, up to 6 Pico+radio blades, fired one-at-a-time;
 *        the GPS blade is POLLED on a schedule (cfg.gps_rate).
 *   2. WRITE the unified CSV log to microSD (fh_hub_logger — single writer).
 *   3. EMIT display data to the Waveshare-S3 HEAD (a Bus-A peer that renders).
 *   Plus: hub-local BLE sniff on the P4's onboard C6 (fh_hub_ble — live since 2026-10-30).
 *
 * BOOT ORDER (dependency-honoring, carried from gen1 HUB_PLAN):
 *   0. NVS flash + TZ=UTC0 (every epoch is UTC)
 *   1. board bring-up (P4: SDMMC card + optional hub-local I2C; NO LCD/touch —
 *      the head owns the panel now)
 *   2. config load (defaults + NVS overrides)
 *   3. logger start (SINGLE SD writer; stages to RAM if SD is down)
 *   4. time base (RTC-if-present + the GPS blade's 0x02 fix consumer)
 *   5. SPI LINK (both buses; peers' IRQ drain + GPS poll + health monitor)
 *   5b. on-demand requester (fires blade REQs, polls to completion, logs results)
 *   6. BLE sniffer (P4 onboard C6 via ESP-Hosted; also the Medusa src-5 feed)
 *   6b. USB host bridge (Medusa/TUI; MONITOR default, ATTACH -> INTERACTIVE)
 *   7. UI/status (headless status line over USB; display is the head's job)
 *
 * Console: UART0 console (onboard CH343 USB-UART on the USB-C, 921600) (idf.py flash monitor). Also the Medusa
 * bridge transport. Set in sdkconfig.defaults (LF-only for byte-transparency).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "fh_hub_board.h"
#include "fh_hub_config.h"
#include "fh_hub_logger.h"
#include "fh_hub_time.h"
#include "fh_hub_link.h"      /* NEW: SPI link layer (replaces the UART agg)   */
#include "fh_hub_req.h"       /* 5b: on-demand requester (blade REQ tracking)  */
#include "fh_hub_bridge.h"
#include "fh_hub_ble.h"
#include "fh_hub_ui.h"

static const char *TAG = "fh_hub";

/* Node eFuse MAC — the single device identity written into the SD log header. */
bool fh_hub_node_mac(uint8_t out[6])
{
    return esp_read_mac(out, ESP_MAC_EFUSE_FACTORY) == ESP_OK;
}

/* Run the logger against a fabricated feed to bake the SD state machine before
 * real sources exist (carried HUB_PLAN practice). Leave false for integrated. */
#define FH_HUB_FAKE_LOGGER_FEED   false

void app_main(void)
{
    ESP_LOGI(TAG, "FHAdvanced-REMIX HUB boot — ESP32-P4-WIFI6-DB");

    /* 0a. UTC everywhere (GPS epoch math must not skew by local offset). */
    setenv("TZ", "UTC0", 1);
    tzset();

    /* 0b. NVS (config override layer). Recover from a corrupt partition. */
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS (%s) and re-init", esp_err_to_name(nv));
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* 1. Board bring-up. On the P4 the MANDATORY part is the SDMMC card for the
     *    logger; there is NO LCD/touch here (the head owns the panel). A hub-
     *    local I2C bus (GPIO7/8) is OPTIONAL (RTC/fuel-gauge) — degrades. */
    if (!fh_board_init()) {
        ESP_LOGE(TAG, "board init reported a problem — continuing to log the fault");
    }

    /* 2. Config: defaults + NVS overrides (NVS wins). */
    fh_hub_cfg_t cfg;
    fh_cfg_load(&cfg);
    fh_cfg_status_t st = fh_cfg_status();
    ESP_LOGI(TAG, "config: %s", st.modified ? "MODIFIED" : "STOCK");

    /* 3. Logger FIRST in the data path (single SD writer). */
    if (!fh_log_start(FH_HUB_FAKE_LOGGER_FEED)) {
        ESP_LOGE(TAG, "logger start failed — capture cannot be recorded!");
    }

    /* 4. Time base: RTC (if a hub-local RTC exists) + the GPS-blade 0x02
     *    consumer. now_utc()/last_fix() feed the logger's stamps. */
    fh_time_start(cfg.gps_rate);

    /* 5. SPI LINK: Bus A (quad peers, IRQ drain) + Bus B (blades, fired + GPS
     *    poll). Starts the health monitor and its PING hook internally. This is
     *    the heart of the framework — prove frames move here. */
    if (!fh_link_start()) {
        ESP_LOGW(TAG, "SPI link start reported a problem");
    }

    /* 5b. ON-DEMAND REQUESTER (concern #1, 2026-10-31): fires blade REQs, polls
     *     until the matching seq RESP lands, writes the result to SD, and lets
     *     route_frame mirror the RESP to Medusa. AFTER the link (needs the SPI
     *     buses) and BEFORE the bridge (the bridge submits jobs to it). */
    if (!fh_req_start()) {
        ESP_LOGW(TAG, "on-demand requester start failed — blade jobs unavailable");
    }

    /* 6. Hub-local BLE sniffer on the P4 onboard C6 -> ws-ble rows + Medusa src 5.
     *    Bring-up runs in its own task; a dead C6 never blocks boot. */
    if (!fh_ble_start()) {
        ESP_LOGW(TAG, "BLE start reported a problem");
    }

    /* 6b. USB host bridge (Medusa/TUI). MONITOR default; ATTACH -> INTERACTIVE.
     *     After the link so it can relay commands to peers/blades. */
    if (!fh_bridge_start()) {
        ESP_LOGW(TAG, "host bridge start failed — Medusa link unavailable, rest runs");
    }

    /* 7. UI/status: headless status line over USB (the panel is the head's). */
    fh_ui_start();

    ESP_LOGI(TAG, "HUB up. Framework live. Watch the fh_ui status line.");
}
