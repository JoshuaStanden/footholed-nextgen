/* ═══════════════════════════════════════════════════════════════════════════
 *  remix-fh-pico-cc1101  —  FH REMIX CC1101 SubGHz blade (SPI slave on Bus B)
 * ───────────────────────────────────────────────────────────────────────────
 *  Board tier : "pico"  — original Raspberry Pi Pico (RP2040)
 *  Radio      : CC1101 "blue board" V2 (8-pin, SMA), ASK/OOK sub-GHz
 *  Core       : arduino-pico (earlephilhower)   https://github.com/earlephilhower/arduino-pico
 *  Status     : UNVERIFIED — reasoned-not-compiled. NOT bench-tested (project rule).
 *  Capture    : GPIO edge interrupt + micros() timestamping (NO PIO in v0).
 *
 *  WHAT THIS IS
 *  ------------
 *  A single-radio "blade": one Pico owns one CC1101, captures/decodes/replays
 *  fixed-code OOK remote signals. In REMIX it is a SPI SLAVE on the P4 hub's
 *  Bus B, speaking the BINARY fh_protocol (fh_frame + fh_ondemand) that the
 *  rest of the system uses — NOT the gen1 ASCII v0. The hub FIRES an on-demand
 *  request (FH_OD_CC1101_REQ) and the blade answers with FH_OD_CC1101_RESP.
 *  One op at a time (the frozen BUSY contract).
 *
 *  THE RADIO CORE IS CARRIED VERBATIM from the proven gen1 blade (timing table,
 *  interrupt capture, decode, OOK TX bit-bang). ONLY the transport changed:
 *  ASCII-lines-over-USB-Serial  ->  binary fh_frame over an SPI slave link.
 *  USB `Serial` is KEPT as a LOCAL DEBUG CONSOLE (human-readable), so the bench
 *  debuggability the gen1 authors valued survives — but it is NOT the hub link.
 *
 *  WHY INTERRUPT CAPTURE (not PIO) IN v0
 *  -------------------------------------
 *  v0 uses the RCSwitch technique: a pin-change interrupt on GDO0 that
 *  timestamps each edge with micros(). Simpler, correct-by-construction, good
 *  enough to prove the blade. PIO is a later optimization. (A hand-assembled
 *  PIO draft was REJECTED earlier — do not resurrect.)
 *
 *  WHAT THIS IS *NOT*
 *  ------------------
 *  - Not a jammer. No continuous-carrier / noise flood mode.
 *  - Not gated here: authorization/scope lives on the HEAD/hub. The blade only
 *    transmits when the hub sends a replay/send request.
 *
 *  AUTHORIZED USE ONLY. Sub-GHz TX is lawful only against gear you own or are
 *  explicitly, in-writing authorized to test. Capture is passive.
 *
 *  PORTING CREDIT (MIT): OOK protocol timing table and the capture->decode
 *  approach are adapted from HaleHound-CYD (Jesse C. Hale) / ESP32-DIV
 *  (CiferTech), MIT. See README for the notice to retain on redistribution.
 *  ═══════════════════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <SPI.h>
#include <SPISlave.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>   // SmartRC-CC1101-Driver-Lib (ELECHOUSE API)

extern "C" {
  #include "fh_frame.h"
  #include "fh_ondemand.h"
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  PIN MAP  —  CC1101 "blue board" <-> Pico (RP2040), SPI0
 * ───────────────────────────────────────────────────────────────────────────
 *  Pins are LOCAL to the blade (PoC choices). Wire the module to match.
 *  CC1101 VCC is 3.3V ONLY — Pico 3V3(OUT) pin 36, NEVER 5V/VBUS (no onboard
 *  regulator; >3.6V destroys it).
 *
 *    CC1101 pin   Pico GPIO   Pico phys pin   Notes
 *    VCC          3V3(OUT)    36              3.3V ONLY
 *    GND          GND         any GND         common ground
 *    SCK          GP18        24              SPI0 SCK
 *    MOSI (SI)    GP19        25              SPI0 TX
 *    MISO (SO)    GP16        21              SPI0 RX
 *    CSN (CS)     GP17        22              chip select (sometimes silk "CKN")
 *    GDO0         GP20        26              RX data (capture IRQ) + TX bit-bang
 *    GDO2         GP21        27              reserved (unused in v0)
 * ═══════════════════════════════════════════════════════════════════════════ */
static const uint8_t PIN_SCK  = 18;
static const uint8_t PIN_MOSI = 19;
static const uint8_t PIN_MISO = 16;
static const uint8_t PIN_CSN  = 17;
static const uint8_t PIN_GDO0 = 20;
static const uint8_t PIN_GDO2 = 21;

/* ═══════════════════════════════════════════════════════════════════════════
 *  RADIO DEFAULTS
 * ═══════════════════════════════════════════════════════════════════════════ */
static float       g_freqMHz = 433.92f;  // US OOK remotes live at 315/433, not 915
static const float FREQ_MIN  = 300.0f;
static const float FREQ_MAX  = 928.0f;

/* ═══════════════════════════════════════════════════════════════════════════
 *  OOK PROTOCOL TIMING TABLE  (adapted from RCSwitch/HaleHound)
 *  Each: base pulse "T" (us) + high/low tick ratios for sync, '0', '1'.
 *  `inverted` flips active/idle levels. Describes FIXED-CODE remotes.
 * ═══════════════════════════════════════════════════════════════════════════ */
struct RCProtocol {
  uint16_t pulseLength;
  uint8_t  syncHigh, syncLow;
  uint8_t  zeroHigh, zeroLow;
  uint8_t  oneHigh,  oneLow;
  bool     inverted;
};

static const RCProtocol RC_PROTOCOLS[] = {
  {   0,  0,  0, 0,  0, 0,  0, false }, // 0: placeholder (keeps indices aligned)
  { 350,  1, 31, 1,  3, 3,  1, false }, // 1: PT2262 / EV1527 — most common
  { 650,  1, 10, 1,  2, 2,  1, false }, // 2: KlikAanKlikUit
  { 100, 30, 71, 4, 11, 9,  6, false }, // 3: tri-state
  { 380,  1,  6, 1,  3, 3,  1, false }, // 4: Intertechno
  { 500,  6, 14, 1,  2, 2,  1, false }, // 5: TPI/I-C sockets
  { 450, 23,  1, 1,  2, 2,  1, true  }, // 6: HT6P20B / Conrad RSL (inv)
  { 150,  2, 62, 1,  6, 6,  1, false }, // 7: HS2303-PT
  { 200,  3,130, 7, 16, 3, 16, false }, // 8
  { 200,130,  7,16,  7,16,  3, true  }, // 9 (inv)
  { 365, 18,  1, 3,  1, 1,  3, true  }, // 10: Brennenstuhl (inv)
  { 270, 36,  1, 1,  2, 2,  1, true  }, // 11 (inv)
  { 320, 36,  1, 1,  2, 2,  1, true  }, // 12 (inv)
};
static const int NUM_RC_PROTOCOLS = (int)(sizeof(RC_PROTOCOLS) / sizeof(RC_PROTOCOLS[0]));

/* ═══════════════════════════════════════════════════════════════════════════
 *  EDGE-TIMING CAPTURE  (interrupt + micros)
 * ───────────────────────────────────────────────────────────────────────────
 *  On every GDO0 edge, the ISR records the time since the previous edge into a
 *  ring of "pulse durations" (microseconds). Each entry is one half-pulse. We
 *  also record the LEVEL the line was AT during that duration, so the decoder
 *  never has to guess phase (fixes the phase-alignment bug from the PIO draft).
 *
 *  Concurrency: the ISR writes g_edgeDur/g_edgeLvl and advances g_isrHead. The
 *  foreground copies a snapshot with interrupts briefly masked. Counters are
 *  volatile; the buffer is only *read* by foreground after capture stops.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define MAX_EDGES 512
static volatile uint32_t g_edgeDur[MAX_EDGES];   // duration of each half-pulse (us)
static volatile uint8_t  g_edgeLvl[MAX_EDGES];   // line level DURING that duration (0/1)
static volatile uint16_t g_isrHead = 0;          // next write index (wraps)
static volatile uint32_t g_lastEdgeUs = 0;       // timestamp of previous edge
static volatile bool     g_capturing = false;

// Snapshot copied out of the volatile ring for decoding (single-threaded use).
static uint32_t g_dur[MAX_EDGES];
static uint8_t  g_lvl[MAX_EDGES];
static int      g_edgeCount = 0;

// ISR: runs on CHANGE of GDO0. Records how long the line was at its PREVIOUS
// level (i.e. the level it held until this edge), then resets the timer.
static void gdo0ISR() {
  uint32_t now = micros();
  if (!g_capturing) { g_lastEdgeUs = now; return; }

  uint32_t delta = now - g_lastEdgeUs;   // unsigned subtraction handles wrap
  g_lastEdgeUs = now;

  // The level the line HELD during 'delta' is the OPPOSITE of its level now
  // (it just changed). Read current pin and invert.
  uint8_t levelNow = (uint8_t)digitalRead(PIN_GDO0);
  uint8_t heldLevel = levelNow ? 0 : 1;

  uint16_t h = g_isrHead;
  if (h < MAX_EDGES) {            // stop recording when full; foreground drains
    g_edgeDur[h] = delta;
    g_edgeLvl[h] = heldLevel;
    g_isrHead = h + 1;
  }
}

static void captureStart() {
  noInterrupts();
  g_isrHead = 0;
  g_lastEdgeUs = micros();
  g_capturing = true;
  interrupts();
}

static void captureStop() {
  g_capturing = false;
}

// Copy the volatile ring into the plain snapshot buffers. Returns count.
static int captureSnapshot() {
  noInterrupts();
  int n = (int)g_isrHead;
  if (n > MAX_EDGES) n = MAX_EDGES;
  for (int i = 0; i < n; i++) { g_dur[i] = g_edgeDur[i]; g_lvl[i] = g_edgeLvl[i]; }
  interrupts();
  g_edgeCount = n;
  return n;
}

// Run a capture window: start IRQ recording, wait until either the window
// elapses or a long idle gap follows some activity, then stop and snapshot.
static int captureFrame(uint32_t windowMs) {
  captureStart();
  uint32_t start = millis();
  const uint32_t IDLE_GAP_US = 12000;   // 12 ms silence after activity => done

  while ((millis() - start) < windowMs) {
    uint16_t head = g_isrHead;
    if (head >= MAX_EDGES) break;                       // buffer full
    if (head > 16) {                                    // some activity seen
      noInterrupts();
      uint32_t since = micros() - g_lastEdgeUs;
      interrupts();
      if (since > IDLE_GAP_US) break;                   // packet boundary
    }
    tight_loop_contents();
  }
  captureStop();
  return captureSnapshot();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  DECODE  —  durations+levels -> {value, bits, protocol}
 * ───────────────────────────────────────────────────────────────────────────
 *  We now KNOW each entry's level, so we locate bit boundaries by pairing a
 *  HIGH half-pulse with the following LOW half-pulse (or inverted), rather than
 *  assuming even/odd phase. This removes the phase-alignment guess.
 * ═══════════════════════════════════════════════════════════════════════════ */
static bool pulseMatch(uint32_t dur, uint32_t target, uint8_t tolPct = 60) {
  if (target == 0) return false;
  uint32_t tol = (target * tolPct) / 100;
  uint32_t lo  = (tol >= target) ? 0 : (target - tol);   // guard underflow (M1)
  return (dur >= lo) && (dur <= (target + tol));
}

// Find index of the first HIGH (level==1) entry at or after 'from'.
static int nextHigh(int from) {
  for (int i = from; i < g_edgeCount; i++) if (g_lvl[i] == 1) return i;
  return -1;
}

static bool decodeSignal(unsigned long* outValue, int* outBits, int* outProto) {
  if (g_edgeCount < 16) return false;

  for (int p = 1; p < NUM_RC_PROTOCOLS; p++) {
    const RCProtocol& pr = RC_PROTOCOLS[p];
    if (pr.pulseLength == 0) continue;

    uint32_t T   = pr.pulseLength;
    uint32_t sHi = T * pr.syncHigh, sLo = T * pr.syncLow;
    uint32_t zHi = T * pr.zeroHigh, zLo = T * pr.zeroLow;
    uint32_t oHi = T * pr.oneHigh,  oLo = T * pr.oneLow;

    // Walk, always anchoring on a HIGH entry followed by a LOW entry.
    for (int i = 0; i + 1 < g_edgeCount; i++) {
      if (g_lvl[i] != 1 || g_lvl[i + 1] != 0) continue;  // want HIGH then LOW
      uint32_t hi = g_dur[i];
      uint32_t lo = g_dur[i + 1];

      // For inverted protocols the "sync/bit" high/low roles swap.
      bool syncFound = pr.inverted
        ? (pulseMatch(lo, sHi, 70) && pulseMatch(hi, sLo, 70))
        : (pulseMatch(hi, sHi, 70) && pulseMatch(lo, sLo, 70));
      if (!syncFound) continue;

      unsigned long value = 0;
      int bits = 0;
      int j = i + 2;
      // Re-align to next HIGH after the sync for the first bit.
      j = nextHigh(j);
      while (j >= 0 && j + 1 < g_edgeCount && bits < 32) {
        if (g_lvl[j] != 1 || g_lvl[j + 1] != 0) { j = nextHigh(j + 1); continue; }
        uint32_t h = g_dur[j];
        uint32_t l = g_dur[j + 1];
        if (l > T * 10) break;   // long low => end of packet

        bool isZero, isOne;
        if (!pr.inverted) {
          isZero = pulseMatch(h, zHi) && pulseMatch(l, zLo);
          isOne  = pulseMatch(h, oHi) && pulseMatch(l, oLo);
        } else {
          isZero = pulseMatch(l, zHi) && pulseMatch(h, zLo);
          isOne  = pulseMatch(l, oHi) && pulseMatch(h, oLo);
        }

        if (isZero)     { value <<= 1;            bits++; }
        else if (isOne) { value = (value << 1)|1; bits++; }
        else break;
        j += 2;
      }

      if (bits >= 8) {   // allow all-zero codes (M2 fix): bits is the real guard
        *outValue = value; *outBits = bits; *outProto = p;
        return true;
      }
    }
  }
  return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  CC1101 CONTROL (ELECHOUSE library)
 * ═══════════════════════════════════════════════════════════════════════════ */
static bool g_radioOk = false;

static void radioInit() {
  SPI.setRX(PIN_MISO);
  SPI.setTX(PIN_MOSI);
  SPI.setSCK(PIN_SCK);
  SPI.begin();

  ELECHOUSE_cc1101.setSpiPin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);
  ELECHOUSE_cc1101.setGDO(PIN_GDO0, PIN_GDO2);

  if (ELECHOUSE_cc1101.getCC1101()) {
    g_radioOk = true;
    ELECHOUSE_cc1101.Init();
    ELECHOUSE_cc1101.setCCMode(0);      // async/raw serial mode (data on GDO0)
    ELECHOUSE_cc1101.setModulation(2);  // ASK/OOK
    ELECHOUSE_cc1101.setMHZ(g_freqMHz);
    ELECHOUSE_cc1101.SetRx();
  } else {
    g_radioOk = false;
  }
}

// Enter RX and (re)attach the capture interrupt. We claim GDO0 as INPUT and
// attach the ISR HERE (not in setup) so TX, which drives GDO0 as OUTPUT, can
// cleanly hand the pin back to capture afterwards (fixes H3 ownership).
static void radioEnterRx() {
  if (!g_radioOk) return;
  detachInterrupt(digitalPinToInterrupt(PIN_GDO0));  // idempotent-safe
  ELECHOUSE_cc1101.setSidle();
  ELECHOUSE_cc1101.SetRx();
  pinMode(PIN_GDO0, INPUT);
  g_lastEdgeUs = micros();
  attachInterrupt(digitalPinToInterrupt(PIN_GDO0), gdo0ISR, CHANGE);
}

// Enter TX: detach capture IRQ and take GDO0 as OUTPUT for bit-banging (H2/H3).
static void radioEnterTx() {
  if (!g_radioOk) return;
  detachInterrupt(digitalPinToInterrupt(PIN_GDO0));
  g_capturing = false;
  pinMode(PIN_GDO0, OUTPUT);
  digitalWrite(PIN_GDO0, LOW);
  ELECHOUSE_cc1101.setPA(12);    // max PA (authorized-use only)
  ELECHOUSE_cc1101.SetTx();
}

static void radioSetFreq(float mhz) {
  if (mhz < FREQ_MIN || mhz > FREQ_MAX) return;
  g_freqMHz = mhz;
  if (!g_radioOk) return;
  // H4 fix: idle the radio, retune, then ONE clean RX re-entry (which also
  // re-attaches the capture IRQ). No capture runs during the retune window.
  detachInterrupt(digitalPinToInterrupt(PIN_GDO0));
  g_capturing = false;
  ELECHOUSE_cc1101.setSidle();
  ELECHOUSE_cc1101.setMHZ(g_freqMHz);
  radioEnterRx();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TRANSMIT (REPLAY / SEND)  —  bit-bang GDO0 in async OOK TX mode
 * ═══════════════════════════════════════════════════════════════════════════ */
static void transmitCode(unsigned long value, int bits, int proto, int repeats) {
  if (proto < 1 || proto >= NUM_RC_PROTOCOLS) return;
  const RCProtocol& pr = RC_PROTOCOLS[proto];
  if (pr.pulseLength == 0 || bits <= 0 || bits > 32) return;

  uint8_t hiLevel = pr.inverted ? LOW : HIGH;
  uint8_t loLevel = pr.inverted ? HIGH : LOW;

  radioEnterTx();

  for (int r = 0; r < repeats; r++) {
    for (int b = bits - 1; b >= 0; b--) {
      bool one = (value >> b) & 1UL;
      uint16_t hT = one ? pr.oneHigh : pr.zeroHigh;
      uint16_t lT = one ? pr.oneLow  : pr.zeroLow;
      digitalWrite(PIN_GDO0, hiLevel);
      delayMicroseconds((uint32_t)pr.pulseLength * hT);
      digitalWrite(PIN_GDO0, loLevel);
      delayMicroseconds((uint32_t)pr.pulseLength * lT);
    }
    digitalWrite(PIN_GDO0, hiLevel);
    delayMicroseconds((uint32_t)pr.pulseLength * pr.syncHigh);
    digitalWrite(PIN_GDO0, loLevel);
    delayMicroseconds((uint32_t)pr.pulseLength * pr.syncLow);
  }

  digitalWrite(PIN_GDO0, loLevel);
  radioEnterRx();                 // hand GDO0 back to capture
}


/* ═══════════════════════════════════════════════════════════════════════════
 *  HUB LINK  —  binary fh_protocol over an SPI SLAVE on the P4 hub's Bus B
 * ───────────────────────────────────────────────────────────────────────────
 *  REPLACES the gen1 ASCII-lines-over-USB protocol. The hub (SPI master) FIRES
 *  an on-demand request; this blade runs ONE radio op and answers with a
 *  binary fh_frame. The radio core above is unchanged.
 *
 *  PIN SEPARATION (critical): the CC1101 radio uses hardware SPI0 (GP18/19/16)
 *  as MASTER to talk to the chip. The HUB LINK is a separate SPI SLAVE on
 *  hardware SPI1 (different pins) — the two SPI blocks never collide. The RP2040
 *  has exactly two SPI blocks; this uses both (spi0=radio master, spi1=hub slave).
 *
 *  BUS-B / FIRED MODEL: the hub fires a request, the blade does the (possibly
 *  multi-second) capture, then stages the response for the hub to clock on the
 *  next transaction. Because capture can be long, we DECODE the request on one
 *  transaction, run the op in loop(), and present the RESP on a later poll — the
 *  hub matches by seq. (One op at a time = the frozen BUSY contract.)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Hub-link SPI1 slave pins (SEPARATE from the radio's SPI0). VERIFY against the
 * hub's Bus B wiring + a valid spi1 pin group on your Pico. */
static const uint8_t HUBLINK_SCK  = 10;   // GP10 — spi1 SCK  (hub drives)
static const uint8_t HUBLINK_RX   = 12;   // GP12 — spi1 RX = MOSI (hub->blade)
static const uint8_t HUBLINK_TX   = 11;   // GP11 — spi1 TX = MISO (blade->hub)
static const uint8_t HUBLINK_CS   = 13;   // GP13 — spi1 CSn  (hub selects)
static const uint8_t HUBLINK_IRQ  =  8;   // GP8  — "result ready" out to hub (optional)

#define HUB_XFER_BYTES   FH_MAX_FRAME     /* 261, matches the hub master */

static fh_decoder_t s_in_dec;             /* decode inbound requests (hub->blade) */
static uint8_t  s_hub_tx[HUB_XFER_BYTES]; /* staged outbound frame                */

/* ---- RAW OOK capture-to-dump (Tier 2, FH_OD_DUMP_IND 0x2F chunk stream) ---
 * A RAW_OOK (action 2) capture serializes the edge buffer (g_dur/g_lvl from
 * the carried core: u32 dur_us LE + u8 level, 5 B/edge, MAX_EDGES 512) into
 * s_dump_buf, then STREAMS it to the hub as FH_OD_DUMP_IND chunks BEFORE the
 * summary RESP. The hub's flight completes on the RESP, so every chunk must
 * already be clocked out by then. Pacing: ONE chunk per poll transaction —
 * hub_on_sent (IRQ) sets s_tx_done; loop()'s service_stream() advances only
 * after a poll clocked the staged chunk, so a chunk is never overwritten
 * before it left the wire. */
#define DUMP_CHUNK_DATA_MAX 250u              /* dump_ind data[] capacity      */
static uint8_t  s_dump_buf[MAX_EDGES * 5];    /* u32 dur_us LE + u8 level      */
static uint16_t s_dump_len   = 0;             /* bytes valid in s_dump_buf     */
static uint8_t  s_dump_chunk = 0;             /* index of the next chunk to build */
static bool     s_streaming  = false;
static fh_od_cc1101_resp_t s_dump_resp;       /* summary RESP staged LAST      */

/* One pending on-demand request, decoded + executed in loop() (single-core). */
static volatile bool s_req_pending = false;
static uint8_t  s_req_type = 0;
static uint8_t  s_req_seq  = 0;
static fh_od_cc1101_req_t s_req_cc;       /* copy of the last CC1101 request       */

/* Inbound raw staging (SPSC: the recv IRQ writes, loop() reads). The IRQ stays
 * tiny per the arduino-pico SPISlave contract (IRQ-time: small, fast, no alloc,
 * NO locking) — all decode/encode happens in loop(). */
static volatile uint8_t  s_rx_raw[HUB_XFER_BYTES];
static volatile uint16_t s_rx_raw_len = 0;
static volatile bool     s_rx_ready   = false;

/* Build an fh_frame into the outbound slot for the hub's next read. LOOP-CTX
 * ONLY (never the IRQ): the IRQ's onDataSent merely re-presents s_hub_tx. No
 * lock — s_hub_tx is written only here (single-core) and read only by the IRQ. */
static void hub_stage(fh_type_t type, const uint8_t *payload, uint8_t len) {
  memset(s_hub_tx, 0, sizeof(s_hub_tx));
  fh_frame_encode(type, payload, len, s_hub_tx, sizeof(s_hub_tx));
  SPISlave1.setData(s_hub_tx, HUB_XFER_BYTES);   /* present on the next clock */
  digitalWrite(HUBLINK_IRQ, HIGH);               /* optional ready signal */
}

/* inbound request handler — LOOP CONTEXT (called from loop() after the IRQ
 * copied raw bytes out). Safe to decode/encode/stage here. */
static void on_hub_frame(fh_type_t type, const uint8_t *payload, uint8_t len, void *user) {
  (void)user;
  switch ((uint8_t)type) {
    case FH_OD_PING_REQ: {
      fh_od_ping_resp_t r; memset(&r, 0, sizeof(r));
      if (len >= 1) r.seq = payload[0];
      r.radios_ready = g_radioOk ? FH_OD_RADIO_CC1101 : 0;
      hub_stage((fh_type_t)FH_OD_PING_RESP, (uint8_t*)&r, sizeof(r));
      break;
    }
    case FH_OD_CC1101_REQ: {
      if (len >= sizeof(fh_od_cc1101_req_t)) {
        if (!s_req_pending) {                 /* BUSY: one op at a time */
          memcpy(&s_req_cc, payload, sizeof(s_req_cc));
          s_req_type = FH_OD_CC1101_REQ;
          s_req_seq  = s_req_cc.seq;
          s_req_pending = true;
        } else {
          fh_od_cc1101_resp_t r; memset(&r, 0, sizeof(r));
          r.seq = payload[0]; r.status = FH_OD_BUSY;
          hub_stage((fh_type_t)FH_OD_CC1101_RESP, (uint8_t*)&r, sizeof(r));
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
       * 0x30..0x37 EVEN = this blade's extern REQs (frozen: JAM 0x30, BRUTE
       * 0x32; 0x34..0x36 reserved for future authors). A REQ here is decoded
       * with the generic fh_od_ext_req_t and answered with the generic
       * fh_od_ext_resp_t. THIS STUB answers FH_OD_EXT_STUB so the bench can
       * prove the routing end-to-end; the EXTERNAL AUTHOR replaces this case
       * with the real implementation (jam/brute are transmit-disruption
       * primitives deliberately not authored in the remix build). The hub's
       * flight completes on the RESP; p3 (bytes 10..13 LE) = duration_ms.  */
      if ((uint8_t)type >= FH_OD_EXT_CC_JAM_REQ && (uint8_t)type <= 0x37
          && (((uint8_t)type & 1u) == 0) && len >= 15) {
        fh_od_ext_req_t ext; memset(&ext, 0, sizeof(ext));
        memcpy(&ext, payload, len > sizeof(ext) ? sizeof(ext) : len);
        fh_od_ext_resp_t er; memset(&er, 0, sizeof(er));
        er.seq = ext.seq;
        if ((uint8_t)type == FH_OD_EXT_CC_TX_REQ) {
          /* CC1101 TX (IMPLEMENTED): transmit author-specified OOK bits using
           * the same transmitCode() core REPLAY carries. p1=freq_khz, p2=bits
           * (1..32), p3=0. tail[0]=proto, tail[1..4]=value u32 LE,
           * tail[5]=repeats (0 -> 3). Responds with a 6-byte echo of what was
           * transmitted. AUTHORIZATION: fires only from a hub REQ sent by an
           * authorized operator in Medusa — for driving YOUR OWN devices. */
          uint8_t proto  = ext.tail[0];
          uint32_t value = (uint32_t)ext.tail[1] | ((uint32_t)ext.tail[2] << 8)
                         | ((uint32_t)ext.tail[3] << 16)
                         | ((uint32_t)ext.tail[4] << 24);
          uint8_t bits   = (uint8_t)(ext.p2 & 0xFF);
          uint8_t reps   = ext.tail[5]; if (reps == 0) reps = 3;
          if (!g_radioOk || bits < 1 || bits > 32 || proto == 0
              || proto >= NUM_RC_PROTOCOLS) {
            er.status = FH_OD_BAD_REQ;
          } else {
            float mhz = (float)(ext.p1) / 1000.0f;
            if (mhz >= FREQ_MIN && mhz <= FREQ_MAX) radioSetFreq(mhz);
            transmitCode(value, bits, proto, reps);
            er.status = FH_OD_OK;
            er.len = 6;                 /* echo: proto, bits, value LE */
            er.data[0] = proto;
            er.data[1] = bits;
            er.data[2] = (uint8_t)(value & 0xFF);
            er.data[3] = (uint8_t)((value >> 8) & 0xFF);
            er.data[4] = (uint8_t)((value >> 16) & 0xFF);
            er.data[5] = (uint8_t)((value >> 24) & 0xFF);
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

/* ---- SPISlave1 IRQ callbacks — TINY, lock-free, no alloc (vendor contract) --
 * onDataRecv: copy raw bytes out for loop() to decode. onDataSent: re-present
 * the loop-built s_hub_tx (do NOT clobber it with idle — that dropped replies). */
static void hub_on_recv(uint8_t *data, size_t len) {
  if (!s_rx_ready) {                       /* SPSC: skip if loop() hasn't drained */
    uint16_t n = (len > HUB_XFER_BYTES) ? HUB_XFER_BYTES : (uint16_t)len;
    for (uint16_t i = 0; i < n; ++i) { s_rx_raw[i] = data[i]; }
    s_rx_raw_len = n;
    s_rx_ready = true;
  }
}
static volatile bool s_tx_done = false;   /* SPSC: set by onDataSent, read in loop() */
static void hub_on_sent() {
  SPISlave1.setData(s_hub_tx, HUB_XFER_BYTES);   /* re-present the built frame */
  s_tx_done = true;                        /* a poll clocked the staged frame out */
}

/* Execute a pending CC1101 request (loop() — a capture can take seconds).
 * Uses the carried radio core verbatim. */
static void service_pending() {
  if (!(s_req_pending && s_req_type == FH_OD_CC1101_REQ)) { return; }
  fh_od_cc1101_req_t req = s_req_cc;

  fh_od_cc1101_resp_t resp; memset(&resp, 0, sizeof(resp));
  resp.seq = req.seq;

  if (!g_radioOk) {
    resp.status = FH_OD_RADIO_FAIL;
  } else if (req.action == FH_OD_CC1101_ACT_REPLAY) {
    /* REPLAY (action 3): re-transmit the LAST captured signal (this blade's
     * g_lastValue/g_lastBits/g_lastProto — captured by a previous LISTEN/RX_ONE
     * REQ) at req.freq_khz. The triple travels in the RESP data[] of that
     * earlier capture (data[0]=proto, [1]=bits, [2..5]=value LE) so the host
     * and hub know exactly what will be re-emitted. transmitCode() re-enters
     * RX afterwards (radioEnterRx at its end), so capture can resume.
     * AUTHORIZATION: like every TX on this blade, this only fires when the
     * hub (driven by an authorized operator in Medusa) sends the REQ. */
    if (!g_haveCapture) {
      resp.status = FH_OD_NO_RESULT;      /* nothing captured yet to replay */
    } else {
      float mhz = (float)req.freq_khz / 1000.0f;
      if (mhz >= FREQ_MIN && mhz <= FREQ_MAX) radioSetFreq(mhz);
      transmitCode(g_lastValue, g_lastBits, g_lastProto, 3);  /* carried core */
      resp.status = FH_OD_OK;
      resp.data[0] = (uint8_t)g_lastProto;
      resp.data[1] = (uint8_t)g_lastBits;
      resp.data[2] = (uint8_t)(g_lastValue & 0xFF);
      resp.data[3] = (uint8_t)((g_lastValue >> 8) & 0xFF);
      resp.data[4] = (uint8_t)((g_lastValue >> 16) & 0xFF);
      resp.data[5] = (uint8_t)((g_lastValue >> 24) & 0xFF);
      resp.len = 6;                       /* echoes what was re-emitted */
      /* dwell_ms is IGNORED for REPLAY — it is a listen parameter, and the
       * hub's job_duration_ms() must not count a dwell for a TX job. */
    }
  } else {
    /* set frequency from the request (kHz -> MHz) if sane */
    float mhz = (float)req.freq_khz / 1000.0f;
    if (mhz >= FREQ_MIN && mhz <= FREQ_MAX) radioSetFreq(mhz);
    uint32_t window = req.dwell_ms ? req.dwell_ms : 4000;
    if (window > 30000) window = 30000;

    int n = captureFrame(window);               /* carried radio core */
    if (n <= 0) {
      resp.status = FH_OD_NO_RESULT;
    } else {
      unsigned long v; int b, p;
      if (decodeSignal(&v, &b, &p)) {            /* carried radio core */
        g_lastValue = v; g_lastBits = b; g_lastProto = p; g_haveCapture = true;
        resp.status = FH_OD_OK;
        resp.data[0] = (uint8_t)p;
        resp.data[1] = (uint8_t)b;
        resp.data[2] = (uint8_t)(v & 0xFF);
        resp.data[3] = (uint8_t)((v >> 8) & 0xFF);
        resp.data[4] = (uint8_t)((v >> 16) & 0xFF);
        resp.data[5] = (uint8_t)((v >> 24) & 0xFF);
        resp.len = 6;
        /* FIX 2026-10-31: report the CC1101's own RSSI register (the radio is
         * still in RX after the capture window) instead of leaving it 0. The
         * SmartRC driver exposes getRssi(); the hub's summary prints it. */
        resp.rssi = (int8_t)ELECHOUSE_cc1101.getRssi();
      } else {
        resp.status = FH_OD_NO_RESULT;           /* RF seen, no decode */
      }
    }
  }

  if (req.action == FH_OD_CC1101_ACT_RAW_OOK && resp.status == FH_OD_OK) {
    /* RAW_OOK capture-to-dump: serialize the edge buffer (g_dur/g_lvl from the
     * carried core), then stream FH_OD_DUMP_IND chunks BEFORE the summary RESP.
     * The RESP keeps the decode triple + RSSI, so a subsequent REPLAY works
     * from a raw capture too. service_stream() takes over from here. */
    int n = g_edgeCount;
    if (n > MAX_EDGES) n = MAX_EDGES;
    s_dump_len = 0;
    for (int i = 0; i < n && s_dump_len + 5 <= sizeof(s_dump_buf); ++i) {
      uint32_t d = g_dur[i];
      s_dump_buf[s_dump_len++] = (uint8_t)(d & 0xFF);
      s_dump_buf[s_dump_len++] = (uint8_t)((d >> 8) & 0xFF);
      s_dump_buf[s_dump_len++] = (uint8_t)((d >> 16) & 0xFF);
      s_dump_buf[s_dump_len++] = (uint8_t)((d >> 24) & 0xFF);
      s_dump_buf[s_dump_len++] = g_lvl[i];
    }
    s_dump_resp = resp;
    s_dump_chunk = 0;
    s_streaming = true;
    return;                                      /* service_stream() paces it out */
  }

  hub_stage((fh_type_t)FH_OD_CC1101_RESP, (uint8_t*)&resp, sizeof(resp));
  s_req_pending = false;                         /* op complete — accept the next */
}

/* Stream one capture-to-dump chunk per poll transaction (loop ctx). Advances
 * ONLY when hub_on_sent (IRQ) set s_tx_done — proof the previously staged
 * frame left on the wire — so a chunk is never overwritten before it left.
 * The summary RESP is staged LAST (the hub's flight completes on it). */
static void service_stream(void) {
  if (!s_streaming) { return; }
  if (!s_tx_done) { return; }                    /* wait for the poll that clocked it */
  s_tx_done = false;
  uint16_t off = (uint16_t)s_dump_chunk * (uint16_t)DUMP_CHUNK_DATA_MAX;
  if (off >= s_dump_len) {                       /* all chunks clocked: RESP last */
    hub_stage((fh_type_t)FH_OD_CC1101_RESP, (uint8_t*)&s_dump_resp, sizeof(s_dump_resp));
    s_streaming = false;
    s_dump_len = 0;
    s_dump_chunk = 0;
    s_req_pending = false;                       /* op complete — accept the next */
    return;
  }
  fh_od_dump_ind_t ind; memset(&ind, 0, sizeof(ind));
  ind.seq   = s_dump_resp.seq;
  ind.radio = FH_DUMP_RADIO_CC1101;
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
  s_rx_ready = false;                            /* release the SPSC slot */
  fh_decoder_push(&s_in_dec, buf, n, on_hub_frame, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  LOCAL DEBUG CONSOLE (USB Serial) — human-readable, NOT the hub link.
 *  Kept so the bench debuggability the gen1 authors valued survives. Optional.
 * ═══════════════════════════════════════════════════════════════════════════ */
static const char* BLADE_ID = "remix-fh-pico-cc1101 v1 (SPI)";

/* ═══════════════════════════════════════════════════════════════════════════
 *  ARDUINO ENTRY POINTS
 * ═══════════════════════════════════════════════════════════════════════════ */
void setup() {
  Serial.begin(115200);                  /* USB debug console only */
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 1500)) { /* brief wait for USB host */ }

  fh_decoder_init(&s_in_dec);

  radioInit();                           /* carried radio core (SPI0 master) */
  if (g_radioOk) radioEnterRx();

  /* Hub link: SPI1 slave on separate pins. */
  pinMode(HUBLINK_IRQ, OUTPUT);
  digitalWrite(HUBLINK_IRQ, LOW);
  SPISlave1.setSCK(HUBLINK_SCK);
  SPISlave1.setTX(HUBLINK_TX);           /* MISO (blade -> hub) */
  SPISlave1.setRX(HUBLINK_RX);           /* MOSI (hub -> blade) */
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

  Serial.print(BLADE_ID);
  Serial.print(F(" boot  radio="));
  Serial.println(g_radioOk ? F("ok") : F("ABSENT (check 3.3V + SPI0 wiring)"));
}

void loop() {
  service_rx();                          /* decode any inbound frame (IRQ copied raw) */
  service_pending();                     /* run any fired on-demand op */
  service_stream();                      /* pace one dump chunk per poll */

  /* Optional local debug: type 'r' over USB for a RAWTEST, 's' for status. */
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 's') {
      Serial.print(F("status radio=")); Serial.print(g_radioOk ? F("ok") : F("absent"));
      Serial.print(F(" freq=")); Serial.print(g_freqMHz, 3);
      Serial.print(F(" lastproto=")); Serial.print(g_lastProto);
      Serial.print(F(" lastbits=")); Serial.print(g_lastBits);
      Serial.print(F(" lastvalue=")); Serial.println(g_lastValue);
    }
  }
  delay(2);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  KNOWN RISKS (UNVERIFIED — first build + bench)
 *   [X1] arduino-pico SPISlave1 API (second SPI block as slave): verify
 *        SPISlave1 exists and setSCK/setTX/setRX/setCS/onData/onDataSent/setData
 *        names on YOUR core version. If only one SPISlave instance exists, the
 *        hub link must move to the block the radio does NOT use — the radio is
 *        SPI0 master (GP18/19/16), so the hub slave must be SPI1. #1 compile-fix.
 *   [X2] TWO SPI BLOCKS AT ONCE: radio = SPI0 master (to the CC1101 chip),
 *        hub = SPI1 slave. Confirm the pin groups don't overlap and both blocks
 *        init. HUBLINK pins (GP10/11/12/13 + IRQ GP8) are a DRAFT — verify a
 *        valid spi1 group on the Pico and that none clash with the radio pins
 *        (GP16/17/18/19/20/21).
 *   [X3] LONG CAPTURE vs SPI: a multi-second captureFrame() runs in loop()
 *        (service_pending), NOT in the SPI callback, so it never blocks the bus.
 *        The hub fires the REQ, then polls for the RESP; the blade answers BUSY
 *        to a second REQ while one is running (frozen one-at-a-time contract).
 *   [X4] RADIO CORE CARRIED VERBATIM from the proven gen1 blade (timing table,
 *        gdo0ISR capture, decodeSignal, transmitCode, radioEnterRx/Tx). The risk
 *        surface is the NEW hub-link layer, not the radio logic.
 *   [X5] REPLAY/SEND via fh_frame: WIRED (2026-10-31) — req.action == 3
 *        (FH_OD_CC1101_ACT_REPLAY) in service_pending() replays the LAST
 *        captured signal via transmitCode(). transmitCode is the proven gen1
 *        core. Bench-verify the first real REPLAY end-to-end (capture, then
 *        replay, then capture again to confirm RX re-entry).
 *   [X7] RAW OOK capture-to-dump (Tier 2, 2026-10-31): action 2 (RAW_OOK)
 *        serializes the carried core's edge buffer (g_dur/g_lvl, 5 B/edge,
 *        MAX_EDGES 512) and streams it as FH_OD_DUMP_IND (0x2F) chunks, one
 *        per poll transaction, paced by s_tx_done from hub_on_sent(); the
 *        summary RESP is staged LAST (the hub's flight completes on it).
 *        The dump store lives on Medusa (chunk reassembly by seq). The RESP
 *        still carries the decode triple, so REPLAY-after-RAW works.
 *   [X8] USB Serial is a DEBUG console only, not the hub link — binary frames
 *        never go over USB, so there is no LF-translation / byte-transparency
 *        concern on the hub link (that lives on SPI1, raw).
 *   [X9] RSSI (FIX 2026-10-31): the RESP now carries getRssi() from the
 *        SmartRC driver. VERIFY the method name on YOUR driver version — if it
 *        is ReadingRssi()/getRssiVal() there, rename; worst case delete the
 *        line (the field reverts to 0, which is honest-no-RSSI).
 * ═══════════════════════════════════════════════════════════════════════════ */
