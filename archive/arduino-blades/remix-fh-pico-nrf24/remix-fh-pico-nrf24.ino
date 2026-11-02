/* ═══════════════════════════════════════════════════════════════════════════
 *  remix-fh-pico-nrf24  —  FH REMIX nRF24 blade (SPI slave on Bus B, FRAMEWORK)
 * ───────────────────────────────────────────────────────────────────────────
 *  Board tier : "pico"  — original Raspberry Pi Pico (RP2040)
 *  Radio      : nRF24L01 (+PA+LNA or plain), 2.4 GHz
 *  Core       : arduino-pico (earlephilhower)
 *  Library    : RF24 by TMRh20  (the standard nRF24 Arduino library)
 *  Status     : UNVERIFIED — reasoned-not-compiled. NOT bench-tested (project rule).
 *
 *  WHAT THIS IS  (and is NOT)
 *  --------------------------
 *  The communication FRAMEWORK only: bring the nRF24 up, prove the Pico talks
 *  to it over SPI, and answer the hub's on-demand requests. In REMIX it is a
 *  SPI SLAVE on the P4 hub's Bus B, speaking the BINARY fh_protocol (fh_frame +
 *  fh_ondemand) the rest of the system uses — NOT the gen1 ASCII v0. The hub
 *  FIRES FH_OD_NRF24_REQ (a channel presence scan) and the blade answers
 *  FH_OD_NRF24_RESP. One op at a time (frozen BUSY contract).
 *
 *  THE RADIO CORE IS CARRIED VERBATIM from the proven gen1 framework blade
 *  (radioInit + the RF24 bring-up). ONLY the transport changed: ASCII-over-USB
 *  -> binary fh_frame over an SPI slave link. USB `Serial` is KEPT as a LOCAL
 *  DEBUG console (human-readable), NOT the hub link.
 *
 *  It does NOT contain capture/attack MODULES — no sniffer, injection, spam, or
 *  jammer. The framework comes first; modules are later head-gated additions.
 *
 *  nRF24 TRUTH (carried lesson): the nRF24 has NO true RSSI — only a 1-bit
 *  carrier-detect (RPD). So a presence "scan" reports WHICH channel showed a
 *  carrier (RPD set), NOT a dBm. The RESP's rssi field is left 0 and the
 *  active_chan field carries the real signal. Do not invent an RSSI here.
 *
 *  AUTHORIZED USE ONLY (once modules exist). Authorization lives on the HEAD.
 *  ═══════════════════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <SPI.h>
#include <SPISlave.h>
#include <RF24.h>          // RF24 by TMRh20 (install via Library Manager)

extern "C" {
  #include "fh_frame.h"
  #include "fh_ondemand.h"
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PIN MAP  —  nRF24L01 <-> Pico (RP2040), SPI0
 * ───────────────────────────────────────────────────────────────────────────
 *  nRF24 VCC is 3.3V ONLY — use the Pico 3V3(OUT) pin (phys 36). 5V kills it.
 *  nRF24 uses CE + CSN as its two control pins (NOT the CC1101's CS + GDO).
 *  A 10uF capacitor across the nRF24 VCC/GND is strongly recommended
 *  (especially +PA+LNA modules) to stop brown-out resets — see WIRING.md.
 *
 *    nRF24 pin   Pico GPIO   Pico phys pin   Notes
 *    VCC         3V3(OUT)    36              3.3V ONLY (+ 10uF cap at module)
 *    GND         GND         any GND         common ground
 *    SCK         GP18        24              SPI0 SCK
 *    MOSI        GP19        25              SPI0 TX
 *    MISO        GP16        21              SPI0 RX
 *    CSN         GP17        22              SPI chip select
 *    CE          GP20        26              chip enable (RX/TX toggle)
 *    IRQ         (unused)    —               optional; not used in framework
 * ═══════════════════════════════════════════════════════════════════════════ */
static const uint8_t PIN_SCK  = 18;
static const uint8_t PIN_MOSI = 19;
static const uint8_t PIN_MISO = 16;
static const uint8_t PIN_CSN  = 17;
static const uint8_t PIN_CE   = 20;

// The RF24 object: tell it which pins are CE and CSN.
static RF24 radio(PIN_CE, PIN_CSN);

/* ═══════════════════════════════════════════════════════════════════════════
 *  STATE
 * ═══════════════════════════════════════════════════════════════════════════ */
static bool    g_radioOk  = false;
static uint8_t g_channel  = 76;      // nRF24 default channel (2400 + ch MHz)
static bool    g_isPlusPA = false;   // true if a +PA+LNA (P-variant) module

static const char* BLADE_ID = "FH-Gen2-blade-nrf24 v0 (framework)";

/* ═══════════════════════════════════════════════════════════════════════════
 *  RADIO BRING-UP  —  framework only: init + confirm it's really there
 * ═══════════════════════════════════════════════════════════════════════════ */
/* radioInit() — FULL radio bring-up AND full reset to framework defaults.
 * NOTE (M1): this re-applies ALL defaults (channel/rate/PA/ack/idle). Once
 * modules exist that change these at runtime, calling radioInit() again will
 * REVERT them. That is intentional for the framework (a known clean state) but
 * modules must not assume their settings survive a radioInit().
 *
 * H1 — SPI PIN BINDING (the most likely cause of a false radio=ABSENT):
 * On arduino-pico, the RF24 library talks over the global `SPI` object. We set
 * the SPI0 pins on that object FIRST, then hand that same `&SPI` to
 * radio.begin(&SPI) so the library uses our configured bus rather than
 * re-initializing a default one. If radio=ABSENT despite correct wiring, this
 * binding is the first suspect — see README/WIRING for the fallbacks. */
static void radioInit() {
  // Bind SPI0 to our chosen pins BEFORE begin(), then start the bus.
  SPI.setRX(PIN_MISO);
  SPI.setTX(PIN_MOSI);
  SPI.setSCK(PIN_SCK);
  SPI.begin();

  // Pass the SAME SPI instance we just configured, so RF24 uses OUR pins.
  // (If your RF24 version lacks the &SPI overload, fall back to radio.begin();
  //  but then verify the default SPI pins match this wiring — see WIRING.md.)
  if (!radio.begin(&SPI)) { g_radioOk = false; return; }

  // isChipConnected() is a second, stronger "is it really there" check.
  if (!radio.isChipConnected()) { g_radioOk = false; return; }

  g_radioOk = true;

  // Sensible, SAFE defaults for a framework (no TX happens here):
  radio.setChannel(g_channel);
  radio.setDataRate(RF24_1MBPS);
  radio.setPALevel(RF24_PA_LOW);   // LOW on purpose — framework shouldn't blast
  radio.setAutoAck(false);
  radio.stopListening();           // idle; modules decide RX/TX later

  // isPVariant() reports whether this is a +PA+LNA module.
  g_isPlusPA = radio.isPVariant();
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  HUB LINK  —  binary fh_protocol over an SPI SLAVE on the P4 hub's Bus B
 * ───────────────────────────────────────────────────────────────────────────
 *  REPLACES the gen1 ASCII-lines-over-USB protocol. PIN SEPARATION: the nRF24
 *  uses hardware SPI0 (GP18/19/16) as MASTER to talk to the chip; the HUB LINK
 *  is a separate SPI SLAVE on hardware SPI1 (different pins). Two SPI blocks,
 *  no collision.
 * ═══════════════════════════════════════════════════════════════════════════ */

static const uint8_t HUBLINK_SCK  = 10;   // GP10 — spi1 SCK  (hub drives)
static const uint8_t HUBLINK_RX   = 12;   // GP12 — spi1 RX = MOSI (hub->blade)
static const uint8_t HUBLINK_TX   = 11;   // GP11 — spi1 TX = MISO (blade->hub)
static const uint8_t HUBLINK_CS   = 13;   // GP13 — spi1 CSn  (hub selects)
static const uint8_t HUBLINK_IRQ  =  8;   // GP8  — "result ready" out (optional)

#define HUB_XFER_BYTES   FH_MAX_FRAME     /* 261, matches the hub master */

static fh_decoder_t s_in_dec;
static uint8_t  s_hub_tx[HUB_XFER_BYTES];

/* Inbound raw staging (SPSC: recv IRQ writes, loop() reads). IRQ stays tiny. */
static volatile uint8_t  s_rx_raw[HUB_XFER_BYTES];
static volatile uint16_t s_rx_raw_len = 0;
static volatile bool     s_rx_ready   = false;

static volatile bool s_req_pending = false;
static uint8_t  s_req_type = 0;
static fh_od_nrf24_req_t s_req_nrf;

/* ---- SNIFF capture-to-dump (Tier 2, FH_OD_DUMP_IND 0x2F chunk stream) -----
 * A SNIFF (mode 1) dwell captures every payload seen (nRF24L01+ payloads are
 * 1..32 B) into s_dump_buf as 40-byte records: u32 t_ms LE (millis() since
 * sniff start) + u8 len + payload[32] + 3 reserved. It streams them to the
 * hub as FH_OD_DUMP_IND chunks BEFORE the summary RESP (the hub's flight
 * completes on the RESP, so every chunk must already be clocked out). Pacing:
 * ONE chunk per poll transaction — hub_on_sent (IRQ) sets s_tx_done; loop()'s
 * service_stream() advances only after a poll clocked the staged chunk. */
#define DUMP_CHUNK_DATA_MAX 250u              /* dump_ind data[] capacity      */
#define DUMP_REC_NRF24      40u               /* u32 t_ms + u8 len + 32 + 3    */
#define NRF_DUMPS_MAX       64u               /* 64 payloads x 40 B = 2560 B   */
static uint8_t  s_dump_buf[NRF_DUMPS_MAX * DUMP_REC_NRF24];
static uint16_t s_dump_len   = 0;             /* bytes valid in s_dump_buf     */
static uint8_t  s_dump_chunk = 0;             /* index of the next chunk to build */
static bool     s_streaming  = false;
static fh_od_nrf24_resp_t s_dump_resp;        /* summary RESP staged LAST      */

/* LOOP-CTX ONLY: build the outbound frame. No lock — single-core; s_hub_tx is
 * written only here, read only by the onDataSent IRQ. */
static void hub_stage(fh_type_t type, const uint8_t *payload, uint8_t len) {
  memset(s_hub_tx, 0, sizeof(s_hub_tx));
  fh_frame_encode(type, payload, len, s_hub_tx, sizeof(s_hub_tx));
  SPISlave1.setData(s_hub_tx, HUB_XFER_BYTES);
  digitalWrite(HUBLINK_IRQ, HIGH);
}

static void on_hub_frame(fh_type_t type, const uint8_t *payload, uint8_t len, void *user) {
  (void)user;
  switch ((uint8_t)type) {
    case FH_OD_PING_REQ: {
      fh_od_ping_resp_t r; memset(&r, 0, sizeof(r));
      if (len >= 1) r.seq = payload[0];
      r.radios_ready = g_radioOk ? FH_OD_RADIO_NRF24 : 0;
      hub_stage((fh_type_t)FH_OD_PING_RESP, (uint8_t*)&r, sizeof(r));
      break;
    }
    case FH_OD_NRF24_REQ: {
      if (len >= sizeof(fh_od_nrf24_req_t)) {
        if (!s_req_pending) {
          memcpy(&s_req_nrf, payload, sizeof(s_req_nrf));
          s_req_type = FH_OD_NRF24_REQ;
          s_req_pending = true;
        } else {
          fh_od_nrf24_resp_t r; memset(&r, 0, sizeof(r));
          r.seq = payload[0]; r.status = FH_OD_BUSY; r.active_chan = 0xFF;
          hub_stage((fh_type_t)FH_OD_NRF24_RESP, (uint8_t*)&r, sizeof(r));
        }
      }
      break;
    }
    case FH_TYPE_PING: {
      if (len >= 1) { fh_ping_t p; p.seq = payload[0];
        hub_stage(FH_TYPE_PONG, (uint8_t*)&p, sizeof(p)); }
      break;
    }
    default:
      /* ---- EXTERN VERB BAND (Tier 3) --------------------------------------
       * 0x38..0x3F EVEN = this blade's extern REQs (frozen: JAM 0x38,
       * PROTOKILL 0x3A; 0x3C..0x3E reserved for future authors). Decoded with
       * the generic fh_od_ext_req_t, answered with the generic
       * fh_od_ext_resp_t. THIS STUB answers FH_OD_EXT_STUB so the bench can
       * prove the routing end-to-end; the EXTERNAL AUTHOR replaces this case
       * with the real implementation (jam/protokill are transmit-disruption
       * primitives deliberately not authored in the remix build). The hub's
       * flight completes on the RESP; p3 (bytes 10..13 LE) = duration_ms.  */
      if ((uint8_t)type >= FH_OD_EXT_NRF_JAM_REQ && (uint8_t)type <= 0x3F
          && (((uint8_t)type & 1u) == 0) && len >= 15) {
        fh_od_ext_req_t ext; memset(&ext, 0, sizeof(ext));
        memcpy(&ext, payload, len > sizeof(ext) ? sizeof(ext) : len);
        fh_od_ext_resp_t er; memset(&er, 0, sizeof(er));
        er.seq = ext.seq;
        if ((uint8_t)type == FH_OD_EXT_NRF_TX_REQ) {
          /* nRF24 TX (IMPLEMENTED): transmit a 1..32-byte payload on a channel
           * to YOUR OWN receiver. p1=channel 0..125, p2=repeats (cap 16),
           * p3=0. tail[0]=payload_len, tail[1..]=payload. Uses the RF24
           * library's DEFAULT address (0xE7E7E7E7E7) — the same address this
           * blade's sniffer listens on — so the bench can prove TX->RX E2E.
           * autoAck stays off (framework setting); write() returns without
           * waiting for an ACK. Restores the idle state after. */
          uint8_t ch  = (uint8_t)(ext.p1 & 0xFF);
          uint8_t rep = (uint8_t)(ext.p2 & 0xFF); if (rep == 0) rep = 1;
          if (rep > 16) rep = 16;                 /* keep under the deadline */
          uint8_t plen = ext.tail[0];
          if (!g_radioOk || ch > 125 || plen < 1 || plen > 32
              || (uint8_t)len < (uint8_t)(15 + plen)) {
            er.status = FH_OD_BAD_REQ;
          } else {
            radio.stopListening();
            radio.setChannel(ch);
            uint8_t okN = 0;
            for (uint8_t r = 0; r < rep; ++r) {
              if (radio.write(ext.tail + 1, plen)) okN++;
            }
            radio.stopListening();                /* back to idle like init */
            radio.setChannel(g_channel);
            er.status = FH_OD_OK;
            er.data[0] = plen;
            memcpy(er.data + 1, ext.tail + 1, plen);
            er.data[1 + plen] = okN;              /* writes acknowledged */
            er.len = 1 + plen + 1;                /* echo: len byte + payload +
                                                     acknowledged-writes count */
          }
        } else {
          /* EXTERNAL AUTHOR'S SLOT: this verb's implementation is deliberately
           * not authored in the remix build. Replace this block with the real
           * verb code. THE CONTRACT IS FROZEN: read
           * remix-FH-Medusa/EXTERN_CONTRACT.md first (generic REQ/RESP shapes,
           * seq echo, p3 = duration_ms deadline convention). See also
           * remix-FH-P4-hub-firmware/CONTINUITY.md §5b. */
          er.status = FH_OD_EXT_STUB;  /* plumbing proven, verb NOT authored */
        }
        hub_stage((fh_type_t)((uint8_t)type + 1), (uint8_t*)&er, sizeof(er));
      }
      break;   /* not ours */
  }
}

/* ---- SPISlave1 IRQ callbacks — TINY, lock-free, no alloc (vendor contract) -- */
static volatile bool s_tx_done = false;   /* SPSC: set by onDataSent, read in loop() */
static void hub_on_recv(uint8_t *data, size_t len) {
  if (!s_rx_ready) {
    uint16_t n = (len > HUB_XFER_BYTES) ? HUB_XFER_BYTES : (uint16_t)len;
    for (uint16_t i = 0; i < n; ++i) { s_rx_raw[i] = data[i]; }
    s_rx_raw_len = n;
    s_rx_ready = true;
  }
}
static void hub_on_sent() {
  SPISlave1.setData(s_hub_tx, HUB_XFER_BYTES);   /* re-present the built frame */
  s_tx_done = true;                              /* a poll clocked the staged frame */
}

/* Execute a pending NRF24 scan (from loop()). RPD-based presence: the nRF24 has
 * NO real RSSI — only the 1-bit RPD. We sweep [chan_lo..chan_hi], dwell per
 * channel, report the FIRST channel whose RPD latched (0xFF = none). rssi is
 * left 0 by design (carried CORE-3 lesson). */
static void service_pending() {
  if (!(s_req_pending && s_req_type == FH_OD_NRF24_REQ)) { return; }
  fh_od_nrf24_req_t req = s_req_nrf;

  fh_od_nrf24_resp_t resp; memset(&resp, 0, sizeof(resp));
  resp.seq = req.seq;
  resp.active_chan = 0xFF;
  resp.rssi = 0;

  if (!g_radioOk) {
    resp.status = FH_OD_RADIO_FAIL;
  } else if (req.mode == FH_OD_NRF24_MODE_SNIFF) {
    /* SNIFF capture-to-dump: dwell on ONE channel for dwell_ms, capture every
     * payload seen into s_dump_buf (40-byte records: t_ms, len, payload[32],
     * 3 reserved). The chunk stream + summary RESP are staged by
     * service_stream() — one chunk per poll, RESP LAST. */
    uint8_t ch = req.chan_lo;
    if (ch > 125) ch = 125;
    uint32_t window = req.dwell_ms ? req.dwell_ms : 5000;
    if (window > 30000) window = 30000;              /* same clamp as the CC1101 */
    radio.stopListening();
    radio.setChannel(ch);
    radio.startListening();
    s_dump_len = 0;
    uint32_t t0 = millis();
    while ((millis() - t0) < window && s_dump_len < sizeof(s_dump_buf)) {
      if (radio.available()) {
        uint8_t plen = radio.getDynamicPayloadSize();
        if (plen >= 1 && plen <= 32) {               /* reject garbage frames   */
          uint8_t pb[32];
          radio.read(pb, plen);
          uint32_t t = millis() - t0;
          uint8_t *r = s_dump_buf + s_dump_len;
          r[0] = (uint8_t)(t & 0xFF); r[1] = (uint8_t)((t >> 8) & 0xFF);
          r[2] = (uint8_t)((t >> 16) & 0xFF); r[3] = (uint8_t)((t >> 24) & 0xFF);
          r[4] = plen;
          memcpy(r + 5, pb, plen);
          memset(r + 5 + plen, 0, 32 + 3 - plen);    /* pad payload + reserved  */
          s_dump_len += DUMP_REC_NRF24;
        } else {
          radio.flush_rx();                          /* drain the bad frame     */
        }
      }
      delay(1);
    }
    radio.stopListening();
    radio.setChannel(g_channel);
    resp.status = (s_dump_len > 0) ? FH_OD_OK : FH_OD_NO_RESULT;
    resp.active_chan = ch;
  } else {
    uint8_t lo = req.chan_lo, hi = req.chan_hi;
    if (hi > 125) hi = 125;
    if (lo > hi) { uint8_t t = lo; lo = hi; hi = t; }
    uint16_t dwell = req.dwell_ms ? req.dwell_ms : 2;
    radio.stopListening();
    bool found = false;
    for (uint8_t ch = lo; ch <= hi && !found; ++ch) {
      radio.setChannel(ch);
      radio.startListening();
      delay(dwell > 10 ? 10 : dwell);
      if (radio.testCarrier() || radio.testRPD()) {
        resp.active_chan = ch;
        found = true;
      }
      radio.stopListening();
    }
    radio.setChannel(g_channel);
    resp.status = found ? FH_OD_OK : FH_OD_NO_RESULT;
  }

  if (req.mode == FH_OD_NRF24_MODE_SNIFF && resp.status == FH_OD_OK) {
    /* Capture-to-dump: stream the chunks BEFORE the summary RESP. */
    s_dump_resp = resp;
    s_dump_chunk = 0;
    s_streaming = true;
    return;                                  /* service_stream() paces it out */
  }

  hub_stage((fh_type_t)FH_OD_NRF24_RESP, (uint8_t*)&resp, sizeof(resp));
  s_req_pending = false;
}

/* Stream one capture-to-dump chunk per poll transaction (loop ctx). Advances
 * ONLY when hub_on_sent (IRQ) set s_tx_done — proof the previously staged
 * chunk left on the wire. The summary RESP is staged LAST (the hub's flight
 * completes on it). */
static void service_stream(void) {
  if (!s_streaming) { return; }
  if (!s_tx_done) { return; }                /* wait for the poll that clocked it */
  s_tx_done = false;
  uint16_t off = (uint16_t)s_dump_chunk * (uint16_t)DUMP_CHUNK_DATA_MAX;
  if (off >= s_dump_len) {                   /* all chunks clocked: RESP last */
    hub_stage((fh_type_t)FH_OD_NRF24_RESP, (uint8_t*)&s_dump_resp, sizeof(s_dump_resp));
    s_streaming = false;
    s_dump_len = 0;
    s_dump_chunk = 0;
    s_req_pending = false;                   /* op complete — accept the next */
    return;
  }
  fh_od_dump_ind_t ind; memset(&ind, 0, sizeof(ind));
  ind.seq   = s_dump_resp.seq;
  ind.radio = FH_DUMP_RADIO_NRF24;
  ind.chunk = s_dump_chunk;
  uint16_t n = (uint16_t)(s_dump_len - off);
  if (n > DUMP_CHUNK_DATA_MAX) n = DUMP_CHUNK_DATA_MAX;
  memcpy(ind.data, s_dump_buf + off, n);
  ind.len  = (uint8_t)n;
  ind.more = (uint8_t)((off + n) < s_dump_len);
  hub_stage((fh_type_t)FH_OD_DUMP_IND, (uint8_t*)&ind, sizeof(ind));
  s_dump_chunk++;
}

/* Drain inbound raw bytes copied out of the IRQ and decode them (loop ctx). */
static void service_rx() {
  if (!s_rx_ready) { return; }
  uint8_t buf[HUB_XFER_BYTES]; uint16_t n = s_rx_raw_len;
  for (uint16_t i = 0; i < n; ++i) { buf[i] = s_rx_raw[i]; }
  s_rx_ready = false;
  fh_decoder_push(&s_in_dec, buf, n, on_hub_frame, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ARDUINO ENTRY POINTS
 * ═══════════════════════════════════════════════════════════════════════════ */
void setup() {
  Serial.begin(115200);                  /* USB debug console only */
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 1500)) { /* brief wait */ }

  fh_decoder_init(&s_in_dec);

  radioInit();                           /* carried radio core (SPI0 master) */

  pinMode(HUBLINK_IRQ, OUTPUT);
  digitalWrite(HUBLINK_IRQ, LOW);
  SPISlave1.setSCK(HUBLINK_SCK);
  SPISlave1.setTX(HUBLINK_TX);
  SPISlave1.setRX(HUBLINK_RX);
  SPISlave1.setCS(HUBLINK_CS);
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

  Serial.print(F("remix-fh-pico-nrf24 v1 (SPI) boot  radio="));
  Serial.println(g_radioOk ? F("ok") : F("ABSENT (check 3.3V + CE/CSN + SPI0)"));
}

void loop() {
  service_rx();                          /* decode any inbound frame (IRQ copied raw) */
  service_pending();
  service_stream();                      /* pace one dump chunk per poll */

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 's') {
      uint8_t ch = g_radioOk ? radio.getChannel() : g_channel;
      Serial.print(F("status radio=")); Serial.print(g_radioOk ? F("ok") : F("absent"));
      Serial.print(F(" channel=")); Serial.print(ch);
      Serial.print(F(" variant=")); Serial.println(g_isPlusPA ? F("+PA+LNA") : F("standard"));
    }
  }
  delay(2);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  KNOWN RISKS (UNVERIFIED — first build + bench)
 *   [N1] arduino-pico SPISlave1 API (SPI1 block as slave): verify SPISlave1
 *        exists + the setter/callback names on YOUR core. #1 compile-fix. The
 *        radio is SPI0 master (RF24 on GP18/19/16), so the hub slave must be SPI1.
 *   [N2] TWO SPI BLOCKS: radio = SPI0 master, hub = SPI1 slave. HUBLINK pins
 *        (GP10/11/12/13 + IRQ GP8) are a DRAFT — verify a valid spi1 group and
 *        no clash with the radio pins (GP16/17/18/19/20).
 *   [N3] NO REAL RSSI (carried CORE-3 lesson): the scan uses RPD/testCarrier —
 *        a 1-bit carrier-present flag — and reports the active CHANNEL, not a
 *        dBm. resp.rssi stays 0 deliberately. testRPD() is P-variant; testCarrier()
 *        is the non-P fallback — we try both. Verify names on your RF24 version.
 *   [N4] FRAMEWORK ONLY: this proves the radio + the link. No sniffer/injection
 *        modules — those are later head-gated additions. The scan is a minimal
 *        presence probe to exercise the REQ/RESP path, not a capture module.
 *   [N5] RADIO CORE (radioInit) CARRIED VERBATIM from the proven gen1 framework
 *        blade (incl. the radio.begin(&SPI) binding — verified vs RF24 ref).
 *   [N7] SNIFF capture-to-dump (Tier 2, 2026-10-31): mode 1 (FH_OD_NRF24_MODE_SNIFF)
 *        dwells on ONE channel and captures every payload seen (1..32 B) as
 *        40-byte records (u32 t_ms LE + u8 len + payload[32] + 3 reserved),
 *        streamed as FH_OD_DUMP_IND (0x2F) chunks, one per poll transaction,
 *        paced by s_tx_done from hub_on_sent(); the summary RESP is staged
 *        LAST. The dump store lives on Medusa (chunk reassembly by seq).
 *        Bench-verify: sniff with a real transmitter on a known channel and
 *        confirm Medusa's dump shows the payloads.
 *   [N8] USB Serial is a DEBUG console only; binary frames go over SPI1 raw.
 * ═══════════════════════════════════════════════════════════════════════════ */
