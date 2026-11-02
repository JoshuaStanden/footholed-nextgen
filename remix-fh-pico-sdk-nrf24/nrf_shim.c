/*
 * nrf_shim.c — minimal nRF24L01(+) driver (native Pico SDK, spi1 master).
 * See nrf_shim.h for scope + provenance. Register map/commands are Nordic
 * nRF24L01+ datasheet facts. CSN is software GPIO; CE is RX/TX enable.
 *
 * UNVERIFIED FIRST DRAFT — reasoned against the datasheet, NOT bench-run.
 * This is FH-owned code (permissive), not a vendored library.
 */
#include "nrf_shim.h"
#include "fh_pico_pins_sdk.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#define NRF_SPI   spi1

/* ---- SPI commands (datasheet §8.3.1) ---- */
#define CMD_R_REGISTER(r)   (0x00 | ((r) & 0x1F))
#define CMD_W_REGISTER(r)   (0x20 | ((r) & 0x1F))
#define CMD_R_RX_PAYLOAD    0x61
#define CMD_W_TX_PAYLOAD    0xA0
#define CMD_FLUSH_TX        0xE1
#define CMD_FLUSH_RX        0xE2
#define CMD_R_RX_PL_WID     0x60   /* (reserved: DPL-only; capture uses static) */
#define CMD_NOP             0xFF

/* ---- registers ---- */
#define REG_CONFIG      0x00
#define REG_EN_AA       0x01
#define REG_EN_RXADDR   0x02
#define REG_SETUP_AW    0x03
#define REG_RF_CH       0x05
#define REG_RF_SETUP    0x06
#define REG_STATUS      0x07
#define REG_RPD         0x09   /* a.k.a. CD on non-plus parts */
#define REG_RX_ADDR_P0  0x0A
#define REG_TX_ADDR     0x10
#define REG_RX_PW_P0    0x11
#define REG_FIFO_STATUS 0x17
#define REG_DYNPD       0x1C
#define REG_FEATURE     0x1D

/* ---- CONFIG bits ---- */
#define CFG_PWR_UP   (1u << 1)
#define CFG_PRIM_RX  (1u << 0)
#define CFG_EN_CRC   (1u << 3)
#define CFG_CRCO     (1u << 2)
/* ---- STATUS bits ---- */
#define ST_RX_DR     (1u << 6)
#define ST_TX_DS     (1u << 5)
#define ST_MAX_RT    (1u << 4)
/* ---- FEATURE bits ---- */
#define FEAT_EN_DPL  (1u << 2)   /* (reserved: DPL path not used in capture) */
/* ---- FIFO_STATUS bits ---- */
#define FIFO_RX_EMPTY (1u << 0)

static uint8_t s_channel = 76;
static bool    s_present = false;

static inline void csn(bool level) { gpio_put(FH_NRF_CSN, level); }
static inline void ce(bool level)  { gpio_put(FH_NRF_CE, level); }

/* One SPI transfer with CSN framing. Writes out[], captures in[] (either may
 * be NULL for don't-care). Returns the STATUS byte (clocked out during the
 * command byte on the nRF24). */
static uint8_t nrf_xfer(const uint8_t *out, uint8_t *in, size_t n) {
    uint8_t status = 0;
    csn(0);
    for (size_t i = 0; i < n; i++) {
        uint8_t tx = out ? out[i] : CMD_NOP;
        uint8_t rx = 0;
        spi_write_read_blocking(NRF_SPI, &tx, &rx, 1);
        if (i == 0) status = rx;          /* STATUS shifts out on byte 0 */
        if (in) in[i] = rx;
    }
    csn(1);
    return status;
}

static uint8_t reg_read(uint8_t reg) {
    uint8_t out[2] = { CMD_R_REGISTER(reg), CMD_NOP };
    uint8_t in[2]  = { 0, 0 };
    nrf_xfer(out, in, 2);
    return in[1];
}

static void reg_write(uint8_t reg, uint8_t val) {
    uint8_t out[2] = { CMD_W_REGISTER(reg), val };
    nrf_xfer(out, NULL, 2);
}

static uint8_t status_read(void) {
    uint8_t cmd = CMD_NOP;
    return nrf_xfer(&cmd, NULL, 1);
}

static void cmd1(uint8_t c) { nrf_xfer(&c, NULL, 1); }

bool nrf_init(uint8_t channel) {
    s_channel = (channel <= 125) ? channel : 76;

    /* spi1 master, MODE 0, 4 MHz (nRF24 max 10 MHz; conservative). */
    spi_init(NRF_SPI, 4 * 1000 * 1000);
    spi_set_format(NRF_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(FH_NRF_SPI_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(FH_NRF_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(FH_NRF_SPI_MISO, GPIO_FUNC_SPI);
    /* CSN + CE are software GPIO. */
    gpio_init(FH_NRF_CSN); gpio_set_dir(FH_NRF_CSN, GPIO_OUT); csn(1);
    gpio_init(FH_NRF_CE);  gpio_set_dir(FH_NRF_CE,  GPIO_OUT); ce(0);

    sleep_ms(5);                           /* power-on settle */

    /* Power up, CRC enabled (2-byte), PRIM_RX=0 (standby-I to start). */
    reg_write(REG_CONFIG, CFG_PWR_UP | CFG_EN_CRC | CFG_CRCO);
    sleep_ms(5);                           /* Tpd2stby */

    reg_write(REG_RF_CH, s_channel);
    reg_write(REG_RF_SETUP, 0x06);         /* 1 Mbps, 0 dBm */
    reg_write(REG_EN_AA, 0x00);            /* auto-ack OFF (framework) */
    reg_write(REG_SETUP_AW, 0x03);         /* 5-byte address width */

    /* Presence check: write then read back RF_CH (round-trip proves SPI link
     * + a live chip, the equivalent of RF24::isChipConnected). */
    reg_write(REG_RF_CH, s_channel);
    s_present = (reg_read(REG_RF_CH) == s_channel);
    return s_present;
}

bool nrf_present(void) {
    if (!s_present) return false;
    return (reg_read(REG_RF_CH) == s_channel);
}

void nrf_set_channel(uint8_t channel) {
    s_channel = (channel <= 125) ? channel : 125;
    reg_write(REG_RF_CH, s_channel);
}

void nrf_idle(void) {
    ce(0);                                 /* standby */
}

void nrf_rx_mode(bool open_capture) {
    ce(0);
    if (open_capture) {
        /* SNIFF capture mode — the DATASHEET-CORRECT recipe for reading
         * payloads without being the intended recipient (confirmed by Nordic:
         * "standard SB mode: clear FEATURE and EN_AA, set max payload length
         * (32), disable CRC"). Dynamic payloads do NOT work here because DPL
         * requires auto-ack; so we use a STATIC 32-byte payload width and read
         * a fixed 32 bytes per packet. NOT true promiscuity — RX_ADDR_P0 stays
         * at the power-on default (0xE7E7E7E7E7), so it captures packets sent
         * to THAT address (which this blade's own TX uses). See R-PROMISC. */
        reg_write(REG_FEATURE, 0x00);                /* no DPL / ack-pay */
        reg_write(REG_DYNPD,   0x00);                /* static payload width */
        reg_write(REG_EN_AA,   0x00);                /* auto-ack OFF */
        reg_write(REG_EN_RXADDR, 0x01);              /* pipe 0 enabled */
        reg_write(REG_RX_PW_P0, 32);                 /* STATIC 32-byte width */
        reg_write(REG_CONFIG, CFG_PWR_UP);           /* CRC OFF; PRIM_RX below */
    } else {
        /* Normal framed RX: CRC on, auto-ack off (framework), static 32-byte
         * width on pipe 0. (SCAN only needs RX settled for RPD; width is set
         * for completeness.) */
        reg_write(REG_FEATURE, 0x00);
        reg_write(REG_DYNPD,   0x00);
        reg_write(REG_EN_AA,   0x00);
        reg_write(REG_EN_RXADDR, 0x01);
        reg_write(REG_RX_PW_P0, 32);
        reg_write(REG_CONFIG, CFG_PWR_UP | CFG_EN_CRC | CFG_CRCO);
    }
    /* set PRIM_RX = 1 (RX), keeping the CRC choice written above */
    uint8_t cfg = reg_read(REG_CONFIG) | CFG_PWR_UP | CFG_PRIM_RX;
    reg_write(REG_CONFIG, cfg);
    cmd1(CMD_FLUSH_RX);
    reg_write(REG_STATUS, ST_RX_DR | ST_TX_DS | ST_MAX_RT);  /* clear */
    ce(1);                                 /* start listening */
    sleep_us(130);                         /* Tstby2a */
}

bool nrf_rpd(void) {
    /* RPD latches high while a carrier > -64 dBm is present in RX. Read the
     * low bit of the RPD register. (On non-plus parts this is CD at 0x09.) */
    return (reg_read(REG_RPD) & 0x01) != 0;
}

int nrf_rx_payload(uint8_t *buf, int cap) {
    uint8_t st = status_read();
    if (!(st & ST_RX_DR)) {
        /* also check FIFO in case RX_DR was already cleared */
        if (reg_read(REG_FIFO_STATUS) & FIFO_RX_EMPTY) return 0;
    }
    /* STATIC payload width: capture mode uses a fixed 32-byte RX_PW_P0 (DPL is
     * disabled — DPL requires auto-ack, which is off for capture). So always
     * read 32 bytes from the FIFO. (R_RX_PL_WID is NOT used; it is only valid
     * with DPL enabled.) */
    const int plen = 32;
    int ret = (plen > cap) ? cap : plen;
    uint8_t tx[33]; uint8_t rx[33];
    tx[0] = CMD_R_RX_PAYLOAD;
    for (int i = 1; i <= plen; i++) tx[i] = CMD_NOP;
    nrf_xfer(tx, rx, (size_t)(plen + 1));
    for (int i = 0; i < ret; i++) buf[i] = rx[i + 1];
    reg_write(REG_STATUS, ST_RX_DR);       /* clear the data-ready flag */
    return ret;
}

bool nrf_tx_payload(const uint8_t *buf, int len) {
    if (len < 1 || len > 32) return false;
    ce(0);
    /* PRIM_RX = 0 (TX mode), PWR_UP, and an EXPLICIT 2-byte CRC. Do NOT
     * inherit the current CONFIG: a preceding SNIFF writes CRC OFF, and a TX
     * sent CRC-less is silently dropped by every standard nRF24 receiver
     * (default CRC on). Multi-check 2026-11-01 fix. Every RX entry rewrites
     * CONFIG via nrf_rx_mode(), so forcing it here has no side effects. */
    reg_write(REG_CONFIG, CFG_PWR_UP | CFG_EN_CRC | CFG_CRCO);
    cmd1(CMD_FLUSH_TX);
    reg_write(REG_STATUS, ST_TX_DS | ST_MAX_RT);

    /* W_TX_PAYLOAD + data */
    uint8_t tx[33];
    tx[0] = CMD_W_TX_PAYLOAD;
    for (int i = 0; i < len; i++) tx[1 + i] = buf[i];
    nrf_xfer(tx, NULL, (size_t)(len + 1));

    /* pulse CE >=10us to transmit */
    ce(1); sleep_us(15); ce(0);

    /* wait for TX_DS or MAX_RT (auto-ack is off, so TX_DS fires once sent). */
    bool sent = false;
    for (int i = 0; i < 100; i++) {         /* ~ up to a few ms */
        uint8_t st = status_read();
        if (st & ST_TX_DS)  { sent = true; break; }
        if (st & ST_MAX_RT) { break; }
        sleep_us(50);
    }
    reg_write(REG_STATUS, ST_TX_DS | ST_MAX_RT);
    if (!sent) cmd1(CMD_FLUSH_TX);
    return sent;
}
