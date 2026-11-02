/*
 * fh_hub_board.c — FHAdvanced-REMIX hub board/platform layer (P4) impl.
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule). See KNOWN RISKS end.
 *
 * P4-WIFI6-DB specifics:
 *   - TF slot is a DEDICATED SDMMC interface on GPIO39..44 (4-bit). SDMMC_HOST
 *     slot 0 on the P4 maps to these pins by default; we set 4-bit width.
 *   - NO panel/touch/PMU/expander (the Waveshare-S3 HEAD owns those).
 *   - Hub-local I2C (GPIO7/8) is OPTIONAL and only brought up if present; the
 *     carried time base degrades gracefully with no RTC.
 */
#include "fh_hub_board.h"
#include "fh_hub_pins.h"
#include <sys/statvfs.h>   /* fh_board_sd_space (SYS_STATE report) */

#include <string.h>
#include "esp_log.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"   /* P4 slot-0 IO power (LDO_VO4) */
#include "driver/gpio.h"                  /* GPIO45 SD_VDD_EN */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"                /* vTaskDelay */
#include "driver/i2c_master.h"

static const char *TAG = "fh_board";

static sdmmc_card_t          *s_card    = NULL;
static bool                   s_sd_ok   = false;
static bool                   s_rtc_ok  = false;
static i2c_master_bus_handle_t s_i2c    = NULL;

/* ---- optional hub-local I2C (GPIO7/8) ----------------------------------- */
static void i2c_try_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port   = I2C_NUM_0,
        .sda_io_num = 7,     /* GPIO7 — board shared I2C SDA (free on P4 hub)  */
        .scl_io_num = 8,     /* GPIO8 — board shared I2C SCL                   */
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &s_i2c) == ESP_OK) {
        ESP_LOGI(TAG, "hub-local I2C up on GPIO7/8 (optional devices)");
    } else {
        s_i2c = NULL;   /* no I2C is fine — no mandatory device lives on it   */
    }
    /* RTC presence probe is PARKED: no hub-local RTC is assumed on the P4 hub.
     * A builder who wires one sets s_rtc_ok after a successful probe here. */
}

/* ---- microSD (SDMMC 4-bit on the P4-WIFI6-DB TF slot) -------------------- */
bool fh_board_sd_mount(void)
{
    if (s_sd_ok) { return true; }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* P4 TF slot is 4-bit (D0..D3). KEEP SDMMC_HOST_FLAG_DEINIT_ARG.
     * BENCH FIX (2026-10-30): plain `= SDMMC_HOST_FLAG_4BIT` dropped it, so a
     * failed mount (e.g. no card) tore down the WHOLE SDMMC controller instead
     * of just slot 0. ESP-Hosted shares that controller (slot 1, the C6 link),
     * and its later connect crashed in sdmmc_host_get_slot_width. With the
     * flag, cleanup removes slot 0 only. */
    host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    /* BENCH FIX (2026-10-30): on the P4, SDMMC_HOST_DEFAULT() picks SLOT 1.
     * ESP-Hosted already owns slot 1 (SDIO to the onboard C6), so the mount
     * failed with "slot is not available". The TF card is on SLOT 0 (the
     * dedicated IOMUX pins GPIO39..44). */
    host.slot = SDMMC_HOST_SLOT_0;

    /* Slot-0 IO is powered by the P4's on-chip LDO channel 4 (LDO_VO4) on
     * Espressif-reference P4 boards; the IDF sdmmc example does the same.
     * VERIFY against the P4-WIFI6-DB schematic. If the board feeds the TF slot
     * from an external regulator instead, this still succeeds and is harmless
     * unless LDO_VO4 drives something else. */
    /* Card power switch: GPIO45 = SD_VDD_EN, active LOW, per the board GPIO
     * table (fh_hub_pins.h / WIRING.md). Nothing drove it before, so the card
     * may have been unpowered. NOT yet bench-proven (no card inserted
     * 2026-10-30): with a card, expect "microSD mounted". */
    static bool s_sd_vdd_on = false;
    if (!s_sd_vdd_on) {
        gpio_config_t io = { .pin_bit_mask = 1ULL << 45, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io);
        gpio_set_level(45, 0);
        vTaskDelay(pdMS_TO_TICKS(10));                /* let the rail settle */
        s_sd_vdd_on = true;
    }

    static sd_pwr_ctrl_handle_t s_sd_pwr = NULL;
    if (s_sd_pwr == NULL) {
        sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = 4 };
        esp_err_t pe = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_sd_pwr);
        if (pe != ESP_OK) {
            ESP_LOGW(TAG, "SD LDO4 power ctrl failed (%s) - trying without it",
                     esp_err_to_name(pe));
            s_sd_pwr = NULL;
        }
    }
    host.pwr_ctrl_handle = s_sd_pwr;

    /* The P4 SDMMC slot pins (GPIO39..44) are the board's dedicated TF wiring;
     * on IDF 6.1 the default slot config targets them. We leave the slot's pin
     * fields at their defaults rather than hard-coding, because the P4 SDMMC
     * host routes these through its own IOMUX (VERIFY at bring-up — D5). */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t e = esp_vfs_fat_sdmmc_mount(FH_SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
    if (e == ESP_OK) {
        s_sd_ok = true;
        ESP_LOGI(TAG, "microSD mounted at %s (4-bit)", FH_SD_MOUNT_POINT);
    } else {
        s_sd_ok = false;
        ESP_LOGW(TAG, "microSD mount failed (%s) — logger will stage to RAM",
                 esp_err_to_name(e));
    }
    return s_sd_ok;
}

bool fh_board_sd_ok(void)      { return s_sd_ok; }
bool fh_board_sd_mounted(void) { return s_sd_ok; }

/* Free/used space on the SD card in MiB, via the FATFS VFS (statvfs). The
 * SYS_STATE report needs this for the "SD card nearly full" bench check.
 * Returns false (and writes 0xFFFFFFFF = unknown) if not mounted or statvfs
 * fails. BENCH-NOTE: statvfs through the IDF VFS is supported by the
 * esp_vfs_fat layer; confirm the f_bsize/f_bavail semantics on IDF 6.1. */
bool fh_board_sd_space(uint32_t *free_mb, uint32_t *used_mb)
{
    if (free_mb)  { *free_mb = 0xFFFFFFFFu; }
    if (used_mb)  { *used_mb = 0xFFFFFFFFu; }
    if (!s_sd_ok || !free_mb || !used_mb) { return false; }
    struct statvfs vfs;
    if (statvfs(FH_SD_MOUNT_POINT, &vfs) != 0) { return false; }
    unsigned long long bsize = (unsigned long long)vfs.f_bsize
                             ? (unsigned long long)vfs.f_bsize : 512u;
    unsigned long long free_b  = (unsigned long long)vfs.f_bavail * bsize;
    unsigned long long total_b = (unsigned long long)vfs.f_blocks * bsize;
    unsigned long long used_b  = (total_b > free_b) ? (total_b - free_b) : 0u;
    *free_mb = (uint32_t)(free_b >> 20);
    *used_mb = (uint32_t)(used_b >> 20);
    return true;
}

/* NC2 (carried): on a detected write failure the logger calls this so the next
 * mount genuinely re-runs instead of short-circuiting on a cached flag. */
void fh_board_sd_mark_lost(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(FH_SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
    s_sd_ok = false;
}

/* ---- RTC (optional; graceful absence) ----------------------------------- */
bool fh_board_rtc_ok(void) { return s_rtc_ok; }

bool fh_board_rtc_get(uint32_t *epoch_utc)
{
    if (!s_rtc_ok || !s_i2c || !epoch_utc) { return false; }
    /* PARKED: real PCF85063 read over s_i2c when a builder wires one. Until
     * then no hub-local RTC exists and the time base uses the GPS blade. */
    return false;
}
bool fh_board_rtc_set(uint32_t epoch_utc)
{
    (void)epoch_utc;
    if (!s_rtc_ok || !s_i2c) { return false; }
    return false;   /* PARKED (see rtc_get) */
}

bool fh_board_pmu_ok(void) { return false; }   /* no PMU on the P4 hub */

i2c_master_bus_handle_t fh_board_i2c_bus(void) { return s_i2c; }

/* ---- bring-up ----------------------------------------------------------- */
bool fh_board_init(void)
{
    i2c_try_init();                 /* optional; no mandatory device on it      */
    s_sd_ok = fh_board_sd_mount();  /* the one real device; non-fatal on fail   */
    ESP_LOGI(TAG, "board init: sd=%s i2c=%s (no panel — head owns it)",
             s_sd_ok ? "ok" : "DOWN", s_i2c ? "up" : "none");
    return true;   /* SD-down is recoverable (logger stages); never block boot  */
}

/* ===========================================================================
 * KNOWN RISKS
 *   [D1] SDMMC SLOT PINS. The P4-WIFI6-DB TF slot is SDMMC on GPIO39..44 (4-bit)
 *        per the board GPIO table. We use SDMMC_SLOT_CONFIG_DEFAULT() + width=4
 *        rather than hard-coding pins, trusting the P4 SDMMC host's default slot
 *        to target the board wiring. VERIFY at bring-up that the default slot is
 *        the TF slot; if not, set slot.clk/cmd/d0..d3 to 43/44/39/40/41/42.
 *   [D2] 4-BIT vs 1-BIT. The board wires 4 data lines (SD_D0..D3). 4-bit is the
 *        intent; if a bring-up issue appears, fall back to SDMMC_HOST_FLAG_1BIT
 *        + width=1 to isolate wiring from width problems.
 *   [D3] NO RTC ASSUMED. The P4 hub has no onboard RTC; time comes from the GPS
 *        blade (0x02) or the boot clock. fh_board_rtc_* are graceful-false
 *        shims until a builder wires an external RTC on GPIO7/8 and fills them.
 *   [D4] esp_vfs_fat_sdcard_unmount symbol: confirm on IDF 6.1 (vs the older
 *        esp_vfs_fat_sdmmc_unmount(void)). Carried from gen1 NC2.
 *   [D5] This is the board layer's ONLY mandatory job now — get the card up.
 *        Everything panel-related left with the head. Keeping this file small
 *        is the point: less hardware surface, less to be wrong.
 * ======================================================================== */
