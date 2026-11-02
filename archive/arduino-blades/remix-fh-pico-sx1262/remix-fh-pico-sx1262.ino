/*
 * remix-fh-pico-sx1262.ino — FHAdvanced-REMIX SX1262 LoRa blade
 *                            (RP2040 / arduino-pico + Seeed Wio-SX1262)
 *
 * ############################################################################
 * #  UNVERIFIED FIRST DRAFT — written WITHOUT a compiler (project rule).     #
 * #  DEDICATED LoRa blade: one Pico + one Wio-SX1262. A SPI SLAVE on the P4  #
 * #  hub's Bus B speaking BINARY fh_protocol. LoRa is the COMMS link to the  #
 * #  sensor stick (TX is CORE), NOT a capture sensor. EXPECT a compile-fix   #
 * #  pass on arduino-pico. KNOWN RISKS at the bottom.                        #
 * ############################################################################
 *
 * HARDWARE SPLIT: the Seeed "XIAO ESP32S3 & Wio-SX1262 Kit" is separated — the
 * Wio-SX1262 LoRa module is paired with THIS Pico; the XIAO ESP32-S3 is retasked
 * as the 2.4 GHz Wi-Fi sniffer (remix-FH-XIAO-S3-firmware). So this Pico drives
 * the SX1262 directly over SPI0 (master), incl. RF_SW1 (host pin) and the
 * TCXO-on-DIO3 detail the Wio module exposes.
 *
 * PORT FROM gen1 FH-Advanced-Pico-rescue-gps (the SX1262 half):
 *   - KEPT VERBATIM: the RadioLib SX1262 bring-up (explicit begin() with TCXO
 *     1.6 V — CORE-2), transmit(), and the RF_SW1 handling.
 *   - DROPPED: the other three radios + GPS (each is now its own blade).
 *   - TRANSPORT CHANGED: gen1 spoke binary fh_protocol over a UART to the hub.
 *     This blade speaks the SAME binary fh_protocol over a SPI1 SLAVE link.
 *   - ADDED a real LISTEN WINDOW (gen1 left it a stub): FH_OD_LORA_LISTEN_REQ
 *     with window_ms (0 = continuous) is the one blessed open-ended verb; while
 *     listening, inbound LoRa frames are emitted as unsolicited FH_OD_LORA_RX_IND.
 *
 * LoRa verbs (fh_ondemand.h):
 *   FH_OD_LORA_TX_REQ   (0x28) -> transmit up to 200 B to the stick -> TX_RESP (toa_ms)
 *   FH_OD_LORA_LISTEN_REQ(0x2B) -> enter/leave an RX window        -> LISTEN_RESP
 *   FH_OD_LORA_RX_IND   (0x2A) -> UNSOLICITED: a frame arrived      (blade -> hub)
 *   FH_OD_PING_REQ / FH_TYPE_PING as usual.
 *
 * TWO SPI BLOCKS: radio = SPI0 master (to the Wio-SX1262); hub = SPI1 slave.
 *
 * Libraries: arduino-pico core (Earle Philhower), RadioLib 7.8.1 (pin it).
 */
#include <Arduino.h>
#include <SPI.h>
#include <SPISlave.h>
#include <RadioLib.h>

extern "C" {
  #include "fh_frame.h"
  #include "fh_ondemand.h"
}
#include "fh_pico_pins.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  SX1262 RADIO (SPI0 master) — carried from the proven gen1 blade
 * ═══════════════════════════════════════════════════════════════════════════ */
/* RadioLib Module(cs, dio1, rst, busy) — order VERIFIED against the gen1 blade
 * which used the official RadioLib SX1262 pin order. */
static SX1262 radioLoRa = new Module(FH_PICO_SX_NSS, FH_PICO_SX_DIO1,
                                     FH_PICO_SX_RST, FH_PICO_SX_BUSY);
static bool   g_radioOk = false;

/* Region default — set YOUR ISM band (US 915 / EU 868). Placeholder 915.0. */
static float  g_freqMHz = 915.0f;

static void radioInit() {
  /* RP2040: bind SPI0 pins on the Arduino SPI object BEFORE begin(). */
  SPI.setSCK(FH_PICO_SPI_SCK);
  SPI.setTX(FH_PICO_SPI_MOSI);
  SPI.setRX(FH_PICO_SPI_MISO);
  SPI.begin();

  /* CORE-2: EXPLICIT begin() — freq, bw, sf, cr, syncWord, power, preamble,
   * tcxoVoltage (1.6 V enables the Wio module's TCXO on DIO3), regulatorLDO. */
  int16_t st = radioLoRa.begin(g_freqMHz, 125.0, 9, 7,
                               RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                               10, 8, 1.6, false);
  if (st == RADIOLIB_ERR_NONE) {
    g_radioOk = true;
    pinMode(FH_PICO_SX_RFSW1, OUTPUT);
    digitalWrite(FH_PICO_SX_RFSW1, LOW);   /* idle; sense VERIFY on your module */
  } else {
    g_radioOk = false;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HUB LINK (SPI1 slave) — binary fh_protocol
 * ═══════════════════════════════════════════════════════════════════════════ */
#define HUB_XFER_BYTES   FH_MAX_FRAME     /* 261, matches the hub master */

static fh_decoder_t s_in_dec;
static uint8_t  s_hub_tx[HUB_XFER_BYTES];

/* Inbound raw staging (SPSC: recv IRQ writes, loop() reads). IRQ stays tiny. */
static volatile uint8_t  s_rx_raw[HUB_XFER_BYTES];
static volatile uint16_t s_rx_raw_len = 0;
static volatile bool     s_rx_ready   = false;

/* pending request slot (one op at a time). LoRa TX carries up to 200 B, bigger
 * than a small slot, so keep a full-size data stage (gen1 pattern). */
static volatile bool s_req_pending = false;
static uint8_t  s_req_type = 0;
static uint8_t  s_req_seq  = 0;
static uint8_t  s_tx_flags = 0;
static uint8_t  s_tx_data[FH_LORA_DATA_MAX];
static uint8_t  s_tx_len = 0;

/* listen-window state (core0). window_ms==0 => continuous until disabled. */
static volatile bool     s_listening = false;
static volatile uint32_t s_listen_until_ms = 0;   /* 0 = continuous */
static uint8_t           s_rx_seq = 0;            /* our own RX counter */

static void hub_stage(fh_type_t type, const uint8_t *payload, uint8_t len) {
  memset(s_hub_tx, 0, sizeof(s_hub_tx));
  fh_frame_encode(type, payload, len, s_hub_tx, sizeof(s_hub_tx));
  SPISlave1.setData(s_hub_tx, HUB_XFER_BYTES);
  digitalWrite(FH_HUBLINK_IRQ, HIGH);
}

static void on_hub_frame(fh_type_t type, const uint8_t *payload, uint8_t len, void *user) {
  (void)user;
  switch ((uint8_t)type) {
    case FH_OD_PING_REQ: {
      fh_od_ping_resp_t r; memset(&r, 0, sizeof(r));
      if (len >= 1) r.seq = payload[0];
      r.radios_ready = g_radioOk ? FH_OD_RADIO_SX1262 : 0;
      hub_stage((fh_type_t)FH_OD_PING_RESP, (uint8_t*)&r, sizeof(r));
      break;
    }
    case FH_TYPE_PING: {
      if (len >= 1) { fh_ping_t p; p.seq = payload[0];
        hub_stage(FH_TYPE_PONG, (uint8_t*)&p, sizeof(p)); }
      break;
    }
    case FH_OD_LORA_LISTEN_REQ: {
      if (len >= sizeof(fh_od_lora_listen_req_t)) {
        fh_od_lora_listen_req_t q; memcpy(&q, payload, sizeof(q));
        if (!g_radioOk) {
          fh_od_lora_listen_resp_t r = { .seq=q.seq, .status=FH_OD_RADIO_FAIL, .listening=0 };
          hub_stage((fh_type_t)FH_OD_LORA_LISTEN_RESP, (uint8_t*)&r, sizeof(r));
        } else {
          if (q.enable) {
            s_listen_until_ms = q.window_ms ? (millis() + q.window_ms) : 0;  /* 0=continuous */
            s_listening = true;
            radioLoRa.startReceive();
          } else {
            s_listening = false;
            radioLoRa.standby();
          }
          fh_od_lora_listen_resp_t r = { .seq=q.seq, .status=FH_OD_OK,
                                         .listening=(uint8_t)(s_listening?1:0) };
          hub_stage((fh_type_t)FH_OD_LORA_LISTEN_RESP, (uint8_t*)&r, sizeof(r));
        }
      }
      break;
    }
    case FH_OD_LORA_TX_REQ: {
      if (len >= 3) {  /* seq + flags + len, then data */
        fh_od_lora_tx_req_t q; memset(&q, 0, sizeof(q));
        memcpy(&q, payload, len > sizeof(q) ? sizeof(q) : len);
        if (s_req_pending) {
          fh_od_lora_tx_resp_t r = { .seq=q.seq, .status=FH_OD_BUSY, .toa_ms=0 };
          hub_stage((fh_type_t)FH_OD_LORA_TX_RESP, (uint8_t*)&r, sizeof(r));
        } else {
          /* clamp the real data length to what the frame actually carried AND
           * to the staging bound (P-AB1: never trust req.len blindly). */
          uint8_t avail = (uint8_t)((len >= 3) ? (len - 3) : 0);
          uint8_t n = q.len < avail ? q.len : avail;
          if (n > FH_LORA_DATA_MAX) n = FH_LORA_DATA_MAX;
          memcpy(s_tx_data, q.data, n);
          s_tx_len   = n;
          s_tx_flags = q.flags;
          s_req_seq  = q.seq;
          s_req_type = FH_OD_LORA_TX_REQ;
          s_req_pending = true;
        }
      }
      break;
    }
    default: break;   /* not ours */
  }
}

/* ---- SPISlave1 IRQ callbacks — TINY, lock-free, no alloc (vendor contract) -- */
static void hub_on_recv(uint8_t *data, size_t len) {
  if (!s_rx_ready) {
    uint16_t nn = (len > HUB_XFER_BYTES) ? HUB_XFER_BYTES : (uint16_t)len;
    for (uint16_t i = 0; i < nn; ++i) { s_rx_raw[i] = data[i]; }
    s_rx_raw_len = nn;
    s_rx_ready = true;
  }
}
static void hub_on_sent() {
  SPISlave1.setData(s_hub_tx, HUB_XFER_BYTES);   /* re-present the built frame */
}

/* Execute a pending LoRa TX (from loop(), not the ISR — a TX can take ~1 s). */
static void service_pending() {
  if (!(s_req_pending && s_req_type == FH_OD_LORA_TX_REQ)) { return; }
  uint8_t seq = s_req_seq, flags = s_tx_flags, n = s_tx_len;
  static uint8_t data[FH_LORA_DATA_MAX];
  memcpy(data, s_tx_data, n);

  (void)flags;
  fh_od_lora_tx_resp_t resp; memset(&resp, 0, sizeof(resp));
  resp.seq = seq;
  if (!g_radioOk) {
    resp.status = FH_OD_RADIO_FAIL;
  } else {
    digitalWrite(FH_PICO_SX_RFSW1, HIGH);        /* TX path (VERIFY sense) */
    uint32_t t0 = millis();
    int16_t st = radioLoRa.transmit(data, n);
    resp.toa_ms = (uint16_t)(millis() - t0);
    digitalWrite(FH_PICO_SX_RFSW1, LOW);         /* back to RX path */
    resp.status = (st == RADIOLIB_ERR_NONE) ? FH_OD_OK : FH_OD_RADIO_FAIL;
    if (s_listening) { radioLoRa.startReceive(); }  /* resume the window */
  }
  hub_stage((fh_type_t)FH_OD_LORA_TX_RESP, (uint8_t*)&resp, sizeof(resp));
  s_req_pending = false;
}

/* Drain inbound raw bytes copied out of the IRQ and decode them (loop ctx). */
static void service_rx() {
  if (!s_rx_ready) { return; }
  uint8_t buf[HUB_XFER_BYTES]; uint16_t n = s_rx_raw_len;
  for (uint16_t i = 0; i < n; ++i) { buf[i] = s_rx_raw[i]; }
  s_rx_ready = false;
  fh_decoder_push(&s_in_dec, buf, n, on_hub_frame, NULL);
}

/* Poll the listen window: if a frame arrived, emit an unsolicited RX_IND. */
static void service_listen() {
  if (!s_listening || !g_radioOk) return;
  /* end a bounded window */
  if (s_listen_until_ms != 0 && (int32_t)(millis() - s_listen_until_ms) >= 0) {
    s_listening = false;
    radioLoRa.standby();
    return;
  }
  /* non-blocking check for a received packet. FIX (2026-10-31, S5): gate on
   * checkDataReady() (the DIO1-packet flag) BEFORE readData() — the old blind
   * readData() poll read no-packet status every loop and its return
   * convention (len vs RADIOLIB_ERR_NONE) is exactly the ambiguity S5 flags.
   * checkDataReady() returns true only when a packet is buffered; then
   * getPacketLength() gives the real size and readData() drains it. */
  if (!radioLoRa.checkDataReady()) { return; }   /* no packet yet (non-blocking) */
  uint8_t buf[FH_LORA_DATA_MAX];
  uint16_t pkt = (uint16_t)radioLoRa.getPacketLength(true);
  if (pkt == 0 || pkt > FH_LORA_DATA_MAX) {      /* bad/oversized: drain + re-arm */
    radioLoRa.readData(buf, sizeof(buf));
    radioLoRa.startReceive();
    return;
  }
  int16_t n = radioLoRa.readData(buf, sizeof(buf));  /* drains the buffer */
  if (n > 0) {
    fh_od_lora_rx_ind_t ind; memset(&ind, 0, sizeof(ind));
    ind.seq  = s_rx_seq++;
    ind.rssi = (int8_t)radioLoRa.getRSSI();
    ind.snr  = (int8_t)radioLoRa.getSNR();
    ind.len  = (n > (int16_t)sizeof(ind.data)) ? (uint8_t)sizeof(ind.data) : (uint8_t)n;
    memcpy(ind.data, buf, ind.len);
    hub_stage((fh_type_t)FH_OD_LORA_RX_IND, (uint8_t*)&ind,
              (uint8_t)(4 + ind.len));              /* seq+rssi+snr+len + data */
    radioLoRa.startReceive();                       /* re-arm */
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ARDUINO ENTRY POINTS
 * ═══════════════════════════════════════════════════════════════════════════ */
void setup() {
  Serial.begin(115200);                  /* USB debug console only */
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 1500)) { /* brief wait */ }

  fh_decoder_init(&s_in_dec);

  radioInit();                           /* SX1262 on SPI0 master */

  pinMode(FH_HUBLINK_IRQ, OUTPUT);
  digitalWrite(FH_HUBLINK_IRQ, LOW);
  SPISlave1.setSCK(FH_HUBLINK_SCK);
  SPISlave1.setTX(FH_HUBLINK_TX);        /* MISO (blade -> hub) */
  SPISlave1.setRX(FH_HUBLINK_RX);        /* MOSI (hub -> blade) */
  SPISlave1.setCS(FH_HUBLINK_CS);
  SPISlave1.onDataRecv(hub_on_recv);     /* vendor name: onDataRecv (not onData) */
  SPISlave1.onDataSent(hub_on_sent);
  /* MODE 1 (CPOL=0, CPHA=1) — REQUIRED on the RP2040 SSP slave: in MODE 0 the
   * slave-select freezes the shift register and only ONE byte per CS assertion
   * is transmitted (byte 0, then 0xff underrun). The hub drives Bus B in MODE 1
   * (fh_hub_link.c dc.mode=1) and the bench-proven GPS blade uses MODE 1. See
   * AGENTS.md §1 / PICO_LESSONS.md §1 — do NOT change this back to SPI_MODE0. */
  SPISlave1.begin(SPISettings(8000000, MSBFIRST, SPI_MODE1));
  memset(s_hub_tx, 0, sizeof(s_hub_tx));
  SPISlave1.setData(s_hub_tx, HUB_XFER_BYTES);  /* prime the first (idle) buffer */

  Serial.print(F("remix-fh-pico-sx1262 v1 (SPI) boot  radio="));
  Serial.println(g_radioOk ? F("ok") : F("ABSENT (check 3.3V + SPI0 + TCXO)"));
}

void loop() {
  service_rx();                          /* decode any inbound frame (IRQ copied raw) */
  service_pending();                     /* run a fired LoRa TX */
  service_listen();                      /* emit unsolicited RX during a window */

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 's') {
      Serial.print(F("status radio=")); Serial.print(g_radioOk ? F("ok") : F("absent"));
      Serial.print(F(" freq=")); Serial.print(g_freqMHz, 3);
      Serial.print(F(" listening=")); Serial.println(s_listening ? F("yes") : F("no"));
    }
  }
  delay(2);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  KNOWN RISKS (UNVERIFIED — first build + bench)
 *   [S1] arduino-pico SPISlave1 API (SPI1 as slave): verify SPISlave1 +
 *        setters/callbacks on YOUR core. The radio is SPI0 master (RadioLib on
 *        GP18/19/16), so the hub slave MUST be SPI1. #1 compile-fix.
 *   [S2] TWO SPI BLOCKS: SX1262 = SPI0 master, hub = SPI1 slave. HUBLINK pins
 *        (GP14/24/15/25 + IRQ GP2) are a VALID spi1 group (FIX 2026-10-31:
 *        the first draft mapped GP6/4/3/5 — that is an SPI0 mux group, the
 *        link would never clock). Still verify on your board: no clash with
 *        the radio pins (GP18/19/16/17/12/11/10/9), and the hub's Bus-B
 *        wiring is updated to match.
 *   [S3] RadioLib Module(cs,dio1,rst,busy) order carried from the proven gen1
 *        blade. begin() uses tcxoVoltage=1.6 (enables the Wio module's TCXO on
 *        DIO3 — CORE-2). A -707 at begin = TCXO issue; -2 = SPI/DIO wiring.
 *   [S4] RF_SW1 (GP9): host-controlled antenna switch on the Wio board. We set
 *        it HIGH for TX, LOW for RX — VERIFY the sense on YOUR module rev (gen1
 *        idled LOW). Keep the ANTENNA attached before any TX (+22 dBm PA).
 *   [S5] readData()/startReceive()/getRSSI()/getSNR()/checkDataReady()/
 *        getPacketLength() names + the return conventions — VERIFY against
 *        RadioLib 7.8.1. FIX (2026-10-31): the listen window now gates on
 *        checkDataReady() BEFORE readData() (the old blind readData() poll
 *        read no-packet status every loop). Still non-blocking: a blocking
 *        receive() would hang the command loop (gen1 CORE-4) — DO NOT use it.
 *   [S6] LoRa TX runs in loop() (service_pending), not the SPI callback, so a
 *        ~1 s TX never stalls the bus; a second REQ mid-TX gets BUSY.
 *   [S7] LISTEN window = the one blessed open-ended verb (window_ms=0 =
 *        continuous). While listening, inbound frames -> unsolicited
 *        FH_OD_LORA_RX_IND. The hub must expect unsolicited RX_IND on this blade.
 *   [S8] LoRa is COMMS (link to the sensor stick), not capture — authorized-use
 *        only; TX gated on the hub/head, never auto. Antenna mandatory.
 * ═══════════════════════════════════════════════════════════════════════════ */
