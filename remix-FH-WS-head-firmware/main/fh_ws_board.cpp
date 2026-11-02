/*
 * fh_ws_board.cpp — Waveshare ESP32-S3-Touch-LCD-3.5 (NON-B) display + touch.
 *
 * FAITHFUL PORT of the PROVEN original FootHoled board adapter
 * (JoshuaStanden/FootHoled @ josh-dev, firmware/src/board_adapter.cpp), which
 * is bench-working on THIS exact board. The REMIX head reuses it verbatim where
 * possible; the only trims are the QMI8658 auto-rotate service and the
 * settings.h coupling (the head gets its orientation from the hub feed, not a
 * local settings store). ALL pin numbers and the ST7796/TCA9554/LVGL config are
 * UNCHANGED from the proven baseline — see firmware/DISPLAY_NOTES.md for the
 * regression history behind each one (TCA9554 reset pulse, internal-DMA draw
 * buffers, OCTAL PSRAM, single mirror). Do NOT "fix" PSRAM to QUAD (R8 is octal).
 *
 * NOT re-verified on hardware by REMIX — but it is a byte-faithful copy of a
 * path the user has already run on this board.
 */
#include "fh_ws_board.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"            // espressif/esp_lcd_st7796
#include "esp_io_expander_tca9554.h"  // espressif/esp_io_expander_tca9554
#include "lvgl.h"
#include "freertos/FreeRTOS.h"

#if LV_COLOR_DEPTH != 16
#error "fh_ws_board.cpp requires LV_COLOR_DEPTH==16 (RGB565) to match ST7796."
#endif

static const char *TAG = "fh_ws_board";

// --- Pins — VERIFIED against the proven FootHoled board_adapter.cpp ----------
#define FH_PIN_LCD_MISO  GPIO_NUM_NC   // not used
#define FH_PIN_LCD_MOSI  GPIO_NUM_1    // SPI MOSI (data to panel)
#define FH_PIN_LCD_SCLK  GPIO_NUM_5    // SPI clock
#define FH_PIN_LCD_CS    GPIO_NUM_NC   // CS not a dedicated GPIO
#define FH_PIN_LCD_DC    GPIO_NUM_3    // data/command select
#define FH_PIN_LCD_RST   GPIO_NUM_NC   // no reset GPIO (on TCA9554)
#define FH_PIN_LCD_BL    GPIO_NUM_6    // backlight (LEDC PWM)
#define FH_PIN_I2C_SDA   GPIO_NUM_8    // shared I2C0 — header pin 28
#define FH_PIN_I2C_SCL   GPIO_NUM_7    // shared I2C0 — header pin 26
#define FH_PIN_TP_INT    GPIO_NUM_NC   // touch INT not wired (polled)
#define FH_PIN_TP_RST    GPIO_NUM_NC   // touch RST on TCA9554

// --- Display params -----------------------------------------------------------
#define FH_LCD_H_RES          320
#define FH_LCD_V_RES          480
#define FH_LCD_PCLK_HZ        (80 * 1000 * 1000)
#define FH_SPI_HOST           SPI2_HOST
#define FH_LCD_CMD_BITS       8
#define FH_LCD_PARAM_BITS     8
#define FH_BUF_LINES          (FH_LCD_V_RES / 20)
#define FH_BUF_SIZE           (FH_LCD_H_RES * FH_BUF_LINES)

// --- Backlight LEDC (matches vendor: 10-bit, 5 kHz, burn-in-safe ceiling) ----
#define FH_BL_LEDC_TIMER      LEDC_TIMER_1
#define FH_BL_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define FH_BL_LEDC_CHANNEL    LEDC_CHANNEL_0
#define FH_BL_DUTY_BITS       LEDC_TIMER_10_BIT
#define FH_BL_FREQ_HZ         5000
#define FH_BL_DUTY_MAX        1023
#define FH_BL_CEIL_PERCENT    85
#define FH_BL_DUTY_CEIL       ((FH_BL_DUTY_MAX * FH_BL_CEIL_PERCENT) / 100)

// --- FT6336 touch -------------------------------------------------------------
#define FH_TOUCH_I2C_ADDR     0x38
#define FT6336_REG_TD_STATUS  0x02
#define FT6336_REG_P1_XH      0x03

static esp_lcd_panel_io_handle_t  s_io_handle    = nullptr;
static esp_lcd_panel_handle_t     s_panel_handle = nullptr;
static i2c_master_bus_handle_t    s_i2c_bus      = nullptr;
static i2c_master_dev_handle_t    s_touch_dev    = nullptr;
static esp_io_expander_handle_t   s_io_expander  = nullptr;
static lv_display_t              *s_lvgl_disp    = nullptr;
static lv_color_t *s_lvgl_buf1 = nullptr, *s_lvgl_buf2 = nullptr, *s_rotate_buf = nullptr;

// --- LVGL 9 async flush (DMA; completion signalled by trans-done cb) ---------
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    lv_area_t out_area = *area;
    uint8_t  *out_px   = px_map;
    lv_display_rotation_t rot = lv_display_get_rotation(disp);
    if (rot != LV_DISPLAY_ROTATION_0 && s_rotate_buf) {
        lv_color_format_t cf = lv_display_get_color_format(disp);
        lv_display_rotate_area(disp, &out_area);
        int32_t src_w = lv_area_get_width(area), src_h = lv_area_get_height(area);
        uint32_t ss = lv_draw_buf_width_to_stride(src_w, cf);
        uint32_t ds = lv_draw_buf_width_to_stride(lv_area_get_width(&out_area), cf);
        lv_draw_sw_rotate(px_map, s_rotate_buf, src_w, src_h, ss, ds, rot, cf);
        out_px = (uint8_t *)s_rotate_buf;
    }
    const uint32_t n = (out_area.x2 - out_area.x1 + 1) * (out_area.y2 - out_area.y1 + 1);
    lv_draw_sw_rgb565_swap(out_px, n);
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel_handle,
        out_area.x1, out_area.y1, out_area.x2 + 1, out_area.y2 + 1, out_px);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap: %s — releasing buffer", esp_err_to_name(err));
        lv_display_flush_ready(disp);
    }
}

static bool colour_trans_done_cb(esp_lcd_panel_io_handle_t,
                                 esp_lcd_panel_io_event_data_t *, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    if (disp) lv_display_flush_ready(disp);
    return false;
}

// --- FT6336 raw read (standard FT6x36 register protocol) ---------------------
static bool read_touch_raw(uint16_t *x_out, uint16_t *y_out)
{
    if (!s_touch_dev) return false;
    const uint8_t start = FT6336_REG_TD_STATUS;
    uint8_t buf[5] = {};
    if (i2c_master_transmit_receive(s_touch_dev, &start, 1, buf, sizeof(buf), 50) != ESP_OK)
        return false;
    uint8_t points = buf[0] & 0x0F;
    if (points < 1 || points > 2) return false;
    *x_out = (uint16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
    *y_out = (uint16_t)(((buf[3] & 0x0F) << 8) | buf[4]);
    return true;
}

static void touch_read_cb(lv_indev_t *, lv_indev_data_t *data)
{
    uint16_t x = 0, y = 0;
    if (read_touch_raw(&x, &y)) {
        data->point.x = (int32_t)x; data->point.y = (int32_t)y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void backlight_init(void)
{
    ledc_timer_config_t t = {};
    t.speed_mode = FH_BL_LEDC_MODE; t.timer_num = FH_BL_LEDC_TIMER;
    t.duty_resolution = FH_BL_DUTY_BITS; t.freq_hz = FH_BL_FREQ_HZ; t.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&t);
    ledc_channel_config_t c = {};
    c.speed_mode = FH_BL_LEDC_MODE; c.channel = FH_BL_LEDC_CHANNEL; c.timer_sel = FH_BL_LEDC_TIMER;
    c.gpio_num = FH_PIN_LCD_BL; c.duty = FH_BL_DUTY_MAX / 2; c.hpoint = 0;
    ledc_channel_config(&c);
    ESP_LOGI(TAG, "Backlight on 50%% (GPIO%d PWM)", FH_PIN_LCD_BL);
}

extern "C" void fh_ws_board_set_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = ((uint32_t)percent * FH_BL_DUTY_CEIL) / 100u;
    ledc_set_duty(FH_BL_LEDC_MODE, FH_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(FH_BL_LEDC_MODE, FH_BL_LEDC_CHANNEL);
}

extern "C" bool fh_ws_board_init(void)
{
    ESP_LOGI(TAG, "Init Waveshare 3.5 (NON-B: ST7796 SPI + FT6336 + TCA9554)");

    // 1. SPI bus (standard SPI, NOT QSPI)
    spi_bus_config_t bus = {};
    bus.sclk_io_num = FH_PIN_LCD_SCLK; bus.mosi_io_num = FH_PIN_LCD_MOSI;
    bus.miso_io_num = FH_PIN_LCD_MISO; bus.quadwp_io_num = -1; bus.quadhd_io_num = -1;
    bus.max_transfer_sz = FH_LCD_H_RES * FH_BUF_LINES * (int)sizeof(uint16_t);
    if (spi_bus_initialize(FH_SPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed"); return false;
    }

    // 1b. I2C bus + TCA9554 reset pulse — BEFORE the panel (else blank screen)
    i2c_master_bus_config_t ic = {};
    ic.i2c_port = I2C_NUM_0; ic.sda_io_num = FH_PIN_I2C_SDA; ic.scl_io_num = FH_PIN_I2C_SCL;
    ic.clk_source = I2C_CLK_SRC_DEFAULT; ic.glitch_ignore_cnt = 7;
    ic.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&ic, &s_i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed"); return false;
    }
    if (esp_io_expander_new_i2c_tca9554(s_i2c_bus,
            ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &s_io_expander) != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 init failed — LCD reset line unavailable"); return false;
    }
    ESP_ERROR_CHECK(esp_io_expander_set_dir(s_io_expander, IO_EXPANDER_PIN_NUM_1, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_io_expander, IO_EXPANDER_PIN_NUM_1, 0)); // assert
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_io_expander, IO_EXPANDER_PIN_NUM_1, 1)); // release
    vTaskDelay(pdMS_TO_TICKS(100));

    // 2. Panel IO (SPI)
    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = FH_PIN_LCD_CS; io.dc_gpio_num = FH_PIN_LCD_DC; io.spi_mode = 0;
    io.pclk_hz = FH_LCD_PCLK_HZ; io.trans_queue_depth = 10;
    io.lcd_cmd_bits = FH_LCD_CMD_BITS; io.lcd_param_bits = FH_LCD_PARAM_BITS;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)FH_SPI_HOST, &io, &s_io_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO create failed"); return false;
    }

    // 3. ST7796 panel (BGR + invert + mirror(X), exactly as the vendor port)
    esp_lcd_panel_dev_config_t pc = {};
    pc.reset_gpio_num = FH_PIN_LCD_RST; pc.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    pc.bits_per_pixel = 16;
    if (esp_lcd_new_panel_st7796(s_io_handle, &pc, &s_panel_handle) != ESP_OK) {
        ESP_LOGE(TAG, "ST7796 panel create failed"); return false;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, false));

    // 4. FT6336 touch on the shared I2C bus
    i2c_device_config_t tc = {};
    tc.dev_addr_length = I2C_ADDR_BIT_LEN_7; tc.device_address = FH_TOUCH_I2C_ADDR;
    tc.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(s_i2c_bus, &tc, &s_touch_dev) != ESP_OK) {
        ESP_LOGW(TAG, "FT6336 add failed — touch disabled, UI still runs");
        s_touch_dev = nullptr;
    }

    // 5. LVGL display + async flush + internal-DMA draw buffers
    lv_display_t *disp = lv_display_create(FH_LCD_H_RES, FH_LCD_V_RES);
    if (!disp) { ESP_LOGE(TAG, "lv_display_create failed"); return false; }
    s_lvgl_disp = disp;
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = colour_trans_done_cb };
    if (esp_lcd_panel_io_register_event_callbacks(s_io_handle, &cbs, disp) != ESP_OK) {
        ESP_LOGE(TAG, "register trans-done cb failed"); return false;
    }
    const uint32_t cap = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    s_lvgl_buf1  = (lv_color_t *)heap_caps_malloc(FH_BUF_SIZE * sizeof(lv_color_t), cap);
    s_lvgl_buf2  = (lv_color_t *)heap_caps_malloc(FH_BUF_SIZE * sizeof(lv_color_t), cap);
    s_rotate_buf = (lv_color_t *)heap_caps_malloc(FH_BUF_SIZE * sizeof(lv_color_t), cap);
    if (!s_lvgl_buf1 || !s_lvgl_buf2 || !s_rotate_buf) {
        ESP_LOGW(TAG, "internal DMA draw-buffer alloc failed; falling back to PSRAM");
        if (s_lvgl_buf1) heap_caps_free(s_lvgl_buf1);
        if (s_lvgl_buf2) heap_caps_free(s_lvgl_buf2);
        if (s_rotate_buf) heap_caps_free(s_rotate_buf);
        s_lvgl_buf1  = (lv_color_t *)heap_caps_malloc(FH_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
        s_lvgl_buf2  = (lv_color_t *)heap_caps_malloc(FH_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
        s_rotate_buf = (lv_color_t *)heap_caps_malloc(FH_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }
    if (!s_lvgl_buf1 || !s_lvgl_buf2 || !s_rotate_buf) {
        ESP_LOGE(TAG, "draw buffer allocation failed"); return false;
    }
    lv_display_set_buffers(disp, s_lvgl_buf1, s_lvgl_buf2,
                           FH_BUF_SIZE * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, true));

    // 6. LVGL touch indev
    lv_indev_t *indev = lv_indev_create();
    if (indev) {
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read_cb);
    } else {
        ESP_LOGW(TAG, "LVGL touch indev create failed");
    }

    // 7. Backlight
    backlight_init();

    ESP_LOGI(TAG, "Board init complete — %dx%d ST7796@%dMHz, touch %s",
             FH_LCD_H_RES, FH_LCD_V_RES, FH_LCD_PCLK_HZ / 1000000,
             indev ? "on" : "OFF");
    return true;
}

extern "C" i2c_master_bus_handle_t fh_ws_board_i2c_bus(void) { return s_i2c_bus; }
extern "C" bool fh_ws_board_display_ready(void) { return s_lvgl_disp != nullptr; }
