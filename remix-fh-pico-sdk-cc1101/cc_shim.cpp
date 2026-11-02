/*
 * cc_shim.cpp — extern "C" bridge so the C main.c can drive the C++ ELECHOUSE
 * CC1101 driver (spi1 master). Also carries the OOK transmit bit-bang
 * (transmitCode) from the arduino blade, adapted to SDK GPIO/timing.
 *
 * The CC1101 async-OOK model: data rides GDO0. RX = capture GDO0 edges (done in
 * main.c's IRQ). TX = drive GDO0 as an output and bit-bang the protocol timing
 * while the radio is in TX. setCCMode(0) + setModulation(2) = async/raw OOK.
 */
#include "ELECHOUSE_CC1101_SRC_DRV.h"
#include "fh_pico_pins_sdk.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

/* The RCSwitch-style protocol table (same values as main.c's copy; TX needs the
 * timing to re-emit). Kept local to the shim to avoid a cross-TU dependency. */
struct RCProto { uint16_t pulseLength; uint8_t syncHigh, syncLow, zeroHigh, zeroLow, oneHigh, oneLow; bool inverted; };
static const RCProto RCP[] = {
    {   0,  0,  0, 0,  0, 0,  0, false },
    { 350,  1, 31, 1,  3, 3,  1, false },
    { 650,  1, 10, 1,  2, 2,  1, false },
    { 100, 30, 71, 4, 11, 9,  6, false },
    { 380,  1,  6, 1,  3, 3,  1, false },
    { 500,  6, 14, 1,  2, 2,  1, false },
    { 450, 23,  1, 1,  2, 2,  1, true  },
    { 150,  2, 62, 1,  6, 6,  1, false },
    { 200,  3,130, 7, 16, 3, 16, false },
    { 200,130,  7,16,  7,16,  3, true  },
    { 365, 18,  1, 3,  1, 1,  3, true  },
    { 270, 36,  1, 1,  2, 2,  1, true  },
    { 320, 36,  1, 1,  2, 2,  1, true  },
};
static const int NRCP = (int)(sizeof(RCP)/sizeof(RCP[0]));

extern "C" {

bool cc_present(void) {
    /* set our spi1 pins + GDO lines, then probe. getCC1101() is guarded (our
     * FH-EDIT 2) so these custom pins survive. */
    ELECHOUSE_cc1101.setSpiPin(FH_CC_SPI_SCK, FH_CC_SPI_MISO, FH_CC_SPI_MOSI, FH_CC_SPI_CSN);
    ELECHOUSE_cc1101.setGDO(FH_CC_GDO0, FH_CC_GDO2);
    return ELECHOUSE_cc1101.getCC1101();
}

void cc_init(float mhz) {
    ELECHOUSE_cc1101.setSpiPin(FH_CC_SPI_SCK, FH_CC_SPI_MISO, FH_CC_SPI_MOSI, FH_CC_SPI_CSN);
    ELECHOUSE_cc1101.setGDO(FH_CC_GDO0, FH_CC_GDO2);
    if (!ELECHOUSE_cc1101.getCC1101()) return;
    ELECHOUSE_cc1101.Init();
    ELECHOUSE_cc1101.setCCMode(0);       /* async/raw serial (data on GDO0) */
    ELECHOUSE_cc1101.setModulation(2);   /* ASK/OOK */
    ELECHOUSE_cc1101.setMHZ(mhz);
    ELECHOUSE_cc1101.SetRx();
}

void cc_enter_rx(void) {
    ELECHOUSE_cc1101.setSidle();
    ELECHOUSE_cc1101.SetRx();
    gpio_set_dir(FH_CC_GDO0, GPIO_IN);   /* capture IRQ reads GDO0 */
}

int cc_rssi(void) { return ELECHOUSE_cc1101.getRssi(); }

void cc_set_freq(float mhz) {
    ELECHOUSE_cc1101.setSidle();
    ELECHOUSE_cc1101.setMHZ(mhz);
    ELECHOUSE_cc1101.SetRx();
    gpio_set_dir(FH_CC_GDO0, GPIO_IN);
}

/* OOK transmit: drive GDO0 as output, bit-bang the protocol timing in TX, then
 * hand GDO0 back to RX. Carried from the arduino blade's transmitCode(). */
void cc_tx_raw(unsigned long value, int bits, int proto, int repeats) {
    if (proto < 1 || proto >= NRCP) return;
    const RCProto *pr = &RCP[proto];
    if (pr->pulseLength == 0 || bits <= 0 || bits > 32) return;

    const int hiLevel = pr->inverted ? 0 : 1;
    const int loLevel = pr->inverted ? 1 : 0;

    ELECHOUSE_cc1101.setSidle();
    gpio_set_dir(FH_CC_GDO0, GPIO_OUT);
    gpio_put(FH_CC_GDO0, loLevel);
    ELECHOUSE_cc1101.setPA(12);
    ELECHOUSE_cc1101.SetTx();

    for (int r = 0; r < repeats; r++) {
        for (int b = bits - 1; b >= 0; b--) {
            bool one = (value >> b) & 1UL;
            uint16_t hT = one ? pr->oneHigh : pr->zeroHigh;
            uint16_t lT = one ? pr->oneLow  : pr->zeroLow;
            gpio_put(FH_CC_GDO0, hiLevel);
            sleep_us((uint32_t)pr->pulseLength * hT);
            gpio_put(FH_CC_GDO0, loLevel);
            sleep_us((uint32_t)pr->pulseLength * lT);
        }
        gpio_put(FH_CC_GDO0, hiLevel);
        sleep_us((uint32_t)pr->pulseLength * pr->syncHigh);
        gpio_put(FH_CC_GDO0, loLevel);
        sleep_us((uint32_t)pr->pulseLength * pr->syncLow);
    }
    gpio_put(FH_CC_GDO0, loLevel);

    ELECHOUSE_cc1101.setSidle();
    ELECHOUSE_cc1101.SetRx();
    gpio_set_dir(FH_CC_GDO0, GPIO_IN);   /* back to capture */
}

} /* extern "C" */
