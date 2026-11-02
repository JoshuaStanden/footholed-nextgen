/*
 * fh_ws_main.c — FHAdvanced-REMIX Waveshare HEAD (the face) — app entry.
 *
 * Architecture (Option A, per the user): the display/touch/power layer is the
 * PROVEN original-FootHoled board adapter (fh_ws_board.cpp, ST7796 SPI +
 * TCA9554 + FT6336 + LVGL 9), reused because it is bench-working on THIS board.
 * The FootHoled-REMIX-specific parts live here:
 *   (1) Bus-A SPI-Slave-HD peer link to the P4 hub (fh_ws_link.c);
 *   (2) PING -> PONG so the hub lights health node 4 (FH_SRC_HEAD);
 *   (3) an LVGL status UI (fh_ws_ui.c) fed from the hub's SYS_STATE feed;
 *   (4) a screen tap reported upstream to the hub.
 *
 * LVGL is driven from THIS task's loop (lv_timer_handler); the Bus-A link runs
 * its own task. The hub feed arrives on the link task and is handed to the UI;
 * since LVGL is not thread-safe, state updates are marshalled to this task via
 * a small flag+copy (the link task only stores the latest state + sets a flag).
 *
 * NOT re-verified on hardware. The board half is a faithful copy of a proven
 * path; the REMIX link/UI half is new.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

#include "lvgl.h"
#include "fh_ws_board.h"
#include "fh_ws_ui.h"
#include "fh_ws_link.h"
#include "fh_ws_pins.h"
#include "fh_frame.h"
#include "fh_ondemand.h"

static const char *TAG = "fh_ws_main";

/* ---- upstream queue: frames the head sends to the hub (PONG, tap) ---- */
#define WS_TXQ_LEN 8
typedef struct { uint8_t buf[FH_MAX_FRAME]; size_t len; } ws_frame_t;
static QueueHandle_t s_txq;

static void enqueue_frame(fh_type_t type, const uint8_t *payload, uint8_t len)
{
    ws_frame_t f;
    f.len = fh_frame_encode(type, payload, len, f.buf, sizeof(f.buf));
    if (f.len) { xQueueSend(s_txq, &f, 0); }
}

/* link pop: drain one upstream frame for the hub to Rd_DMA. */
static size_t link_pop(uint8_t *out, size_t cap)
{
    ws_frame_t f;
    if (xQueueReceive(s_txq, &f, 0) == pdTRUE) {
        size_t n = f.len < cap ? f.len : cap;
        memcpy(out, f.buf, n);
        return n;
    }
    return 0;
}

/* ---- latest hub state, marshalled from the link task to the LVGL task ---- */
static volatile bool s_state_dirty = false;
static fh_od_sys_state_resp_t s_state;      /* written on link task, read on LVGL task */

/* link on_control: a frame arrived from the hub (runs on the LINK task). */
static void link_on_control(fh_type_t type, const uint8_t *payload, uint8_t len)
{
    switch ((uint8_t)type) {
        case FH_TYPE_PING:
            if (len >= 1) {
                fh_ping_t p; p.seq = payload[0];
                enqueue_frame(FH_TYPE_PONG, (uint8_t *)&p, sizeof(p));
            }
            break;
        case FH_OD_SYS_STATE_RESP:
            if (len >= sizeof(fh_od_sys_state_resp_t)) {
                memcpy(&s_state, payload, sizeof(s_state));
                s_state_dirty = true;       /* LVGL task picks it up */
            }
            break;
        /* FUTURE: case FH_TYPE_HEAD_RENDER: dedicated render frame (parked). */
        default: break;
    }
}

/* ---- tap -> upstream (runs on the LVGL task via the UI event) ---- */
static void on_tap(int32_t x, int32_t y)
{
    /* Framework: a dedicated touch-event frame type is a FUTURE addition. For
     * now a tap nudges the hub with a sentinel PING seq 0xFE so liveness/intent
     * is visible; x/y carried here for when the real event frame lands. */
    (void)x; (void)y;
    uint8_t seq = 0xFE;
    enqueue_frame(FH_TYPE_PING, &seq, 1);
}

void app_main(void)
{
    ESP_LOGI(TAG, "FHAdvanced-REMIX HEAD boot");

    s_txq = xQueueCreate(WS_TXQ_LEN, sizeof(ws_frame_t));

    /* LVGL core + proven board adapter (display/touch/backlight). lv_init MUST
     * precede board init (the adapter calls lv_display_create). */
    lv_init();
    const bool display_ready = fh_ws_board_init();
    if (!display_ready) {
        ESP_LOGE(TAG, "board init failed — running headless (link only)");
    } else {
        fh_ws_board_set_backlight(80);
        fh_ws_ui_init(on_tap);        /* needs a registered LVGL display */
        fh_ws_ui_set_waiting(true);
    }

    /* Bus-A SPI-Slave-HD link to the hub (own task). Runs even headless so the
     * hub still sees PONG (health node 4) if only the panel failed. */
    fh_ws_link_cfg_t lc = { .pop = link_pop, .on_control = link_on_control };
    if (!fh_ws_link_start(&lc)) { ESP_LOGE(TAG, "Bus-A link failed"); }

    /* LVGL service loop. Accumulate only whole ms into the tick so the sub-ms
     * remainder is not discarded each pass (matches the proven main.cpp). */
    int64_t last = esp_timer_get_time();
    for (;;) {
        int64_t now = esp_timer_get_time();
        uint32_t elapsed = (uint32_t)((now - last) / 1000);
        if (elapsed > 0) {
            lv_tick_inc(elapsed);
            last += (int64_t)elapsed * 1000;
        }
        if (display_ready) {
            if (s_state_dirty) {
                s_state_dirty = false;
                fh_od_sys_state_resp_t snap = s_state;   /* tolerated struct-copy race */
                fh_ws_ui_set_state(&snap);
            }
            lv_timer_handler();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
