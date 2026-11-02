/*
 * fh_ws_link.c — Waveshare HEAD Bus-A SPI-Slave-HD transport impl.
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule). Twin of the XIAO
 * HD links (see ../../remix-FH-XIAO-S3-firmware/main/fh_xs3_link.c KNOWN RISKS).
 * Bus A = QUAD SPI Slave HD, half-duplex, hub/ESSL-driven:
 *   TX chan -> our upstream frames (touch/PONG); hub Rd_DMA pulls, IRQ HIGH.
 *   RX chan -> hub's render/control frames; hub Wr_DMA pushes -> on_control.
 * FROZEN HD params: mode 0, command_bits=8, address_bits=8, dummy_bits=8,
 * 261 B segments. Bus-A slave is on FH_WS_BUSA_SPI_HOST (SPI3); the PROVEN LCD
 * adapter owns SPI2 (fh_ws_board.cpp) — see fh_ws_pins.h R-SPIHOST.
 */
#include "fh_ws_link.h"
#include "fh_ws_pins.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_slave_hd.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "fh_ws_link";

#define WS_XFER_BYTES   FH_MAX_FRAME
#define WS_SPI_HOST     FH_WS_BUSA_SPI_HOST
#define WS_RX_DMA_LEN   ((WS_XFER_BYTES + 3u) & ~3u)   /* 264, 4-byte aligned */

static fh_ws_pop_fn      s_pop;
static fh_ws_control_fn  s_on_control;
static fh_decoder_t      s_in_dec;
static uint8_t          *s_txbuf;
static uint8_t          *s_rxbuf;

static void irq_set(bool high) { gpio_set_level(FH_WS_IRQ_OUT, high ? 1 : 0); }

static void in_cb(fh_type_t type, const uint8_t *payload, uint8_t len, void *user)
{
    (void)user;
    if (s_on_control) { s_on_control(type, payload, len); }
}

static void rx_rearm(void)
{
    static spi_slave_hd_data_t rxdesc;
    rxdesc.data = s_rxbuf;
    rxdesc.len  = WS_RX_DMA_LEN;
    rxdesc.arg  = NULL;
    esp_err_t e = spi_slave_hd_queue_trans(WS_SPI_HOST, SPI_SLAVE_CHAN_RX,
                                           &rxdesc, portMAX_DELAY);
    if (e != ESP_OK) { ESP_LOGW(TAG, "rx queue err %s", esp_err_to_name(e)); }
}

static void link_task(void *arg)
{
    (void)arg;
    rx_rearm();
    for (;;) {
        memset(s_txbuf, 0, WS_XFER_BYTES);
        size_t n = s_pop ? s_pop(s_txbuf, WS_XFER_BYTES) : 0;
        if (n > 0) {
            static spi_slave_hd_data_t txdesc;
            txdesc.data = s_txbuf; txdesc.len = WS_XFER_BYTES; txdesc.arg = NULL;
            if (spi_slave_hd_queue_trans(WS_SPI_HOST, SPI_SLAVE_CHAN_TX,
                                         &txdesc, portMAX_DELAY) == ESP_OK) {
                irq_set(true);
                spi_slave_hd_data_t *done = NULL;
                esp_err_t e = spi_slave_hd_get_trans_res(WS_SPI_HOST,
                                 SPI_SLAVE_CHAN_TX, &done, pdMS_TO_TICKS(1000));
                irq_set(false);
                if (e != ESP_OK && e != ESP_ERR_TIMEOUT) {
                    ESP_LOGW(TAG, "tx res err %s", esp_err_to_name(e));
                }
            } else { irq_set(false); }
        }
        spi_slave_hd_data_t *rxdone = NULL;
        esp_err_t re = spi_slave_hd_get_trans_res(WS_SPI_HOST, SPI_SLAVE_CHAN_RX,
                                                  &rxdone, pdMS_TO_TICKS(5));
        if (re == ESP_OK && rxdone) {
            size_t rx_n = rxdone->trans_len;
            if (rx_n > 0 && rx_n <= WS_RX_DMA_LEN) {
                fh_decoder_push(&s_in_dec, s_rxbuf, rx_n, in_cb, NULL);
            }
            rx_rearm();
        }
        if (n == 0 && re != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(2)); }
    }
}

bool fh_ws_link_start(const fh_ws_link_cfg_t *cfg)
{
    if (!cfg || !cfg->pop) { return false; }
    s_pop = cfg->pop; s_on_control = cfg->on_control;
    fh_decoder_init(&s_in_dec);

    s_txbuf = heap_caps_malloc(WS_XFER_BYTES, MALLOC_CAP_DMA);
    s_rxbuf = heap_caps_malloc(WS_RX_DMA_LEN, MALLOC_CAP_DMA);
    if (!s_txbuf || !s_rxbuf) { ESP_LOGE(TAG, "DMA buf alloc failed"); return false; }

    gpio_config_t io = {0};
    io.pin_bit_mask = 1ULL << FH_WS_IRQ_OUT;
    io.mode         = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    irq_set(false);

    spi_bus_config_t bus = {0};
    bus.mosi_io_num   = FH_WS_SPI_MOSI;
    bus.miso_io_num   = FH_WS_SPI_MISO;
    bus.sclk_io_num   = FH_WS_SPI_SCK;
    bus.quadwp_io_num = FH_WS_SPI_WP;
    bus.quadhd_io_num = FH_WS_SPI_HD;
    bus.max_transfer_sz = WS_RX_DMA_LEN;

    spi_slave_hd_slot_config_t sc = {0};
    sc.mode         = 0;
    sc.spics_io_num = FH_WS_SPI_CS;
    sc.flags        = 0;
    sc.command_bits = 8;
    sc.address_bits = 8;
    sc.dummy_bits   = 8;
    sc.queue_size   = 4;
    sc.dma_chan     = SPI_DMA_CH_AUTO;

    esp_err_t e = spi_slave_hd_init(WS_SPI_HOST, &bus, &sc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spi_slave_hd_init failed: %s", esp_err_to_name(e));
        return false;
    }
    if (xTaskCreate(link_task, "fh_ws_link", 4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "link task create failed");
        return false;
    }
    ESP_LOGI(TAG, "Bus-A SPI-Slave-HD link up (head peer, IRQ GPIO%d)", FH_WS_IRQ_OUT);
    return true;
}

/* KNOWN RISKS: identical to the XIAO HD links (HD/ESSL pairing, QIO dummy
 * cycles #9823 — 1-bit HD first, RX 4-byte align, quad pins, multidrop).
 * HEAD-SPECIFIC: the Bus-A slave host (SPI3) differs from the PROVEN LCD host
 * (SPI2, fh_ws_board.cpp) — R-SPIHOST. Confirm the Bus-A GPIOs are free header
 * pins with no overlap on the LCD/I2C pins (fh_ws_pins.h). See the decision doc. */
