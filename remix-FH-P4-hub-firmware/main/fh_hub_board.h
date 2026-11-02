/*
 * fh_hub_board.h — FHAdvanced-REMIX hub board/platform layer (P4)
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule).
 *
 * ON THE ESP32-P4-WIFI6-DB the hub's board surface is MUCH smaller than the
 * gen1 Waveshare-S3 hub, because the P4 hub has NO panel: the Waveshare-S3
 * HEAD owns the LCD + touch now. So this layer is:
 *   - microSD via the P4's DEDICATED SDMMC TF slot (GPIO39..44, 4-bit) — the
 *     single log sink; the ONLY mandatory device;
 *   - an OPTIONAL hub-local I2C master on GPIO7/8 (free per the pin budget) for
 *     a future RTC / fuel-gauge — degrades cleanly if absent.
 * NO LCD, NO touch, NO TCA9554 expander, NO AXP2101 PMU, NO backlight, NO
 * ST7796 — all of that lived on the S3 hub and now lives on the S3 HEAD.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FH_SD_MOUNT_POINT   "/sdcard"

/* Optional hub-local I2C addresses (only if a device is wired to GPIO7/8).
 * None are mandatory on the P4 hub; the gen1 PMU/touch/expander are gone. */
#define FH_I2C_ADDR_RTC_EXT   0x51   /* e.g. a PCF85063 if the builder adds one */

/* Bring the board up: mount the SDMMC card (mandatory for logging) and, if
 * present, init the hub-local I2C bus. Returns false only if a MANDATORY device
 * failed — but SD failure is treated as NON-fatal here too (the logger stages
 * to RAM and retries), so this returns true in normal operation and sets the
 * fh_board_*_ok() flags for the status model. */
bool fh_board_init(void);

/* The optional hub-local I2C bus handle (NULL if none was brought up). */
i2c_master_bus_handle_t fh_board_i2c_bus(void);

/* Per-device status for the status model. On the P4: sd + (optional) rtc only.
 * The LCD/touch/pmu accessors are RETAINED as always-false shims so the carried
 * status model (fh_hub_ui/overall_status) compiles unchanged — there is simply
 * no panel on this board. */
bool fh_board_sd_ok(void);
bool fh_board_rtc_ok(void);
static inline bool fh_board_lcd_ok(void)   { return false; } /* no panel on P4 */
static inline bool fh_board_touch_ok(void) { return false; } /* no touch on P4 */
bool fh_board_pmu_ok(void);                                   /* false on P4    */

/* SD (re)mount + state, same contract as gen1 (the logger drives recovery). */
bool fh_board_sd_mount(void);
bool fh_board_sd_mounted(void);
/* Free/used SD space in MiB (0xFFFFFFFF = unknown). Returns false if not
 * mounted or the statvfs query failed. Feeds the SYS_STATE report. */
bool fh_board_sd_space(uint32_t *free_mb, uint32_t *used_mb);
void fh_board_sd_mark_lost(void);

/* RTC raw access (UTC epoch). Returns false if no hub-local RTC is present —
 * the time base then falls back to the GPS blade / boot clock. */
bool fh_board_rtc_get(uint32_t *epoch_utc);
bool fh_board_rtc_set(uint32_t epoch_utc);

#ifdef __cplusplus
}
#endif
