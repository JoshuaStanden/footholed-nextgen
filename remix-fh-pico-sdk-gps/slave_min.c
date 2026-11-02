/* slave_min.c — BARE-METAL SPI slave isolation test (NOT the blade firmware).
 *
 * Question: does a stripped-down SDK SPI slave stream >1 byte per transaction?
 *   - NO pump, NO ISR, NO dual-core, NO GPS, NO decoder.
 *   - Static tx buffer: 55 AA 55 AA 01 02 03 ... 08 08 08 ... (261 bytes).
 *   - Procedure: init spi0 slave @ 1 MHz, preload 8 bytes, then just SIT in
 *     a busy loop. LED solid = running. Every CS fall: reset the SSP (SSE
 *     off -> drain -> SSE on) and re-preload 8 bytes, then busy-wait.
 * The hub's BENCH dump is the verdict:
 *   [55 aa 55 aa 01 02 03 ...] -> slave hardware streams fine; bug is in main.c
 *   [55 aa then 00/ff]         -> SSP slave needs the datasheet workaround
 */
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#define SPI_PORT spi0

int main(void) {
    const uint LED = 25;   /* Pico default LED */
    gpio_init(LED);
    gpio_set_dir(LED, GPIO_OUT);
    gpio_put(LED, true);   /* solid = alive */

    /* 1 MHz is slower than the hub's 8 MHz, but the hub's bench dump works
     * at any speed >= its own? NO — the master dictates the clock. The hub
     * clocks at 1 MHz (FH_BUSB_CLK_HZ is 1 MHz for bring-up), so a slave
     * doesn't set a speed that matters; spi_init only arms the SSP. */
    spi_init(SPI_PORT, 1000000);
    spi_set_slave(SPI_PORT, true);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    /* GP18 SCK / GP16 MOSI(hub->blade) / GP19 MISO(blade->hub) / GP21 CS */
    gpio_set_function(18, GPIO_FUNC_SPI);
    gpio_set_function(19, GPIO_FUNC_SPI);
    gpio_set_function(16, GPIO_FUNC_SPI);
    gpio_set_function(21, GPIO_FUNC_SPI);

    static uint8_t tx[261];
    tx[0] = 0x55; tx[1] = 0xAA; tx[2] = 0x55; tx[3] = 0xAA;
    for (int i = 4; i < 261; ++i) { tx[i] = (uint8_t)(i & 0xFF); }
    if (tx[8] == 8 && tx[9] == 9) { /* silence unused-result lint */ }

    spi_hw_t *hw = spi_get_hw(SPI_PORT);

    /* Preload 8 bytes for the very first transaction. */
    for (int i = 0; i < 8 && spi_is_writable(SPI_PORT); ++i) { hw->dr = tx[i]; }

    /* Busy loop: poll the CS pin directly (NO ISR). On each CS RISE: flush RX
     * and re-preload 8 bytes. While CS is low: drain every RX byte as it
     * clocks in. NO paired-push pump — the FIFO drains purely from the
     * preload (8 bytes) then underruns. So:
     *   hub sees [55 aa 55 aa 01 02 03 04 <pad>]  = preload WORKS (8 bytes!)
     *   hub sees [55 <pad>]                        = preload broken (1 byte)
     * Since main.c's paired-push pump is proven to cycle (counter increments),
     * the preload's survival is the ONLY open variable. */
    bool last_cs = true;   /* CS idle high */
    for (;;) {
        bool cs = (gpio_get(21) != 0);
        if (cs && !last_cs) {
            /* CS just ROSE: transaction over. Flush RX, re-preload 8 bytes. */
            while (spi_is_readable(SPI_PORT)) { (void)hw->dr; }
            for (int i = 0; i < 8 && spi_is_writable(SPI_PORT); ++i) {
                hw->dr = tx[i];
            }
        }
        last_cs = cs;

        /* Drain RX continuously (proves full-duplex RX works too). */
        while (spi_is_readable(SPI_PORT)) { (void)hw->dr; }
    }
}
