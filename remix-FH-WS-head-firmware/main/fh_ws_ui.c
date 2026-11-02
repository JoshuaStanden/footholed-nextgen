/*
 * fh_ws_ui.c — Waveshare HEAD LVGL status screen.
 *
 * Builds a simple status face on the proven board adapter's LVGL display and
 * updates it from the hub's FH_OD_SYS_STATE_RESP feed. A screen tap is reported
 * upstream via the on_tap callback (the render layer forwards it to the hub).
 * All LVGL mutation outside LVGL's own task is wrapped in lvgl_port lock — but
 * the head drives LVGL from a single task (fh_ws_main loop), so set_state is
 * called from that same task; it still guards with a simple flag for clarity.
 */
#include "fh_ws_ui.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "fh_ws_ui";

static fh_ws_ui_tap_fn s_on_tap;
static lv_obj_t *s_title, *s_banner;
static lv_obj_t *s_l_uptime, *s_l_sd, *s_l_gps, *s_l_jobs, *s_l_cfg;

static void screen_tap_cb(lv_event_t *e)
{
    if (!s_on_tap) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_on_tap(p.x, p.y);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, lv_coord_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 8, y);
    return l;
}

void fh_ws_ui_init(fh_ws_ui_tap_fn on_tap)
{
    s_on_tap = on_tap;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_add_event_cb(scr, screen_tap_cb, LV_EVENT_CLICKED, NULL);

    s_title = lv_label_create(scr);
    lv_label_set_text(s_title, "FootHoled HEAD");
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 8, 8);

    s_banner = lv_label_create(scr);
    lv_label_set_text(s_banner, "waiting for hub...");
    lv_obj_align(s_banner, LV_ALIGN_TOP_LEFT, 8, 36);

    s_l_uptime = mk_label(scr, "uptime --",  70);
    s_l_sd     = mk_label(scr, "SD --",       98);
    s_l_gps    = mk_label(scr, "GPS --",     126);
    s_l_jobs   = mk_label(scr, "jobs --",    154);
    s_l_cfg    = mk_label(scr, "cfg --",     182);

    ESP_LOGI(TAG, "status UI built");
}

void fh_ws_ui_set_waiting(bool waiting)
{
    if (!s_banner) return;
    if (waiting) {
        lv_label_set_text(s_banner, "waiting for hub...");
        lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    }
}

void fh_ws_ui_set_state(const fh_od_sys_state_resp_t *st)
{
    if (!st || !s_l_uptime) return;
    char line[48];

    fh_ws_ui_set_waiting(false);

    snprintf(line, sizeof(line), "uptime %us", (unsigned)st->uptime_s);
    lv_label_set_text(s_l_uptime, line);

    snprintf(line, sizeof(line), "SD %s  free %uMB",
             st->sd_mounted ? "up" : "down", (unsigned)st->sd_free_mb);
    lv_label_set_text(s_l_sd, line);

    if (st->fix_valid) {
        snprintf(line, sizeof(line), "GPS fix  %d sats", (int)st->sats);
    } else {
        snprintf(line, sizeof(line), "GPS no fix");
    }
    lv_label_set_text(s_l_gps, line);

    snprintf(line, sizeof(line), "jobs %u  timeouts %u",
             (unsigned)st->jobs_done, (unsigned)st->job_timeouts);
    lv_label_set_text(s_l_jobs, line);

    snprintf(line, sizeof(line), "cfg %s  (%u overrides)",
             st->cfg_modified ? "MODIFIED" : "stock", (unsigned)st->override_count);
    lv_label_set_text(s_l_cfg, line);
}
