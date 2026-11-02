/*
 * fh_hub_config.h — FHAdvanced hub config subsystem (SUBSYSTEM 6)
 *
 * ############################################################################
 * #  UNVERIFIED FIRST DRAFT — written WITHOUT a compiler (project rule).      #
 * #  Design-faithful to CONFIG_MENU.md + ARCHITECTURE.md #9, written against  #
 * #  the frozen fh_protocol headers. EXPECT a compile-fix pass on the first   #
 * #  real ESP-IDF 6.1 build. See KNOWN COMPILE RISKS in fh_hub_config.c.      #
 * ############################################################################
 *
 * Config model (ARCHITECTURE #9, authoritative):
 *   compiled-in DEFAULTS  +  an NVS OVERRIDE layer (NVS wins).  NEVER on SD.
 *   6 settings + 4 maintenance actions (CONFIG_MENU.md).
 *   STOCK / MODIFIED(N) tracked in NVS and written into the SD log header.
 *   UART baud is a LOCKED constant, NOT a setting (brick risk) — lives in
 *   fh_hub_pins.h, never here.
 *
 * Public surface: load once at boot, read tunables anywhere, set an override
 * (bumps MODIFIED), reset-to-defaults (back to STOCK). The logger reads
 * fh_cfg_header_provenance() to stamp each file's header.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the 6 settings (CONFIG_MENU.md, authoritative) ----------------------
 * Kept deliberately few — "fewer knobs" is a standing design preference. Each
 * has a compiled-in default; NVS can override; NVS wins. Values that CONFIG_MENU
 * fixed as constants (roll size 128 MB, dwell default, etc.) are NOT settings. */

/* #1 GPS fix rate. NEO-6M tops out at 5 Hz; only 1 or 5 offered (no 10 Hz). */
typedef enum { FH_GPS_1HZ = 0, FH_GPS_5HZ = 1 } fh_gps_rate_t;

/* #4 re-log policy: identity "seen is seen"; one row per WINDOW, or once/session.
 * This is the radio-button choice from CONFIG_MENU (timed default vs log-once). */
typedef enum { FH_RELOG_TIMED = 0, FH_RELOG_ONCE_PER_SESSION = 1 } fh_relog_mode_t;

/* #3 BLE scan mode: passive by default; active is opt-in (liability on the
 * operator — only probes devices that already advertise). */
typedef enum { FH_BLE_PASSIVE = 0, FH_BLE_ACTIVE = 1 } fh_ble_scan_mode_t;

typedef struct {
    /* #1 */ fh_gps_rate_t      gps_rate;       /* default 1 Hz (safe cold-start) */
    /* #2 */ uint8_t            brightness_pct; /* 0..100 UI scale -> 85% HW ceil; default 50 */
    /* #3 */ fh_ble_scan_mode_t ble_scan_mode;  /* default PASSIVE                 */
    /* #4 */ fh_relog_mode_t    relog_mode;     /* default TIMED                   */
    /* #4b*/ uint16_t           relog_window_s; /* default 60 s; used when TIMED   */
    /* #5 */ bool               band_24_en;     /* 2.4 GHz capture enable; default on */
    /* #5 */ bool               band_5_en;      /* 5 GHz capture enable;   default on */
    /* #5 */ bool               band_ble_en;    /* BLE capture enable;     default on */
    /* #6 */ uint16_t           dwell_ms;       /* Wi-Fi per-channel dwell; default 110 */
} fh_hub_cfg_t;

/* ---- compiled-in defaults (the STOCK baseline) --------------------------- */
#define FH_CFG_DEF_GPS_RATE       FH_GPS_5HZ   /* 5 Hz bench test (2026-11-01) */
#define FH_CFG_DEF_BRIGHTNESS     50u          /* % on the 0..100 UI scale        */
#define FH_CFG_DEF_BLE_SCAN       FH_BLE_PASSIVE
#define FH_CFG_DEF_RELOG_MODE     FH_RELOG_TIMED
#define FH_CFG_DEF_RELOG_WINDOW_S 60u
#define FH_CFG_DEF_BAND_24        true
#define FH_CFG_DEF_BAND_5         true
#define FH_CFG_DEF_BAND_BLE       true
#define FH_CFG_DEF_DWELL_MS       110u         /* ~tied to 102 ms beacon interval */

/* Backlight: UI exposes 0..100%, mapped onto an 85% HARDWARE ceiling to prevent
 * burn-in (from the FootHoled board_adapter lineage). The map lives in the
 * board layer (fh_hub_board) which owns the backlight; config only stores the
 * UI-scale percent. 100% UI => 85% duty; 0% UI => 0% duty (linear). */
#define FH_BL_CEIL_PERCENT        85u

/* ---- STOCK / MODIFIED record --------------------------------------------- */
typedef struct {
    bool     modified;        /* false => STOCK, true => MODIFIED(override_count) */
    uint16_t override_count;  /* how many settings differ from the baseline      */
} fh_cfg_status_t;

/* ---- public API ---------------------------------------------------------- */

/* Load defaults, then apply any NVS overrides (NVS wins). Safe to call once at
 * boot. Returns false only on an NVS subsystem failure (falls back to pure
 * defaults and still returns a usable config in *out). */
bool fh_cfg_load(fh_hub_cfg_t *out);

/* Current in-RAM config snapshot (what fh_cfg_load produced, plus any runtime
 * fh_cfg_set_* calls). Pointer is stable for the process lifetime. */
const fh_hub_cfg_t *fh_cfg_get(void);

/* Status derived from comparing the live config to the compiled-in defaults.
 * Written into the SD log header by the logger. */
fh_cfg_status_t fh_cfg_status(void);

/* Maintenance action #1: reset every setting to the compiled-in default AND
 * erase the NVS override namespace. Returns to STOCK. Caller confirms first
 * (UI responsibility). Returns true on success. */
bool fh_cfg_reset_defaults(void);

/* Persist one changed setting to the NVS override layer and update the live
 * snapshot. Thin typed setters keep the NVS key names in one place. Each bumps
 * the MODIFIED delta if the new value differs from the compiled-in default (or
 * clears that key's override when the value returns to default). Returns true
 * on a successful NVS commit. UI calls these; nothing else writes config. */
bool fh_cfg_set_gps_rate(fh_gps_rate_t v);
bool fh_cfg_set_brightness(uint8_t pct);           /* clamped to 0..100 */
bool fh_cfg_set_ble_scan(fh_ble_scan_mode_t v);
bool fh_cfg_set_relog(fh_relog_mode_t mode, uint16_t window_s);
bool fh_cfg_set_bands(bool b24, bool b5, bool bble);
bool fh_cfg_set_dwell(uint16_t ms);

/* Render the provenance lines for the SD log header into `buf` (see
 * BUILD_PLAN Appendix B format). Writes up to `cap` bytes, always
 * NUL-terminated. Returns the number of bytes that WOULD be written (may be
 * >= cap if truncated, like snprintf). Two lines (the config= token on line 1
 * is owned by the logger; this fills the overrides line body it owns):
 *   config=STOCK            (or config=MODIFIED(N))\n
 *   # config_overrides=(none)   (or k=v,k=v of the NVS delta)
 * See logger for full-line assembly. */
size_t fh_cfg_header_provenance(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
