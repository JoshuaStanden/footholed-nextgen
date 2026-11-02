/*
 * fh_hub_ble.h — FHAdvanced hub BLE sniffer (SUBSYSTEM 4)
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule).
 *
 * The hub's OWN radio doing BLE scan — a fifth data source, hub-local. Emits
 * the SAME record shape as a CAPTURE with source = "ws-ble" (FH_SRCSTR_WS_BLE)
 * into the logger (subsystem 5) via fh_log_submit() — identical path to a UART
 * record, so the logger does not care where a row came from.
 *
 * NAMING NOTE (user): "BLE" = Bluetooth Low Energy; the user calls it
 * "Bluetooth/BT". Same thing. The hub does BLE observe/scan, not Classic.
 *
 * Scan mode (CONFIG_MENU #3): PASSIVE by default (observe adv packets only);
 * ACTIVE is opt-in (sends scan-requests — liability on the operator, only
 * probes devices that already advertise). Identity-based dedup "seen is seen"
 * (CONFIG_MENU #4), RSSI recorded once, re-log per the window.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "fh_hub_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the BLE scanner with the config's scan mode + re-log policy. Reads the
 * live config (fh_cfg_get) for passive/active and the window. Returns false if
 * the BT controller / BLE stack could not start (non-fatal to the hub — the UI
 * shows a red BLE chip and the other sources keep logging). Honors
 * cfg->band_ble_en (if BLE capture is disabled, this is a no-op returning true).
 */
bool fh_ble_start(void);

/* Diagnostics for the health UI. */
typedef struct {
    bool     running;
    uint32_t adv_seen;        /* advertising packets observed                  */
    uint32_t rows_emitted;    /* ws-ble rows submitted to the logger           */
    uint32_t dropped_keys;    /* seen-table evictions                          */
} fh_ble_stats_t;
fh_ble_stats_t fh_ble_stats(void);

#ifdef __cplusplus
}
#endif
