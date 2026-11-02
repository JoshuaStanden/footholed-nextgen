/*
 * fh_hub_config.c — FHAdvanced hub config subsystem (SUBSYSTEM 6) impl.
 *
 * UNVERIFIED FIRST DRAFT — no compiler was run (project rule). See KNOWN
 * COMPILE RISKS at the bottom.
 *
 * Model: compiled-in defaults -> NVS overrides (NVS wins). One NVS namespace
 * ("fh_cfg"); one key per setting. A key present => that setting is overridden;
 * absent => it tracks the compiled-in default. STOCK/MODIFIED(N) is DERIVED by
 * comparing the live snapshot to the defaults at read time, so it never drifts
 * out of sync with the actual values (no separate "modified" flag to forget to
 * update — ARCHITECTURE "one source of truth").
 */
#include "fh_hub_config.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>   /* va_list/va_start for the provenance printf helper [C1 fixed] */
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "fh_cfg";

#define CFG_NS   "fh_cfg"          /* NVS namespace for the override layer      */

/* NVS keys — short (<=15 chars, NVS limit) and stable. Renaming a key orphans
 * an existing override, so treat these as a wire-ish contract too. */
#define K_GPS_RATE   "gps_rate"
#define K_BRIGHT     "bright"
#define K_BLE_SCAN   "ble_scan"
#define K_RELOG_M    "relog_m"
#define K_RELOG_W    "relog_w"
#define K_BAND24     "b24"
#define K_BAND5      "b5"
#define K_BANDBLE    "bble"
#define K_DWELL      "dwell"

/* Live snapshot. Initialised to defaults by fh_cfg_load(); updated by setters. */
static fh_hub_cfg_t s_cfg;
static bool         s_loaded = false;

static void set_defaults(fh_hub_cfg_t *c)
{
    c->gps_rate       = FH_CFG_DEF_GPS_RATE;
    c->brightness_pct = FH_CFG_DEF_BRIGHTNESS;
    c->ble_scan_mode  = FH_CFG_DEF_BLE_SCAN;
    c->relog_mode     = FH_CFG_DEF_RELOG_MODE;
    c->relog_window_s = FH_CFG_DEF_RELOG_WINDOW_S;
    c->band_24_en     = FH_CFG_DEF_BAND_24;
    c->band_5_en      = FH_CFG_DEF_BAND_5;
    c->band_ble_en    = FH_CFG_DEF_BAND_BLE;
    c->dwell_ms       = FH_CFG_DEF_DWELL_MS;
}

/* Load one u8/u16 key if present; leave *dst untouched (= default) if absent. */
static void load_u8 (nvs_handle_t h, const char *k, uint8_t  *dst)
{
    uint8_t v;
    if (nvs_get_u8(h, k, &v) == ESP_OK) { *dst = v; }
}
static void load_u16(nvs_handle_t h, const char *k, uint16_t *dst)
{
    uint16_t v;
    if (nvs_get_u16(h, k, &v) == ESP_OK) { *dst = v; }
}

bool fh_cfg_load(fh_hub_cfg_t *out)
{
    set_defaults(&s_cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace has never been written: pure STOCK. Not an error. */
        s_loaded = true;
        if (out) { *out = s_cfg; }
        ESP_LOGI(TAG, "no NVS overrides; running STOCK defaults");
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s); using compiled defaults",
                 esp_err_to_name(err));
        s_loaded = true;            /* usable config still returned */
        if (out) { *out = s_cfg; }
        return false;
    }

    uint8_t tmp8;
    /* enums are stored as u8 on the wire-to-flash boundary */
    tmp8 = (uint8_t)s_cfg.gps_rate;      load_u8 (h, K_GPS_RATE, &tmp8); s_cfg.gps_rate      = (fh_gps_rate_t)(tmp8 ? FH_GPS_5HZ : FH_GPS_1HZ);
    load_u8 (h, K_BRIGHT, &s_cfg.brightness_pct);
    tmp8 = (uint8_t)s_cfg.ble_scan_mode; load_u8 (h, K_BLE_SCAN, &tmp8); s_cfg.ble_scan_mode = (fh_ble_scan_mode_t)(tmp8 ? FH_BLE_ACTIVE : FH_BLE_PASSIVE);
    tmp8 = (uint8_t)s_cfg.relog_mode;    load_u8 (h, K_RELOG_M,  &tmp8); s_cfg.relog_mode    = (fh_relog_mode_t)(tmp8 ? FH_RELOG_ONCE_PER_SESSION : FH_RELOG_TIMED);
    load_u16(h, K_RELOG_W, &s_cfg.relog_window_s);
    tmp8 = s_cfg.band_24_en;   load_u8(h, K_BAND24,  &tmp8); s_cfg.band_24_en  = (tmp8 != 0);
    tmp8 = s_cfg.band_5_en;    load_u8(h, K_BAND5,   &tmp8); s_cfg.band_5_en   = (tmp8 != 0);
    tmp8 = s_cfg.band_ble_en;  load_u8(h, K_BANDBLE, &tmp8); s_cfg.band_ble_en = (tmp8 != 0);
    load_u16(h, K_DWELL, &s_cfg.dwell_ms);

    /* clamp loaded values to sane ranges (NVS could hold a stale/garbage write) */
    if (s_cfg.brightness_pct > 100u) { s_cfg.brightness_pct = 100u; }
    if (s_cfg.relog_window_s == 0u)  { s_cfg.relog_window_s = FH_CFG_DEF_RELOG_WINDOW_S; }
    if (s_cfg.dwell_ms == 0u)        { s_cfg.dwell_ms = FH_CFG_DEF_DWELL_MS; }

    nvs_close(h);
    s_loaded = true;
    if (out) { *out = s_cfg; }
    ESP_LOGI(TAG, "config loaded (overrides applied where present)");
    return true;
}

const fh_hub_cfg_t *fh_cfg_get(void)
{
    if (!s_loaded) { set_defaults(&s_cfg); s_loaded = true; }
    return &s_cfg;
}

/* Count how many live settings differ from the compiled-in defaults. This is
 * the DERIVED MODIFIED(N) — no stored flag to desync. */
fh_cfg_status_t fh_cfg_status(void)
{
    if (!s_loaded) { set_defaults(&s_cfg); s_loaded = true; }
    fh_hub_cfg_t d; set_defaults(&d);
    uint16_t n = 0;
    if (s_cfg.gps_rate       != d.gps_rate)       { n++; }
    if (s_cfg.brightness_pct != d.brightness_pct) { n++; }
    if (s_cfg.ble_scan_mode  != d.ble_scan_mode)  { n++; }
    if (s_cfg.relog_mode     != d.relog_mode)     { n++; }
    if (s_cfg.relog_window_s != d.relog_window_s) { n++; }
    if (s_cfg.band_24_en     != d.band_24_en)     { n++; }
    if (s_cfg.band_5_en      != d.band_5_en)      { n++; }
    if (s_cfg.band_ble_en    != d.band_ble_en)    { n++; }
    if (s_cfg.dwell_ms       != d.dwell_ms)       { n++; }
    fh_cfg_status_t st = { .modified = (n > 0), .override_count = n };
    return st;
}

/* ---- setters ------------------------------------------------------------- */
/* Each writes the NVS key when the value differs from default, or ERASES the
 * key when the value equals default (so the override layer only ever holds real
 * deltas — keeps MODIFIED(N) honest and the overrides line short). */
static bool put_u8(const char *k, uint8_t v, uint8_t def)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK) { return false; }
    esp_err_t e = (v == def) ? nvs_erase_key(h, k) : nvs_set_u8(h, k, v);
    /* erase of an absent key returns NOT_FOUND — treat as success */
    if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) { nvs_close(h); return false; }
    e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}
static bool put_u16(const char *k, uint16_t v, uint16_t def)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK) { return false; }
    esp_err_t e = (v == def) ? nvs_erase_key(h, k) : nvs_set_u16(h, k, v);
    if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) { nvs_close(h); return false; }
    e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

bool fh_cfg_set_gps_rate(fh_gps_rate_t v)
{
    if (!put_u8(K_GPS_RATE, (uint8_t)v, (uint8_t)FH_CFG_DEF_GPS_RATE)) { return false; }
    s_cfg.gps_rate = v; return true;
}
bool fh_cfg_set_brightness(uint8_t pct)
{
    if (pct > 100u) { pct = 100u; }
    if (!put_u8(K_BRIGHT, pct, FH_CFG_DEF_BRIGHTNESS)) { return false; }
    s_cfg.brightness_pct = pct; return true;
}
bool fh_cfg_set_ble_scan(fh_ble_scan_mode_t v)
{
    if (!put_u8(K_BLE_SCAN, (uint8_t)v, (uint8_t)FH_CFG_DEF_BLE_SCAN)) { return false; }
    s_cfg.ble_scan_mode = v; return true;
}
bool fh_cfg_set_relog(fh_relog_mode_t mode, uint16_t window_s)
{
    if (window_s == 0u) { window_s = FH_CFG_DEF_RELOG_WINDOW_S; }
    if (!put_u8 (K_RELOG_M, (uint8_t)mode, (uint8_t)FH_CFG_DEF_RELOG_MODE))   { return false; }
    if (!put_u16(K_RELOG_W, window_s, FH_CFG_DEF_RELOG_WINDOW_S))            { return false; }
    s_cfg.relog_mode = mode; s_cfg.relog_window_s = window_s; return true;
}
bool fh_cfg_set_bands(bool b24, bool b5, bool bble)
{
    if (!put_u8(K_BAND24,  b24  ? 1 : 0, FH_CFG_DEF_BAND_24  ? 1 : 0)) { return false; }
    if (!put_u8(K_BAND5,   b5   ? 1 : 0, FH_CFG_DEF_BAND_5   ? 1 : 0)) { return false; }
    if (!put_u8(K_BANDBLE, bble ? 1 : 0, FH_CFG_DEF_BAND_BLE ? 1 : 0)) { return false; }
    s_cfg.band_24_en = b24; s_cfg.band_5_en = b5; s_cfg.band_ble_en = bble; return true;
}
bool fh_cfg_set_dwell(uint16_t ms)
{
    if (ms == 0u) { ms = FH_CFG_DEF_DWELL_MS; }
    if (!put_u16(K_DWELL, ms, FH_CFG_DEF_DWELL_MS)) { return false; }
    s_cfg.dwell_ms = ms; return true;
}

bool fh_cfg_reset_defaults(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) { set_defaults(&s_cfg); return true; }
    if (err != ESP_OK) { return false; }
    err = nvs_erase_all(h);
    if (err == ESP_OK) { err = nvs_commit(h); }
    nvs_close(h);
    set_defaults(&s_cfg);
    return err == ESP_OK;
}

/* ---- provenance line (overrides body) ------------------------------------ */
/* Appends, into buf, a comma-separated list of the overridden keys as k=v, or
 * "(none)" when STOCK. The logger owns the leading "# config_overrides=" and the
 * config=STOCK|MODIFIED(N) token on the first header line; this returns ONLY the
 * delta body so the format lives in one place but the logger controls layout.
 *
 * snprintf-style return: the length that WOULD be written; truncates safely. */
static int app(char *buf, size_t cap, size_t used, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static int app(char *buf, size_t cap, size_t used, const char *fmt, ...)
{
    if (used >= cap) { return 0; }
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf + used, cap - used, fmt, ap);
    va_end(ap);
    return (n < 0) ? 0 : n;
}

size_t fh_cfg_header_provenance(char *buf, size_t cap)
{
    if (!buf || cap == 0) { return 0; }
    buf[0] = '\0';
    fh_hub_cfg_t d; set_defaults(&d);
    const fh_hub_cfg_t *c = fh_cfg_get();
    size_t used = 0;
    bool any = false;

    #define DELTA(cond, ...) do { \
        if (cond) { \
            used += app(buf, cap, used, any ? "," : ""); \
            used += app(buf, cap, used, __VA_ARGS__); \
            any = true; \
        } } while (0)

    DELTA(c->gps_rate       != d.gps_rate,       "gps_rate=%s", c->gps_rate == FH_GPS_5HZ ? "5hz" : "1hz");
    DELTA(c->brightness_pct != d.brightness_pct, "bright=%u", (unsigned)c->brightness_pct);
    DELTA(c->ble_scan_mode  != d.ble_scan_mode,  "ble=%s", c->ble_scan_mode == FH_BLE_ACTIVE ? "active" : "passive");
    DELTA(c->relog_mode     != d.relog_mode,     "relog=%s", c->relog_mode == FH_RELOG_ONCE_PER_SESSION ? "once" : "timed");
    DELTA(c->relog_window_s != d.relog_window_s, "relog_w=%us", (unsigned)c->relog_window_s);
    DELTA(c->band_24_en     != d.band_24_en,     "b24=%u", c->band_24_en ? 1u : 0u);
    DELTA(c->band_5_en      != d.band_5_en,      "b5=%u",  c->band_5_en  ? 1u : 0u);
    DELTA(c->band_ble_en    != d.band_ble_en,    "bble=%u", c->band_ble_en ? 1u : 0u);
    DELTA(c->dwell_ms       != d.dwell_ms,       "dwell_ms=%u", (unsigned)c->dwell_ms);
    #undef DELTA

    if (!any) { used += app(buf, cap, used, "(none)"); }
    return used;
}

/* ===========================================================================
 * KNOWN COMPILE RISKS (fix on the first real ESP-IDF 6.1 build)
 * ---------------------------------------------------------------------------
 *  1. <stdarg.h> is needed for va_list/va_start — add the include (it is
 *     pulled in transitively via esp_log.h on IDF, but DO NOT rely on that;
 *     add #include <stdarg.h> explicitly on the first build).            [C1]
 *  2. The printf-format attribute on app() will make -Wformat check the
 *     call sites; the %u args are cast to unsigned to match. Verify no
 *     -Wformat-truncation warning trips under -Werror (the DELTAs are short
 *     and the buffer is sized by the caller — logger passes >= 160 B).   [C2]
 *  3. nvs_flash_init() must have run (app_main does it) before fh_cfg_load;
 *     this module does not init NVS itself (the board/boot layer owns that).[C3]
 * ======================================================================== */
