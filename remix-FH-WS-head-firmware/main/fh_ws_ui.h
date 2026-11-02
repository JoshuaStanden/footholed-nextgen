/*
 * fh_ws_ui.h — Waveshare HEAD LVGL status UI.
 * Built on the proven board adapter's LVGL display. The render layer feeds it
 * the hub's status model; it also reports taps upstream via a callback.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fh_ondemand.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called when the user taps the screen (x/y in LVGL display coords). The render
 * layer forwards this to the hub as an upstream frame. */
typedef void (*fh_ws_ui_tap_fn)(int32_t x, int32_t y);

/* Build the LVGL status screen. Call AFTER fh_ws_board_init(). */
void fh_ws_ui_init(fh_ws_ui_tap_fn on_tap);

/* Update the screen from the hub's system-state feed (thread-safe: locks LVGL).
 * Call when a new FH_OD_SYS_STATE_RESP arrives. */
void fh_ws_ui_set_state(const fh_od_sys_state_resp_t *st);

/* Show a "waiting for hub" banner until the first state arrives. */
void fh_ws_ui_set_waiting(bool waiting);

#ifdef __cplusplus
}
#endif
