/*
 * fh_xs3_link.c — FHAdvanced-REMIX XIAO-S3 Bus-A SPI-SLAVE-HD transport impl.
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule). See KNOWN RISKS end.
 *
 * ── Bus A = QUAD SPI, SPI Slave HD (committed 2026-11-01) ───────────────────
 * See ../../remix-BUS-A-QUAD-HD-DECISION.md. The hub is an ESSL/HD MASTER; this
 * board is a `spi_slave_hd` slave. HD is HALF-DUPLEX, master-driven:
 *   - TX (board -> hub): we queue the staged outbound frame on SPI_SLAVE_CHAN_TX;
 *     the hub pulls it with an Rd_DMA command. We raise FH_XS3_IRQ_OUT while a
 *     TX frame is staged ("Rd_DMA me").
 *   - RX (hub -> board): we keep an RX DMA buffer queued on SPI_SLAVE_CHAN_RX;
 *     the hub pushes a control frame with a Wr_DMA command. We decode it and
 *     hand each validated frame to on_control().
 * The PUBLIC CONTRACT (pop/on_control/fh_xs3_link_start) is UNCHANGED from the
 * 1-bit version, so fh_xs3_main.c (the capture path) is untouched.
 *
 * FROZEN HD params (must match the hub + the other peers — see the decision
 * doc): mode 0, command_bits=8, address_bits=8, dummy_bits=8, 261 B segments.
 * Data-phase line width (1-bit vs QIO) is a bench ramp: bring up 1-bit HD, then
 * QIO once the dummy cycles are confirmed (Espressif issue #9823). The slave
 * driver auto-handles the line width the master's command selects; no code
 * change here to go QIO — it is the master's command mask + matched dummy_bits.
 */
#include "fh_xs3_link.h"
#include "fh_xs3_pins.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_slave_hd.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "fh_xs3_link";

#define XS3_XFER_BYTES   FH_MAX_FRAME   /* 261, matches the hub master */
#define XS3_SPI_HOST     SPI2_HOST      /* XIAO-S3 general SPI host (HD slave) */

/* RX DMA length must be a multiple of 4 (HD driver truncates otherwise). Round
 * 261 up to 264. */
#define XS3_RX_DMA_LEN   ((XS3_XFER_BYTES + 3u) & ~3u)   /* 264 */

static fh_xs3_pop_fn     s_pop;
static fh_xs3_control_fn s_on_control;
static fh_decoder_t      s_in_dec;      /* decodes inbound (Wr_DMA) bytes */

/* inbound decoder callback -> hand each control frame to main */
static void in_cb(fh_type_t type, const uint8_t *payload, uint8_t len, void *user)
{
    (void)user;
    if (s_on_control) { s_on_control(type, payload, len); }
}

/* DMA-capable buffers (HD requires DMA-capable; RX length 4-aligned). */
static uint8_t *s_txbuf;                /* staged outbound frame (TX chan)      */
static uint8_t *s_rxbuf;                /* inbound control landing (RX chan)    */

static void irq_set(bool high) { gpio_set_level(FH_XS3_IRQ_OUT, high ? 1 : 0); }

/* Keep exactly one RX descriptor queued so the hub can Wr_DMA control at any
 * time; re-queue it after each completion. */
static void rx_rearm(void)
{
    static spi_slave_hd_data_t rxdesc;   /* persists while queued */
    rxdesc.data = s_rxbuf;
    rxdesc.len  = XS3_RX_DMA_LEN;
    rxdesc.arg  = NULL;
    esp_err_t e = spi_slave_hd_queue_trans(XS3_SPI_HOST, SPI_SLAVE_CHAN_RX,
                                           &rxdesc, portMAX_DELAY);
    if (e != ESP_OK) { ESP_LOGW(TAG, "rx queue err %s", esp_err_to_name(e)); }
}

static void link_task(void *arg)
{
    (void)arg;
    rx_rearm();                          /* always have an RX buffer waiting */

    for (;;) {
        /* ---- stage an outbound frame for the hub to Rd_DMA, if we have one ---- */
        memset(s_txbuf, 0, XS3_XFER_BYTES);
        size_t n = s_pop ? s_pop(s_txbuf, XS3_XFER_BYTES) : 0;
        if (n > 0) {
            static spi_slave_hd_data_t txdesc;
            txdesc.data = s_txbuf;
            txdesc.len  = XS3_XFER_BYTES;   /* send a full fixed segment (0-pad) */
            txdesc.arg  = NULL;
            if (spi_slave_hd_queue_trans(XS3_SPI_HOST, SPI_SLAVE_CHAN_TX,
                                         &txdesc, portMAX_DELAY) == ESP_OK) {
                irq_set(true);              /* "Rd_DMA me" */
                /* block until the hub pulls it (or a long timeout re-checks) */
                spi_slave_hd_data_t *done = NULL;
                esp_err_t e = spi_slave_hd_get_trans_res(XS3_SPI_HOST,
                                 SPI_SLAVE_CHAN_TX, &done, pdMS_TO_TICKS(1000));
                irq_set(false);
                if (e != ESP_OK && e != ESP_ERR_TIMEOUT) {
                    ESP_LOGW(TAG, "tx res err %s", esp_err_to_name(e));
                }
            } else {
                irq_set(false);
            }
        }

        /* ---- drain any inbound control the hub Wr_DMA'd (non-blocking-ish) ---- */
        spi_slave_hd_data_t *rxdone = NULL;
        esp_err_t re = spi_slave_hd_get_trans_res(XS3_SPI_HOST, SPI_SLAVE_CHAN_RX,
                                                  &rxdone, pdMS_TO_TICKS(5));
        if (re == ESP_OK && rxdone) {
            size_t rx_n = rxdone->trans_len;          /* bytes actually received */
            if (rx_n > 0 && rx_n <= XS3_RX_DMA_LEN) {
                fh_decoder_push(&s_in_dec, s_rxbuf, rx_n, in_cb, NULL);
            }
            rx_rearm();                               /* re-queue the RX buffer */
        }
        /* if no TX staged and no RX completed, yield briefly */
        if (n == 0 && re != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(2)); }
    }
}

bool fh_xs3_link_start(const fh_xs3_link_cfg_t *cfg)
{
    if (!cfg || !cfg->pop) { return false; }
    s_pop        = cfg->pop;
    s_on_control = cfg->on_control;
    fh_decoder_init(&s_in_dec);

    /* DMA-capable buffers (RX length 4-aligned per the HD driver). */
    s_txbuf = heap_caps_malloc(XS3_XFER_BYTES, MALLOC_CAP_DMA);
    s_rxbuf = heap_caps_malloc(XS3_RX_DMA_LEN, MALLOC_CAP_DMA);
    if (!s_txbuf || !s_rxbuf) { ESP_LOGE(TAG, "DMA buf alloc failed"); return false; }

    /* IRQ-out GPIO (idle low). */
    gpio_config_t io = {0};
    io.pin_bit_mask = 1ULL << FH_XS3_IRQ_OUT;
    io.mode         = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    irq_set(false);

    /* Bus pins. QUADWP/QUADHD provided so the master can drive QIO once the
     * data phase is widened (see decision doc). For 1-bit-HD bring-up they are
     * simply unused; wiring them does no harm. */
    spi_bus_config_t bus = {0};
    bus.mosi_io_num   = FH_XS3_SPI_MOSI;   /* DQ0 */
    bus.miso_io_num   = FH_XS3_SPI_MISO;   /* DQ1 */
    bus.sclk_io_num   = FH_XS3_SPI_SCK;
    bus.quadwp_io_num = FH_XS3_SPI_WP;     /* DQ2 — see pins.h (QIO) */
    bus.quadhd_io_num = FH_XS3_SPI_HD;     /* DQ3 — see pins.h (QIO) */
    bus.max_transfer_sz = XS3_RX_DMA_LEN;

    spi_slave_hd_slot_config_t sc = {0};
    sc.mode         = 0;                   /* FROZEN: CPOL0/CPHA0 */
    sc.spics_io_num = FH_XS3_SPI_CS;
    sc.flags        = 0;
    sc.command_bits = 8;                   /* FROZEN HD params — match the hub  */
    sc.address_bits = 8;
    sc.dummy_bits   = 8;
    sc.queue_size   = 4;
    sc.dma_chan     = SPI_DMA_CH_AUTO;
    /* Callbacks left NULL: we poll via get_trans_res in link_task (simpler and
     * keeps all decode out of ISR). */

    esp_err_t e = spi_slave_hd_init(XS3_SPI_HOST, &bus, &sc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spi_slave_hd_init failed: %s", esp_err_to_name(e));
        return false;
    }

    if (xTaskCreate(link_task, "fh_xs3_link", 4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "link task create failed");
        return false;
    }
    ESP_LOGI(TAG, "Bus-A SPI-Slave-HD link up (quad-capable, IRQ GPIO%d)",
             FH_XS3_IRQ_OUT);
    return true;
}

/* ============================================================================
 * KNOWN RISKS (UNVERIFIED — SPI Slave HD, the quad transport)
 * ----------------------------------------------------------------------------
 * L1  HD/ESSL PAIRING. The hub must drive this with the ESSL HD command set
 *     (Rd_DMA to pull our TX, Wr_DMA to push control), with command_bits=8,
 *     address_bits=8, dummy_bits=8 matching sc above. If the hub issues plain
 *     full-duplex transactions instead, nothing moves. (Hub side rewritten to
 *     ESSL in the same pass — see fh_hub_link.c.)
 * L2  QIO DUMMY CYCLES (Espressif issue #9823) — THE #1 bench item. Bring up
 *     1-bit HD first (prove ESSL + DMA), THEN widen the master's command to QIO
 *     and align dummy_bits on both sides. No slave code change to go QIO.
 * L3  RX DMA LENGTH must be 4-byte aligned (XS3_RX_DMA_LEN = 264); the HD driver
 *     truncates a non-multiple-of-4 RX. trans_len reports the real byte count.
 * L4  QUAD PINS. QUADWP=DQ2, QUADHD=DQ3 are wired (FH_XS3_SPI_WP/_HD) so the
 *     master can select QIO once the dummy cycles are aligned (L2). For 1-bit
 *     HD bring-up they are unused; harmless to have wired.
 * L5  CS/IRQ on GPIO43/44 are UART0 pins (carried ABYSS-S3-1 warning, see
 *     pins.h): confirm boot-time UART0 activity does not glitch CS/IRQ.
 * L6  MULTIDROP: HD slave releases its data lines when CS is high; confirm two
 *     peers never both drive DQ (bench).
 * ========================================================================== */
