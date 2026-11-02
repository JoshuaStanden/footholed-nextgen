/*
 * fh_hub_ui.h — FHAdvanced-REMIX hub UI/status layer (P4 / headless)
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule).
 *
 * ON THE P4 THE HUB HAS NO PANEL. The Waveshare-S3 HEAD owns the LCD + touch;
 * the P4 hub EMITS display data to the head (a Bus-A peer) and the head renders
 * it. So this layer is the STATUS MODEL + a headless USB status line only:
 *   - a status task that samples health (7), logger (5) and BLE (4) and
 *     computes the silent-when-healthy status model (green/amber/red);
 *   - a headless STATUS LINE over the UART0 console (CH343, 921600) so you can watch
 *     the hub run with no local screen. The SAME status model will drive the
 *     head's render feed over Bus A in a later pass (only the output sink
 *     changes — no logic here).
 * There is no local screen-sleep / backlight / touch here — those left with
 * the panel when the hub moved to the P4.
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the UI/status task. Safe to call after board+logger+health+ble+link
 * are up. Returns false on a resource failure. */
bool fh_ui_start(void);

/* Register activity (retained in the API for a future head touch event relayed
 * upstream over Bus A). On the headless hub it only timestamps. */
void fh_ui_note_activity(void);

#ifdef __cplusplus
}
#endif
