/* ELECHOUSE_CC1101_SRC_DRV.cpp — CC1101 driver (SmartRC / ELECHOUSE), Pico-SDK.
 *
 * VENDORED + VETTED 2026-11-01 from sentoxo/SmartRC-CC1101-Driver-Lib-Pico
 * (fork of LSatan/SmartRC-CC1101-Driver-Lib, MIT; orig. (c) 2010 ELECHOUSE).
 * Retain this notice on redistribution.
 *
 * TWO LOCAL FOOTHOLED EDITS vs the vetted upstream (search "FH-EDIT"):
 *   FH-EDIT 1: #define SPI_BUS spi1  (upstream: spi0). The radio runs on spi1
 *              because the hub-link pump owns spi0 (bench-proven GPS config).
 *   FH-EDIT 2: getCC1101() no longer calls setSpi() when custom pins were set
 *              via setSpiPin() (the `spi` flag). Upstream's getCC1101() always
 *              re-ran setSpi(), clobbering custom pins back to SS=GP2. The blade
 *              calls setSpiPin(..., CSN=GP13) then getCC1101(); without this
 *              guard the custom CSN was lost. Everything else is upstream.
 */
#include "ELECHOUSE_CC1101_SRC_DRV.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"
#include "stdlib.h"
#include "string.h"

#define WRITE_BURST     0x40
#define READ_SINGLE     0x80
#define READ_BURST      0xC0
#define BYTES_IN_RXFIFO 0x7F
#define max_modul       6
#define byte uint8_t

#define SPI_BUS spi1    /* FH-EDIT 1: radio on spi1 (hub pump owns spi0) */

byte modulation = 2;
byte frend0;
byte chan = 0;
int  pa = 12;
byte last_pa;
byte SCK_PIN;
byte MISO_PIN;
byte MOSI_PIN;
byte SS_PIN;
byte GDO0;
byte GDO2;
byte SCK_PIN_M[max_modul];
byte MISO_PIN_M[max_modul];
byte MOSI_PIN_M[max_modul];
byte SS_PIN_M[max_modul];
byte GDO0_M[max_modul];
byte GDO2_M[max_modul];
byte gdo_set = 0;
bool spi = 0;
bool ccmode = 0;
float MHz = 433.92;
byte m4RxBw = 0;
byte m4DaRa;
byte m2DCOFF;
byte m2MODFM;
byte m2MANCH;
byte m2SYNCM;
byte m1FEC;
byte m1PRE;
byte m1CHSP;
byte pc1PQT;
byte pc1CRC_AF;
byte pc1APP_ST;
byte pc1ADRCHK;
byte pc0WDATA;
byte pc0PktForm;
byte pc0CRC_EN;
byte pc0LenConf;
byte trxstate = 0;
byte clb1[2] = {24, 28};
byte clb2[2] = {31, 38};
byte clb3[2] = {65, 76};
byte clb4[2] = {77, 79};

uint8_t PA_TABLE[8]{0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
uint8_t PA_TABLE_315[8]{0x12, 0x0D, 0x1C, 0x34, 0x51, 0x85, 0xCB, 0xC2};
uint8_t PA_TABLE_433[8]{0x12, 0x0E, 0x1D, 0x34, 0x60, 0x84, 0xC8, 0xC0};
uint8_t PA_TABLE_868[10]{0x03, 0x17, 0x1D, 0x26, 0x37, 0x50, 0x86, 0xCD, 0xC5, 0xC0};
uint8_t PA_TABLE_915[10]{0x03, 0x0E, 0x1E, 0x27, 0x38, 0x8E, 0x84, 0xCC, 0xC3, 0xC0};

void ELECHOUSE_CC1101::SpiStart(void) {
    gpio_set_function(SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(MISO_PIN, GPIO_FUNC_SPI);
    gpio_set_dir(SS_PIN, GPIO_OUT);
    spi_init(SPI_BUS, 1000 * 1000 * 4);
    spi_set_format(SPI_BUS, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

void ELECHOUSE_CC1101::SpiEnd(void) {
    spi_deinit(SPI_BUS);
}

void ELECHOUSE_CC1101::GDO_Set(void) {
    gpio_set_dir(GDO0, GPIO_OUT);
    gpio_set_dir(GDO2, GPIO_IN);
}

void ELECHOUSE_CC1101::GDO0_Set(void) {
    gpio_set_dir(GDO0, GPIO_IN);
}

void ELECHOUSE_CC1101::Reset(void) {
    gpio_put(SS_PIN, 0);
    sleep_ms(1);
    gpio_put(SS_PIN, 1);
    sleep_ms(1);
    gpio_put(SS_PIN, 0);
    while (gpio_get(MISO_PIN));
    uint8_t command = CC1101_SRES;
    spi_write_blocking(SPI_BUS, &command, 1);
    while (gpio_get(MISO_PIN));
    gpio_put(SS_PIN, 1);
}

void ELECHOUSE_CC1101::Init(void) {
    setSpi();
    SpiStart();
    gpio_put(SS_PIN, 1);
    gpio_put(SCK_PIN, 1);
    gpio_put(MOSI_PIN, 0);
    Reset();
    RegConfigSettings();
    SpiEnd();
}

void ELECHOUSE_CC1101::SpiWriteReg(byte addr, byte value) {
    SpiStart();
    gpio_put(SS_PIN, 0);
    while (gpio_get(MISO_PIN));
    spi_write_blocking(SPI_BUS, &addr, 1);
    spi_write_blocking(SPI_BUS, &value, 1);
    gpio_put(SS_PIN, 1);
    SpiEnd();
}

void ELECHOUSE_CC1101::SpiWriteBurstReg(byte addr, byte *buffer, byte num) {
    byte temp = addr | WRITE_BURST;
    SpiStart();
    gpio_put(SS_PIN, 0);
    while (gpio_get(MISO_PIN));
    spi_write_blocking(SPI_BUS, &temp, 1);
    spi_write_blocking(SPI_BUS, buffer, num);
    gpio_put(SS_PIN, 1);
    SpiEnd();
}

void ELECHOUSE_CC1101::SpiStrobe(byte strobe) {
    SpiStart();
    gpio_put(SS_PIN, 0);
    while (gpio_get(MISO_PIN));
    spi_write_blocking(SPI_BUS, &strobe, 1);
    gpio_put(SS_PIN, 1);
    SpiEnd();
}

byte ELECHOUSE_CC1101::SpiReadReg(byte addr) {
    byte temp = addr | READ_SINGLE, value;
    SpiStart();
    gpio_put(SS_PIN, 0);
    while (gpio_get(MISO_PIN));
    spi_write_blocking(SPI_BUS, &temp, 1);
    uint8_t dummy = 0;
    spi_write_read_blocking(SPI_BUS, &dummy, &value, 1);
    gpio_put(SS_PIN, 1);
    SpiEnd();
    return value;
}

void ELECHOUSE_CC1101::SpiReadBurstReg(byte addr, byte *buffer, byte num) {
    byte temp = addr | READ_BURST;
    SpiStart();
    gpio_put(SS_PIN, 0);
    while (gpio_get(MISO_PIN));
    spi_write_blocking(SPI_BUS, &temp, 1);
    uint8_t dummy[num] = {0};
    spi_write_read_blocking(SPI_BUS, dummy, buffer, num);
    gpio_put(SS_PIN, 1);
    SpiEnd();
}

byte ELECHOUSE_CC1101::SpiReadStatus(byte addr) {
    byte value, temp = addr | READ_BURST;
    SpiStart();
    gpio_put(SS_PIN, 0);
    while (gpio_get(MISO_PIN));
    spi_write_blocking(SPI_BUS, &temp, 1);
    uint8_t dummy = 0;
    spi_write_read_blocking(SPI_BUS, &dummy, &value, 1);
    gpio_put(SS_PIN, 1);
    SpiEnd();
    return value;
}

void ELECHOUSE_CC1101::setSpi(void) {
    if (spi == 0) {
        SCK_PIN = 18; MISO_PIN = 16; MOSI_PIN = 19; SS_PIN = 2;
    }
}

void ELECHOUSE_CC1101::setSpiPin(byte sck, byte miso, byte mosi, byte ss) {
    spi = 1;
    SCK_PIN = sck; MISO_PIN = miso; MOSI_PIN = mosi; SS_PIN = ss;
}

void ELECHOUSE_CC1101::addSpiPin(byte sck, byte miso, byte mosi, byte ss, byte modul) {
    spi = 1;
    SCK_PIN_M[modul] = sck; MISO_PIN_M[modul] = miso;
    MOSI_PIN_M[modul] = mosi; SS_PIN_M[modul] = ss;
}

void ELECHOUSE_CC1101::setGDO(byte gdo0, byte gdo2) {
    GDO0 = gdo0; GDO2 = gdo2; GDO_Set();
}

void ELECHOUSE_CC1101::setGDO0(byte gdo0) {
    GDO0 = gdo0; GDO0_Set();
}

void ELECHOUSE_CC1101::addGDO(byte gdo0, byte gdo2, byte modul) {
    GDO0_M[modul] = gdo0; GDO2_M[modul] = gdo2; gdo_set = 2; GDO_Set();
}

void ELECHOUSE_CC1101::addGDO0(byte gdo0, byte modul) {
    GDO0_M[modul] = gdo0; gdo_set = 1; GDO0_Set();
}

void ELECHOUSE_CC1101::setModul(byte modul) {
    SCK_PIN = SCK_PIN_M[modul]; MISO_PIN = MISO_PIN_M[modul];
    MOSI_PIN = MOSI_PIN_M[modul]; SS_PIN = SS_PIN_M[modul];
    if (gdo_set == 1) { GDO0 = GDO0_M[modul]; }
    else if (gdo_set == 2) { GDO0 = GDO0_M[modul]; GDO2 = GDO2_M[modul]; }
}

void ELECHOUSE_CC1101::setCCMode(bool s) {
    ccmode = s;
    if (ccmode == 1) {
        SpiWriteReg(CC1101_IOCFG2, 0x0B);
        SpiWriteReg(CC1101_IOCFG0, 0x06);
        SpiWriteReg(CC1101_PKTCTRL0, 0x05);
        SpiWriteReg(CC1101_MDMCFG3, 0xF8);
        SpiWriteReg(CC1101_MDMCFG4, 11 + m4RxBw);
    } else {
        SpiWriteReg(CC1101_IOCFG2, 0x0D);
        SpiWriteReg(CC1101_IOCFG0, 0x0D);
        SpiWriteReg(CC1101_PKTCTRL0, 0x32);
        SpiWriteReg(CC1101_MDMCFG3, 0x93);
        SpiWriteReg(CC1101_MDMCFG4, 7 + m4RxBw);
    }
    setModulation(modulation);
}

void ELECHOUSE_CC1101::setModulation(byte m) {
    if (m > 4) { m = 4; }
    modulation = m;
    Split_MDMCFG2();
    switch (m) {
    case 0: m2MODFM = 0x00; frend0 = 0x10; break;
    case 1: m2MODFM = 0x10; frend0 = 0x10; break;
    case 2: m2MODFM = 0x30; frend0 = 0x11; break;
    case 3: m2MODFM = 0x40; frend0 = 0x10; break;
    case 4: m2MODFM = 0x70; frend0 = 0x10; break;
    }
    SpiWriteReg(CC1101_MDMCFG2, m2DCOFF + m2MODFM + m2MANCH + m2SYNCM);
    SpiWriteReg(CC1101_FREND0, frend0);
    setPA(pa);
}

void ELECHOUSE_CC1101::setPA(int p) {
    int a = 0;
    pa = p;
    if (MHz >= 300 && MHz <= 348) {
        if (pa <= -30) { a = PA_TABLE_315[0]; }
        else if (pa <= -20) { a = PA_TABLE_315[1]; }
        else if (pa <= -15) { a = PA_TABLE_315[2]; }
        else if (pa <= -10) { a = PA_TABLE_315[3]; }
        else if (pa <= 0)   { a = PA_TABLE_315[4]; }
        else if (pa <= 5)   { a = PA_TABLE_315[5]; }
        else if (pa <= 7)   { a = PA_TABLE_315[6]; }
        else                { a = PA_TABLE_315[7]; }
        last_pa = 1;
    } else if (MHz >= 378 && MHz <= 464) {
        if (pa <= -30) { a = PA_TABLE_433[0]; }
        else if (pa <= -20) { a = PA_TABLE_433[1]; }
        else if (pa <= -15) { a = PA_TABLE_433[2]; }
        else if (pa <= -10) { a = PA_TABLE_433[3]; }
        else if (pa <= 0)   { a = PA_TABLE_433[4]; }
        else if (pa <= 5)   { a = PA_TABLE_433[5]; }
        else if (pa <= 7)   { a = PA_TABLE_433[6]; }
        else                { a = PA_TABLE_433[7]; }
        last_pa = 2;
    } else if (MHz >= 779 && MHz <= 899.99) {
        if (pa <= -30) { a = PA_TABLE_868[0]; }
        else if (pa <= -20) { a = PA_TABLE_868[1]; }
        else if (pa <= -15) { a = PA_TABLE_868[2]; }
        else if (pa <= -10) { a = PA_TABLE_868[3]; }
        else if (pa <= -6)  { a = PA_TABLE_868[4]; }
        else if (pa <= 0)   { a = PA_TABLE_868[5]; }
        else if (pa <= 5)   { a = PA_TABLE_868[6]; }
        else if (pa <= 7)   { a = PA_TABLE_868[7]; }
        else if (pa <= 10)  { a = PA_TABLE_868[8]; }
        else                { a = PA_TABLE_868[9]; }
        last_pa = 3;
    } else if (MHz >= 900 && MHz <= 928) {
        if (pa <= -30) { a = PA_TABLE_915[0]; }
        else if (pa <= -20) { a = PA_TABLE_915[1]; }
        else if (pa <= -15) { a = PA_TABLE_915[2]; }
        else if (pa <= -10) { a = PA_TABLE_915[3]; }
        else if (pa <= -6)  { a = PA_TABLE_915[4]; }
        else if (pa <= 0)   { a = PA_TABLE_915[5]; }
        else if (pa <= 5)   { a = PA_TABLE_915[6]; }
        else if (pa <= 7)   { a = PA_TABLE_915[7]; }
        else if (pa <= 10)  { a = PA_TABLE_915[8]; }
        else                { a = PA_TABLE_915[9]; }
        last_pa = 4;
    }
    if (modulation == 2) { PA_TABLE[0] = 0; PA_TABLE[1] = a; }
    else                 { PA_TABLE[0] = a; PA_TABLE[1] = 0; }
    SpiWriteBurstReg(CC1101_PATABLE, PA_TABLE, 8);
}

void ELECHOUSE_CC1101::setMHZ(float mhz) {
    byte freq2 = 0, freq1 = 0, freq0 = 0;
    MHz = mhz;
    for (bool i = 0; i == 0;) {
        if (mhz >= 26) { mhz -= 26; freq2 += 1; }
        else if (mhz >= 0.1015625) { mhz -= 0.1015625; freq1 += 1; }
        else if (mhz >= 0.00039675) { mhz -= 0.00039675; freq0 += 1; }
        else { i = 1; }
    }
    if (freq0 > 255) { freq1 += 1; freq0 -= 256; }
    SpiWriteReg(CC1101_FREQ2, freq2);
    SpiWriteReg(CC1101_FREQ1, freq1);
    SpiWriteReg(CC1101_FREQ0, freq0);
    Calibrate();
}

void ELECHOUSE_CC1101::Calibrate(void) {
    if (MHz >= 300 && MHz <= 348) {
        SpiWriteReg(CC1101_FSCTRL0, map(MHz, 300, 348, clb1[0], clb1[1]));
        if (MHz < 322.88) { SpiWriteReg(CC1101_TEST0, 0x0B); }
        else {
            SpiWriteReg(CC1101_TEST0, 0x09);
            int s = ELECHOUSE_cc1101.SpiReadStatus(CC1101_FSCAL2);
            if (s < 32) { SpiWriteReg(CC1101_FSCAL2, s + 32); }
            if (last_pa != 1) { setPA(pa); }
        }
    } else if (MHz >= 378 && MHz <= 464) {
        SpiWriteReg(CC1101_FSCTRL0, map(MHz, 378, 464, clb2[0], clb2[1]));
        if (MHz < 430.5) { SpiWriteReg(CC1101_TEST0, 0x0B); }
        else {
            SpiWriteReg(CC1101_TEST0, 0x09);
            int s = ELECHOUSE_cc1101.SpiReadStatus(CC1101_FSCAL2);
            if (s < 32) { SpiWriteReg(CC1101_FSCAL2, s + 32); }
            if (last_pa != 2) { setPA(pa); }
        }
    } else if (MHz >= 779 && MHz <= 899.99) {
        SpiWriteReg(CC1101_FSCTRL0, map(MHz, 779, 899, clb3[0], clb3[1]));
        if (MHz < 861) { SpiWriteReg(CC1101_TEST0, 0x0B); }
        else {
            SpiWriteReg(CC1101_TEST0, 0x09);
            int s = ELECHOUSE_cc1101.SpiReadStatus(CC1101_FSCAL2);
            if (s < 32) { SpiWriteReg(CC1101_FSCAL2, s + 32); }
            if (last_pa != 3) { setPA(pa); }
        }
    } else if (MHz >= 900 && MHz <= 928) {
        SpiWriteReg(CC1101_FSCTRL0, map(MHz, 900, 928, clb4[0], clb4[1]));
        SpiWriteReg(CC1101_TEST0, 0x09);
        int s = ELECHOUSE_cc1101.SpiReadStatus(CC1101_FSCAL2);
        if (s < 32) { SpiWriteReg(CC1101_FSCAL2, s + 32); }
        if (last_pa != 4) { setPA(pa); }
    }
}

void ELECHOUSE_CC1101::setClb(byte b, byte s, byte e) {
    if (b == 1) { clb1[0] = s; clb1[1] = e; }
    else if (b == 2) { clb2[0] = s; clb2[1] = e; }
    else if (b == 3) { clb3[0] = s; clb3[1] = e; }
    else if (b == 4) { clb4[0] = s; clb4[1] = e; }
}

bool ELECHOUSE_CC1101::getCC1101(void) {
    /* FH-EDIT 2: upstream calls setSpi() here unconditionally, which clobbers
     * custom pins set via setSpiPin() back to the driver defaults (SS=GP2).
     * Guard it: only apply the defaults when NO custom pins were set. */
    if (spi == 0) { setSpi(); }
    if (SpiReadStatus(0x31) > 0) { return 1; }
    else { return 0; }
}

byte ELECHOUSE_CC1101::getMode(void) { return trxstate; }

void ELECHOUSE_CC1101::setSyncWord(byte sh, byte sl) {
    SpiWriteReg(CC1101_SYNC1, sh);
    SpiWriteReg(CC1101_SYNC0, sl);
}

void ELECHOUSE_CC1101::setAddr(byte v) { SpiWriteReg(CC1101_ADDR, v); }

void ELECHOUSE_CC1101::setPQT(byte v) {
    Split_PKTCTRL1(); pc1PQT = 0; if (v > 7) { v = 7; } pc1PQT = v * 32;
    SpiWriteReg(CC1101_PKTCTRL1, pc1PQT + pc1CRC_AF + pc1APP_ST + pc1ADRCHK);
}

void ELECHOUSE_CC1101::setCRC_AF(bool v) {
    Split_PKTCTRL1(); pc1CRC_AF = 0; if (v == 1) { pc1CRC_AF = 8; }
    SpiWriteReg(CC1101_PKTCTRL1, pc1PQT + pc1CRC_AF + pc1APP_ST + pc1ADRCHK);
}

void ELECHOUSE_CC1101::setAppendStatus(bool v) {
    Split_PKTCTRL1(); pc1APP_ST = 0; if (v == 1) { pc1APP_ST = 4; }
    SpiWriteReg(CC1101_PKTCTRL1, pc1PQT + pc1CRC_AF + pc1APP_ST + pc1ADRCHK);
}

void ELECHOUSE_CC1101::setAdrChk(byte v) {
    Split_PKTCTRL1(); pc1ADRCHK = 0; if (v > 3) { v = 3; } pc1ADRCHK = v;
    SpiWriteReg(CC1101_PKTCTRL1, pc1PQT + pc1CRC_AF + pc1APP_ST + pc1ADRCHK);
}

void ELECHOUSE_CC1101::setWhiteData(bool v) {
    Split_PKTCTRL0(); pc0WDATA = 0; if (v == 1) { pc0WDATA = 64; }
    SpiWriteReg(CC1101_PKTCTRL0, pc0WDATA + pc0PktForm + pc0CRC_EN + pc0LenConf);
}

void ELECHOUSE_CC1101::setPktFormat(byte v) {
    Split_PKTCTRL0(); pc0PktForm = 0; if (v > 3) { v = 3; } pc0PktForm = v * 16;
    SpiWriteReg(CC1101_PKTCTRL0, pc0WDATA + pc0PktForm + pc0CRC_EN + pc0LenConf);
}

void ELECHOUSE_CC1101::setCrc(bool v) {
    Split_PKTCTRL0(); pc0CRC_EN = 0; if (v == 1) { pc0CRC_EN = 4; }
    SpiWriteReg(CC1101_PKTCTRL0, pc0WDATA + pc0PktForm + pc0CRC_EN + pc0LenConf);
}

void ELECHOUSE_CC1101::setLengthConfig(byte v) {
    Split_PKTCTRL0(); pc0LenConf = 0; if (v > 3) { v = 3; } pc0LenConf = v;
    SpiWriteReg(CC1101_PKTCTRL0, pc0WDATA + pc0PktForm + pc0CRC_EN + pc0LenConf);
}

void ELECHOUSE_CC1101::setPacketLength(byte v) { SpiWriteReg(CC1101_PKTLEN, v); }

void ELECHOUSE_CC1101::setDcFilterOff(bool v) {
    Split_MDMCFG2(); m2DCOFF = 0; if (v == 1) { m2DCOFF = 128; }
    SpiWriteReg(CC1101_MDMCFG2, m2DCOFF + m2MODFM + m2MANCH + m2SYNCM);
}

void ELECHOUSE_CC1101::setManchester(bool v) {
    Split_MDMCFG2(); m2MANCH = 0; if (v == 1) { m2MANCH = 8; }
    SpiWriteReg(CC1101_MDMCFG2, m2DCOFF + m2MODFM + m2MANCH + m2SYNCM);
}

void ELECHOUSE_CC1101::setSyncMode(byte v) {
    Split_MDMCFG2(); m2SYNCM = 0; if (v > 7) { v = 7; } m2SYNCM = v;
    SpiWriteReg(CC1101_MDMCFG2, m2DCOFF + m2MODFM + m2MANCH + m2SYNCM);
}

void ELECHOUSE_CC1101::setFEC(bool v) {
    Split_MDMCFG1(); m1FEC = 0; if (v == 1) { m1FEC = 128; }
    SpiWriteReg(CC1101_MDMCFG1, m1FEC + m1PRE + m1CHSP);
}

void ELECHOUSE_CC1101::setPRE(byte v) {
    Split_MDMCFG1(); m1PRE = 0; if (v > 7) { v = 7; } m1PRE = v * 16;
    SpiWriteReg(CC1101_MDMCFG1, m1FEC + m1PRE + m1CHSP);
}

void ELECHOUSE_CC1101::setChannel(byte ch) { chan = ch; SpiWriteReg(CC1101_CHANNR, chan); }

void ELECHOUSE_CC1101::setChsp(float f) {
    Split_MDMCFG1();
    byte MDMCFG0 = 0; m1CHSP = 0;
    if (f > 405.456543) { f = 405.456543; }
    if (f < 25.390625) { f = 25.390625; }
    for (int i = 0; i < 5; i++) {
        if (f <= 50.682068) {
            f -= 25.390625; f /= 0.0991825; MDMCFG0 = f;
            float s1 = (f - MDMCFG0) * 10; if (s1 >= 5) { MDMCFG0++; }
            i = 5;
        } else { m1CHSP++; f /= 2; }
    }
    SpiWriteReg(19, m1CHSP + m1FEC + m1PRE);
    SpiWriteReg(20, MDMCFG0);
}

void ELECHOUSE_CC1101::setRxBW(float f) {
    Split_MDMCFG4();
    int s1 = 3, s2 = 3;
    for (int i = 0; i < 3; i++) { if (f > 101.5625) { f /= 2; s1--; } else { i = 3; } }
    for (int i = 0; i < 3; i++) { if (f > 58.1) { f /= 1.25; s2--; } else { i = 3; } }
    s1 *= 64; s2 *= 16; m4RxBw = s1 + s2;
    SpiWriteReg(16, m4RxBw + m4DaRa);
}

void ELECHOUSE_CC1101::setDRate(float d) {
    Split_MDMCFG4();
    float c = d;
    byte MDMCFG3 = 0;
    if (c > 1621.83) { c = 1621.83; }
    if (c < 0.0247955) { c = 0.0247955; }
    m4DaRa = 0;
    for (int i = 0; i < 20; i++) {
        if (c <= 0.0494942) {
            c = c - 0.0247955; c = c / 0.00009685; MDMCFG3 = c;
            float s1 = (c - MDMCFG3) * 10; if (s1 >= 5) { MDMCFG3++; }
            i = 20;
        } else { m4DaRa++; c = c / 2; }
    }
    SpiWriteReg(16, m4RxBw + m4DaRa);
    SpiWriteReg(17, MDMCFG3);
}

void ELECHOUSE_CC1101::setDeviation(float d) {
    float f = 1.586914, v = 0.19836425;
    int c = 0;
    if (d > 380.859375) { d = 380.859375; }
    if (d < 1.586914) { d = 1.586914; }
    for (int i = 0; i < 255; i++) {
        f += v;
        if (c == 7) { v *= 2; c = -1; i += 8; }
        if (f >= d) { c = i; i = 255; }
        c++;
    }
    SpiWriteReg(21, c);
}

void ELECHOUSE_CC1101::Split_PKTCTRL1(void) {
    int calc = SpiReadStatus(7);
    pc1PQT = 0; pc1CRC_AF = 0; pc1APP_ST = 0; pc1ADRCHK = 0;
    for (bool i = 0; i == 0;) {
        if (calc >= 32) { calc -= 32; pc1PQT += 32; }
        else if (calc >= 8) { calc -= 8; pc1CRC_AF += 8; }
        else if (calc >= 4) { calc -= 4; pc1APP_ST += 4; }
        else { pc1ADRCHK = calc; i = 1; }
    }
}

void ELECHOUSE_CC1101::Split_PKTCTRL0(void) {
    int calc = SpiReadStatus(8);
    pc0WDATA = 0; pc0PktForm = 0; pc0CRC_EN = 0; pc0LenConf = 0;
    for (bool i = 0; i == 0;) {
        if (calc >= 64) { calc -= 64; pc0WDATA += 64; }
        else if (calc >= 16) { calc -= 16; pc0PktForm += 16; }
        else if (calc >= 4) { calc -= 4; pc0CRC_EN += 4; }
        else { pc0LenConf = calc; i = 1; }
    }
}

void ELECHOUSE_CC1101::Split_MDMCFG1(void) {
    int calc = SpiReadStatus(19);
    m1FEC = 0; m1PRE = 0; m1CHSP = 0;
    for (bool i = 0; i == 0;) {
        if (calc >= 128) { calc -= 128; m1FEC += 128; }
        else if (calc >= 16) { calc -= 16; m1PRE += 16; }
        else { m1CHSP = calc; i = 1; }
    }
}

void ELECHOUSE_CC1101::Split_MDMCFG2(void) {
    int calc = SpiReadStatus(18);
    m2DCOFF = 0; m2MODFM = 0; m2MANCH = 0; m2SYNCM = 0;
    for (bool i = 0; i == 0;) {
        if (calc >= 128) { calc -= 128; m2DCOFF += 128; }
        else if (calc >= 16) { calc -= 16; m2MODFM += 16; }
        else if (calc >= 8) { calc -= 8; m2MANCH += 8; }
        else { m2SYNCM = calc; i = 1; }
    }
}

void ELECHOUSE_CC1101::Split_MDMCFG4(void) {
    int calc = SpiReadStatus(16);
    m4RxBw = 0; m4DaRa = 0;
    for (bool i = 0; i == 0;) {
        if (calc >= 64) { calc -= 64; m4RxBw += 64; }
        else if (calc >= 16) { calc -= 16; m4RxBw += 16; }
        else { m4DaRa = calc; i = 1; }
    }
}

void ELECHOUSE_CC1101::RegConfigSettings(void) {
    SpiWriteReg(CC1101_FSCTRL1, 0x06);
    setCCMode(ccmode);
    setMHZ(MHz);
    SpiWriteReg(CC1101_MDMCFG1, 0x02);
    SpiWriteReg(CC1101_MDMCFG0, 0xF8);
    SpiWriteReg(CC1101_CHANNR, chan);
    SpiWriteReg(CC1101_DEVIATN, 0x47);
    SpiWriteReg(CC1101_FREND1, 0x56);
    SpiWriteReg(CC1101_MCSM0, 0x18);
    SpiWriteReg(CC1101_FOCCFG, 0x16);
    SpiWriteReg(CC1101_BSCFG, 0x1C);
    SpiWriteReg(CC1101_AGCCTRL2, 0xC7);
    SpiWriteReg(CC1101_AGCCTRL1, 0x00);
    SpiWriteReg(CC1101_AGCCTRL0, 0xB2);
    SpiWriteReg(CC1101_FSCAL3, 0xE9);
    SpiWriteReg(CC1101_FSCAL2, 0x2A);
    SpiWriteReg(CC1101_FSCAL1, 0x00);
    SpiWriteReg(CC1101_FSCAL0, 0x1F);
    SpiWriteReg(CC1101_FSTEST, 0x59);
    SpiWriteReg(CC1101_TEST2, 0x81);
    SpiWriteReg(CC1101_TEST1, 0x35);
    SpiWriteReg(CC1101_TEST0, 0x09);
    SpiWriteReg(CC1101_PKTCTRL1, 0x04);
    SpiWriteReg(CC1101_ADDR, 0x00);
    SpiWriteReg(CC1101_PKTLEN, 0x00);
}

void ELECHOUSE_CC1101::SetTx(void) {
    SpiStrobe(CC1101_SIDLE); SpiStrobe(CC1101_STX); trxstate = 1;
}

void ELECHOUSE_CC1101::SetRx(void) {
    SpiStrobe(CC1101_SIDLE); SpiStrobe(CC1101_SRX); trxstate = 2;
}

void ELECHOUSE_CC1101::SetTx(float mhz) {
    SpiStrobe(CC1101_SIDLE); setMHZ(mhz); SpiStrobe(CC1101_STX); trxstate = 1;
}

void ELECHOUSE_CC1101::SetRx(float mhz) {
    SpiStrobe(CC1101_SIDLE); setMHZ(mhz); SpiStrobe(CC1101_SRX); trxstate = 2;
}

int ELECHOUSE_CC1101::getRssi(void) {
    int rssi = SpiReadStatus(CC1101_RSSI);
    if (rssi >= 128) { rssi = (rssi - 256) / 2 - 74; }
    else { rssi = (rssi / 2) - 74; }
    return rssi;
}

byte ELECHOUSE_CC1101::getLqi(void) { return SpiReadStatus(CC1101_LQI); }

void ELECHOUSE_CC1101::setSres(void) { SpiStrobe(CC1101_SRES); trxstate = 0; }

void ELECHOUSE_CC1101::setSidle(void) { SpiStrobe(CC1101_SIDLE); trxstate = 0; }

void ELECHOUSE_CC1101::goSleep(void) {
    trxstate = 0; SpiStrobe(0x36); SpiStrobe(0x39);
}

void ELECHOUSE_CC1101::SendData(char *txchar) {
    int len = strlen(txchar);
    byte chartobyte[len];
    for (int i = 0; i < len; i++) { chartobyte[i] = txchar[i]; }
    SendData(chartobyte, len);
}

void ELECHOUSE_CC1101::SendData(byte *txBuffer, byte size) {
    SpiWriteReg(CC1101_TXFIFO, size);
    SpiWriteBurstReg(CC1101_TXFIFO, txBuffer, size);
    SpiStrobe(CC1101_SIDLE);
    SpiStrobe(CC1101_STX);
    while (!gpio_get(GDO0));
    while (gpio_get(GDO0));
    SpiStrobe(CC1101_SFTX);
    trxstate = 1;
}

void ELECHOUSE_CC1101::SendData(char *txchar, int t) {
    int len = strlen(txchar);
    byte chartobyte[len];
    for (int i = 0; i < len; i++) { chartobyte[i] = txchar[i]; }
    SendData(chartobyte, len, t);
}

void ELECHOUSE_CC1101::SendData(byte *txBuffer, byte size, int t) {
    SpiWriteReg(CC1101_TXFIFO, size);
    SpiWriteBurstReg(CC1101_TXFIFO, txBuffer, size);
    SpiStrobe(CC1101_SIDLE);
    SpiStrobe(CC1101_STX);
    sleep_ms(t);
    SpiStrobe(CC1101_SFTX);
    trxstate = 1;
}

bool ELECHOUSE_CC1101::CheckCRC(void) {
    byte lqi = SpiReadStatus(CC1101_LQI);
    bool crc_ok = (lqi >> 7) & 0x01;
    if (crc_ok == 1) { return 1; }
    else { SpiStrobe(CC1101_SFRX); SpiStrobe(CC1101_SRX); return 0; }
}

bool ELECHOUSE_CC1101::CheckRxFifo(int t) {
    if (trxstate != 2) { SetRx(); }
    if (SpiReadStatus(CC1101_RXBYTES) & BYTES_IN_RXFIFO) { sleep_ms(t); return 1; }
    else { return 0; }
}

byte ELECHOUSE_CC1101::CheckReceiveFlag(void) {
    if (trxstate != 2) { SetRx(); }
    if (gpio_get(GDO0)) { while (gpio_get(GDO0)); return 1; }
    else { return 0; }
}

byte ELECHOUSE_CC1101::ReceiveData(byte *rxBuffer) {
    byte size;
    byte status[2];
    if (SpiReadStatus(CC1101_RXBYTES) & BYTES_IN_RXFIFO) {
        size = SpiReadReg(CC1101_RXFIFO);
        SpiReadBurstReg(CC1101_RXFIFO, rxBuffer, size);
        SpiReadBurstReg(CC1101_RXFIFO, status, 2);
        SpiStrobe(CC1101_SFRX);
        SpiStrobe(CC1101_SRX);
        return size;
    } else {
        SpiStrobe(CC1101_SFRX);
        SpiStrobe(CC1101_SRX);
        return 0;
    }
}

ELECHOUSE_CC1101 ELECHOUSE_cc1101;
