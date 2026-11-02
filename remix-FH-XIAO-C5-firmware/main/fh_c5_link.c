/*
 * fh_c5_link.c — FHAdvanced-REMIX XIAO-C5 Bus-A SPI-SLAVE-HD transport impl.
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule). Twin of the XIAO-S3
 * link; see ../../remix-BUS-A-QUAD-HD-DECISION.md and the S3 file's KNOWN RISKS
 * for the full rationale. Bus A = QUAD SPI, SPI Slave HD (half-duplex,
 * master/ESSL-driven). TX chan = our outbound frame (hub Rd_DMA pulls it; IRQ
 * HIGH = "Rd_DMA me"); RX chan = inbound control (hub Wr_DMA pushes it).
 * PUBLIC CONTRACT (pop/on_control/fh_c5_link_start) UNCHANGED — fh_c5_main.c
 * (the 5 GHz capture path) is untouched.
 *
 * FROZEN HD params (match the hub + peers): mode 0, command_bits=8,
 * address_bits=8, dummy_bits=8, 261 B segments. 1-bit-HD first, QIO after the
 * dummy cycles are aligned (Espressif issue #9823).
 */
#include "fh_c5_link.h"
#include "fh_c5_pins.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_slave_hd.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "fh_c5_link";

#define C5_XFER_BYTES   FH_MAX_FRAME               /* 261, matches the hub */
#define C5_SPI_HOST     SPI2_HOST                  /* C5 general SPI host (HD slave) */
#define C5_RX_DMA_LEN   ((C5_XFER_BYTES + 3u) & ~3u)   /* 264, 4-byte aligned */

static fh_c5_pop_fn      s_pop;
static fh_c5_control_fn  s_on_control;
static fh_decoder_t      s_in_dec;

static uint8_t *s_txbuf;
static uint8_t *s_rxbuf;

static void irq_set(bool high) { gpio_set_level(FH_C5_IRQ_OUT, high ? 1 : 0); }

/* inbound decoder callback -> hand each control frame to main */
static void in_cb(fh_type_t type, const uint8_t *payload, uint8_t len, void *user)
{
    (void)user;
    if (s_on_control) { s_on_control(type, payload, len); }
}

static void rx_rearm(void)
{
    static spi_slave_hd_data_t rxdesc;
    rxdesc.data = s_rxbuf;
    rxdesc.len  = C5_RX_DMA_LEN;
    rxdesc.arg  = NULL;
    esp_err_t e = spi_slave_hd_queue_trans(C5_SPI_HOST, SPI_SLAVE_CHAN_RX,
                                           &rxdesc, portMAX_DELAY);
    if (e != ESP_OK) { ESP_LOGW(TAG, "rx queue err %s", esp_err_to_name(e)); }
}

static void link_task(void *arg)
{
    (void)arg;
    rx_rearm();

    for (;;) {
        memset(s_txbuf, 0, C5_XFER_BYTES);
        size_t n = s_pop ? s_pop(s_txbuf, C5_XFER_BYTES) : 0;
        if (n > 0) {
            static spi_slave_hd_data_t txdesc;
            txdesc.data = s_txbuf;
            txdesc.len  = C5_XFER_BYTES;
            txdesc.arg  = NULL;
            if (spi_slave_hd_queue_trans(C5_SPI_HOST, SPI_SLAVE_CHAN_TX,
                                         &txdesc, portMAX_DELAY) == ESP_OK) {
                irq_set(true);
                spi_slave_hd_data_t *done = NULL;
                esp_err_t e = spi_slave_hd_get_trans_res(C5_SPI_HOST,
                                 SPI_SLAVE_CHAN_TX, &done, pdMS_TO_TICKS(1000));
                irq_set(false);
                if (e != ESP_OK && e != ESP_ERR_TIMEOUT) {
                    ESP_LOGW(TAG, "tx res err %s", esp_err_to_name(e));
                }
            } else {
                irq_set(false);
            }
        }

        spi_slave_hd_data_t *rxdone = NULL;
        esp_err_t re = spi_slave_hd_get_trans_res(C5_SPI_HOST, SPI_SLAVE_CHAN_RX,
                                                  &rxdone, pdMS_TO_TICKS(5));
        if (re == ESP_OK && rxdone) {
            size_t rx_n = rxdone->trans_len;
            if (rx_n > 0 && rx_n <= C5_RX_DMA_LEN) {
                fh_decoder_push(&s_in_dec, s_rxbuf, rx_n, in_cb, NULL);
            }
            rx_rearm();
        }
        if (n == 0 && re != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(2)); }
    }
}

bool fh_c5_link_start(const fh_c5_link_cfg_t *cfg)
{
    if (!cfg || !cfg->pop) { return false; }
    s_pop        = cfg->pop;
    s_on_control = cfg->on_control;
    fh_decoder_init(&s_in_dec);

    s_txbuf = heap_caps_malloc(C5_XFER_BYTES, MALLOC_CAP_DMA);
    s_rxbuf = heap_caps_malloc(C5_RX_DMA_LEN, MALLOC_CAP_DMA);
    if (!s_txbuf || !s_rxbuf) { ESP_LOGE(TAG, "DMA buf alloc failed"); return false; }

    gpio_config_t io = {0};
    io.pin_bit_mask = 1ULL << FH_C5_IRQ_OUT;
    io.mode         = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    irq_set(false);

    spi_bus_config_t bus = {0};
    bus.mosi_io_num   = FH_C5_SPI_MOSI;   /* DQ0 */
    bus.miso_io_num   = FH_C5_SPI_MISO;   /* DQ1 */
    bus.sclk_io_num   = FH_C5_SPI_SCK;
    bus.quadwp_io_num = FH_C5_SPI_WP;     /* DQ2 (QIO) */
    bus.quadhd_io_num = FH_C5_SPI_HD;     /* DQ3 (QIO) */
    bus.max_transfer_sz = C5_RX_DMA_LEN;

    spi_slave_hd_slot_config_t sc = {0};
    sc.mode         = 0;
    sc.spics_io_num = FH_C5_SPI_CS;
    sc.flags        = 0;
    sc.command_bits = 8;
    sc.address_bits = 8;
    sc.dummy_bits   = 8;
    sc.queue_size   = 4;
    sc.dma_chan     = SPI_DMA_CH_AUTO;

    esp_err_t e = spi_slave_hd_init(C5_SPI_HOST, &bus, &sc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spi_slave_hd_init failed: %s", esp_err_to_name(e));
        return false;
    }

    if (xTaskCreate(link_task, "fh_c5_link", 4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "link task create failed");
        return false;
    }
    ESP_LOGI(TAG, "Bus-A SPI-Slave-HD link up (quad-capable, IRQ GPIO%d)",
             FH_C5_IRQ_OUT);
    return true;
}

/* ============================================================================
 * KNOWN RISKS — identical to the XIAO-S3 link; see
 * ../../remix-FH-XIAO-S3-firmware/main/fh_xs3_link.c (L1 HD/ESSL pairing,
 * L2 QIO dummy cycles #9823 — bring up 1-bit HD first, L3 RX 4-byte align,
 * L4 quad pins, L6 multidrop tri-state). C5-specific: the IRQ/CS/QIO pins are
 * on the C5 silkscreen (see fh_c5_pins.h) — VERIFY free + broken out.
 * ========================================================================== */
