/*
 * sx_shim.cpp — extern "C" bridge so the C main.c can drive the C++ RadioLib
 * SX1262 via RadioLib's official Pico-SDK HAL (PicoHal). The radio runs on
 * spi1 (master); the hub-link pump (main.c) runs on spi0. This is the LoRa
 * analogue of the cc1101 cc_shim.cpp: reuse the vetted upstream library, write
 * only the thin extern-"C" glue + the fh_ondemand verb mapping.
 *
 * RadioLib provenance: jgromes/RadioLib, MIT. PicoHal is RadioLib's own
 * in-tree Pico-SDK HAL (src/hal/RPiPico/PicoHal.{h,cpp}). We construct it on
 * spi1 and let the proven RadioLib SX1262 begin()/transmit()/startReceive()/
 * readData()/getRSSI()/getSNR() logic run unchanged — the RF bring-up is NOT
 * re-implemented from the datasheet.
 *
 * ── IRQ / CORE RECONCILIATION (the one hard design call, documented) ────────
 * PicoHal::attachInterrupt installs a SHARED IO_IRQ_BANK0 raw handler (via
 * gpio_add_raw_irq_handler_masked64) for RadioLib's DIO1 line. The RP2040 does
 * NOT allow that to coexist with gpio_set_irq_enabled_with_callback (the API
 * the GPS pump used for CS edges) — they fight over the same bank IRQ slot.
 * RESOLUTION: this shim (RadioLib + PicoHal, incl. its DIO1 bank IRQ) runs
 * ENTIRELY on core1; the hub pump on core0 drives CS by POLLING (no bank IRQ
 * registered on core0 at all), leaving IO_IRQ_BANK0 solely to PicoHal. See
 * main.c (core0 polls CS) and KNOWN RISKS R-IRQBANK.
 * LANDMINE: PicoHal::noTone() calls multicore_reset_core1(). This blade NEVER
 * calls tone()/noTone(); do not add them — it would reset the radio core.
 */
#if !defined(RADIOLIB_BUILD_RPI_PICO)
#  error "Build with -DRADIOLIB_BUILD_RPI_PICO (see CMakeLists)"
#endif

#include <RadioLib.h>
#include "hal/RPiPico/PicoHal.h"

/* RadioLib API PIN GUARD. This shim calls the POSITIONAL SX1262::begin(freq,
 * bw, sf, cr, sync, power, preamble, tcxo, ldo). That overload exists in
 * RadioLib 7.x (DEPRECATED but present as of 7.8.1) and was REMOVED on master
 * in favour of begin(const ConfigLoRa_t&). The project pins 7.8.1 (see the
 * arduino blade + CMake). If you build against a newer RadioLib, this call
 * will not compile — port it to the ConfigLoRa_t designated-initializer form
 * (radio.begin({.freq=..., .bw=..., ...})) rather than guessing. The guard
 * below turns a silent API drift into a clear error where RADIOLIB_VERSION is
 * exposed. */
#if defined(RADIOLIB_VERSION_MAJOR) && (RADIOLIB_VERSION_MAJOR > 7)
#  error "This shim targets RadioLib 7.x positional begin(); port to ConfigLoRa_t for 8.x+ (see sx_shim.cpp note)."
#endif

#include "fh_pico_pins_sdk.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

/* RadioLib runs on spi1. PicoHal(spi, miso, mosi, sck, speed). */
static PicoHal* s_hal = new PicoHal(spi1, FH_SX_SPI_MISO, FH_SX_SPI_MOSI,
                                    FH_SX_SPI_SCK, 2 * 1000 * 1000);

/* Module(hal, cs, irq=DIO1, rst, gpio=BUSY) — the SX1262 four-pin order
 * verified against the arduino blade (NSS, DIO1, RST, BUSY). */
static SX1262 s_radio = new Module(s_hal, FH_SX_NSS, FH_SX_DIO1,
                                   FH_SX_RST, FH_SX_BUSY);

static bool  s_ok   = false;
static float s_freq = 915.0f;      /* North America ISM band (committed) */
static bool  s_listening = false;

/* RF switch: the Wio board's host-controlled antenna switch. Sense VERIFY on
 * your module rev (gen1 idled LOW; HIGH = TX path here). */
static inline void rfsw_tx(void) { gpio_put(FH_SX_RFSW1, 1); }
static inline void rfsw_rx(void) { gpio_put(FH_SX_RFSW1, 0); }

extern "C" {

/* Bring up the SX1262 on spi1. Returns true if begin() succeeded. Mirrors the
 * arduino blade's radioInit(): explicit begin() with TCXO 1.6 V (enables the
 * Wio module's TCXO on DIO3) + the standard SF/BW/CR/sync/power/preamble. */
bool sx_init(float mhz) {
    s_freq = mhz;
    /* PicoHal.init() runs spi_init(spi1)+format; RadioLib calls it in begin(). */
    gpio_init(FH_SX_RFSW1); gpio_set_dir(FH_SX_RFSW1, GPIO_OUT); rfsw_rx();
    int16_t st = s_radio.begin(s_freq, 125.0, 9, 7,
                               RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                               10, 8, 1.6, false);
    s_ok = (st == RADIOLIB_ERR_NONE);
    return s_ok;
}

bool sx_present(void) { return s_ok; }

/* Transmit up to FH_OD_LORA_PAYLOAD_MAX (200) bytes. Returns time-on-air ms
 * (0 on fail). Blocking transmit() — fine, it runs on core1, off the hub pump. */
int sx_tx(const uint8_t *data, int len, int *out_status) {
    if (!s_ok) { if (out_status) *out_status = 0; return 0; }
    rfsw_tx();
    uint32_t t0 = s_hal->millis();
    int16_t st = s_radio.transmit((uint8_t*)data, (size_t)len);
    uint32_t toa = s_hal->millis() - t0;
    rfsw_rx();
    if (s_listening) { s_radio.startReceive(); }   /* resume the RX window */
    if (out_status) *out_status = (st == RADIOLIB_ERR_NONE) ? 1 : 0;
    return (int)toa;
}

/* Enter/leave the RX listen window. enable!=0 -> startReceive; else standby. */
int sx_listen(int enable) {
    if (!s_ok) return 0;
    if (enable) { rfsw_rx(); s_radio.startReceive(); s_listening = true; }
    else        { s_radio.standby();                 s_listening = false; }
    return s_listening ? 1 : 0;
}

/* Non-blocking poll for a received frame. Returns payload length (>0) and
 * fills data/rssi/snr when a packet is ready; 0 when nothing is buffered.
 * Gates on checkIrqFlags/available BEFORE readData (the S5 fix from the
 * arduino blade — never a blind readData() poll). */
int sx_rx_poll(uint8_t *data, int cap, int *rssi, int *snr) {
    if (!s_ok || !s_listening) return 0;
    /* RadioLib: getPacketLength(update=false) is 0 until a packet lands. The
     * DIO1 IRQ (PicoHal bank handler) drives RadioLib's internal flag; a poll
     * of getPacketLength reflects it without blocking. */
    size_t pkt = s_radio.getPacketLength();
    if (pkt == 0) return 0;
    if ((int)pkt > cap) pkt = (size_t)cap;
    /* readData() returns a STATUS CODE (RADIOLIB_ERR_NONE on success), not a
     * length — the length came from getPacketLength() above. */
    int16_t st = s_radio.readData(data, pkt);
    if (st != RADIOLIB_ERR_NONE) { s_radio.startReceive(); return 0; }
    if (rssi) *rssi = (int)s_radio.getRSSI();
    if (snr)  *snr  = (int)s_radio.getSNR();
    s_radio.startReceive();                 /* re-arm the window */
    return (int)pkt;
}

} /* extern "C" */
