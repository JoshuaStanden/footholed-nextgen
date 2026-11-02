/*
 * fh_ondemand.h — FHAdvanced on-demand radio command set (hub <-> Pico)
 *
 * Extends the shared UART wire protocol (fh_frame.h) with the TYPE codes and
 * payloads the HUB uses to PROMPT the Raspberry Pi Pico (RP2040) on-demand
 * radio node, and that the Pico uses to RETURN results. The Pico hosts four
 * radios, driven ONE AT A TIME by the contract below (the RP2040 is dual-core,
 * so this is a chosen model for simplicity, not a hardware limit):
 *     CC1101 (sub-GHz)  nRF24L01+ (2.4 GHz)  PN532 (NFC)  SX1262 (LoRa)
 *
 * These TYPE codes occupy the 0x20..0x3F band, which is UNUSED by fh_frame.h
 * (that file defines 0x01-0x03, 0x10-0x11, 0x7C-0x7F). No collision.
 *
 * Plain C, framework-agnostic, little-endian, packed — same rules as fh_frame.h
 * so the hub (ESP-IDF) and the Pico (arduino-pico) share ONE definition.
 *
 * FLOW (all carried in fh_frame.h frames; this only defines TYPE + PAYLOAD):
 *   HUB --REQ--> Pico          (a *_REQ TYPE with a request payload + seq)
 *   Pico --RESP--> HUB         (the matching *_RESP TYPE, same seq, + status)
 * Every request carries a `seq`; the response echoes it so the hub can match
 * reply to request and time out a radio that never answers.
 */

#pragma once

#include <stdint.h>
#include "fh_frame.h"   /* for packing conventions + fh_type_t band note */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------- on-demand TYPE codes
 * 0x20..0x3F band. _REQ = hub->Pico, _RESP = Pico->hub. */
typedef enum {
    /* generic */
    FH_OD_PING_REQ      = 0x20,  /* "are you alive / ready?"                   */
    FH_OD_PING_RESP     = 0x21,  /* readiness + which radios initialised OK    */

    /* CC1101 sub-GHz */
    FH_OD_CC1101_REQ    = 0x22,  /* do a sub-GHz action (listen/burst)         */
    FH_OD_CC1101_RESP   = 0x23,  /* result (rssi, bytes captured, status)      */

    /* nRF24 2.4 GHz proprietary */
    FH_OD_NRF24_REQ     = 0x24,  /* scan/sniff a channel set                   */
    FH_OD_NRF24_RESP    = 0x25,  /* result                                     */

    /* PN532 NFC */
    FH_OD_NFC_REQ       = 0x26,  /* attempt an NFC read                        */
    FH_OD_NFC_RESP      = 0x27,  /* tag UID/type/payload or "no tag"           */

    /* SX1262 LoRa — sensor-stick relay (the promptable LoRa link) */
    FH_OD_LORA_TX_REQ   = 0x28,  /* transmit this payload to the sensor stick  */
    FH_OD_LORA_TX_RESP  = 0x29,  /* TX done / failed (+ time-on-air ms)        */
    FH_OD_LORA_RX_IND   = 0x2A,  /* UNSOLICITED: a LoRa frame arrived from the
                                  * sensor stick (itsy->hub, no matching req)  */
    FH_OD_LORA_LISTEN_REQ  = 0x2B, /* enter/leave LoRa receive window          */
    FH_OD_LORA_LISTEN_RESP = 0x2C, /* ack the listen-state change              */
    FH_OD_SYS_STATE_REQ    = 0x2D, /* hub answers ITSELF (not a blade): the    */
    FH_OD_SYS_STATE_RESP   = 0x2E, /*   host asks for SD/uptime/config/job     */
                                   /*   health; the hub bridge builds the RESP */
    FH_OD_DUMP_IND         = 0x2F, /* UNSOLICITED chunk stream: a blade doing  */
                                   /*   a capture-to-dump job stages chunks    */
                                   /*   FIRST, then the summary RESP last.     */
    /* ── EXTERN VERB BAND (0x30..0x3F) — Tier 3 ──────────────────────────────
     * Transmit-disruption primitives AUTHORED EXTERNALLY (no menu in Medusa;
     * command-line only). Contract:
     *   • RESP = REQ + 1, exactly like the frozen band. REQs are EVEN.
     *   • Routing by band: 0x30..0x37 = CC1101 blade, 0x38..0x3F = nRF24
     *     blade (fh_link_slot_for_radio). One generic REQ/RESP shape; the
     *     blade decodes its own verb_id.
     *   • A blade WITHOUT an authored implementation answers the generic
     *     RESP with status FH_OD_EXT_STUB — proof the plumbing works and a
     *     slot for the external author to replace.
     *   • p3 = duration_ms convention for timed ops (0 = instant); the hub
     *     uses it for the job deadline, clamped to 30 s.
     * Shipped verb ids (frozen so Medusa's CLI and any future author agree):
     *   0x30/0x31 CC1101 JAM · 0x32/0x33 CC1101 BRUTE (rolling-code)
     *   0x38/0x39 nRF24 JAM   · 0x3A/0x3B nRF24 PROTOKILL
     *   0x34/0x35 CC1101 TX (IMPLEMENTED — transmit author-specified OOK bits:
     *           p1=freq_khz, p2=bits 1..32, p3=0; tail[0]=proto, tail[1..4]=
     *           value u32 LE, tail[5]=repeats 0->3. RESP echoes the triple.)
     *   0x3C/0x3D nRF24 TX (IMPLEMENTED — transmit a payload to your own
     *           receiver: p1=channel 0..125, p2=repeats 0->1 (cap 16), p3=0;
     *           tail[0]=payload_len 1..32, tail[1..]=payload. RESP echoes it.)
     *   0x36 (CC side) / 0x3E (NRF side) reserved for future authors.        */
    FH_OD_EXT_CC_JAM_REQ   = 0x30,
    FH_OD_EXT_CC_JAM_RESP  = 0x31,
    FH_OD_EXT_CC_BRUTE_REQ = 0x32,
    FH_OD_EXT_CC_BRUTE_RESP= 0x33,
    FH_OD_EXT_CC_TX_REQ    = 0x34,
    FH_OD_EXT_CC_TX_RESP   = 0x35,   /* IMPLEMENTED: raw OOK TX (own signals) */
    FH_OD_EXT_NRF_JAM_REQ  = 0x38,
    FH_OD_EXT_NRF_JAM_RESP = 0x39,
    FH_OD_EXT_NRF_PROTOKILL_REQ  = 0x3A,
    FH_OD_EXT_NRF_PROTOKILL_RESP = 0x3B,
    FH_OD_EXT_NRF_TX_REQ   = 0x3C,
    FH_OD_EXT_NRF_TX_RESP  = 0x3D,   /* IMPLEMENTED: nRF24 payload TX (own)   */
} fh_od_type_t;

/* --------------------------------------------------------- status codes */
typedef enum {
    FH_OD_OK          = 0x00,
    FH_OD_BUSY        = 0x01,  /* another radio op in progress (one-at-a-time) */
    FH_OD_NO_RESULT   = 0x02,  /* ran OK but found nothing (no tag, no frame)  */
    FH_OD_RADIO_FAIL  = 0x03,  /* the radio module did not respond / init fail */
    FH_OD_BAD_REQ     = 0x04,  /* malformed request payload                    */
    FH_OD_TIMEOUT     = 0x05,  /* op exceeded its deadline                     */
    FH_OD_EXT_STUB    = 0x06,  /* extern-band REQ hit a blade with NO authored */
                               /*   implementation — the plumbing answered the */
                               /*   generic RESP so the bench can prove the    */
                               /*   route; the external author replaces this.  */
} fh_od_status_t;

/* radio id (for PING_RESP readiness bitmap + logging) */
#define FH_OD_RADIO_CC1101  (1u << 0)
#define FH_OD_RADIO_NRF24   (1u << 1)
#define FH_OD_RADIO_PN532   (1u << 2)
#define FH_OD_RADIO_SX1262  (1u << 3)

/* ============================================================ payloads
 * Every REQ starts with a seq byte; every RESP echoes seq + carries status.
 * ==================================================================== */

/* PING_REQ (0x20) — 1 B */
typedef struct __attribute__((packed)) {
    uint8_t seq;
} fh_od_ping_req_t;

/* PING_RESP (0x21) — 3 B */
typedef struct __attribute__((packed)) {
    uint8_t seq;
    uint8_t status;        /* fh_od_status_t */
    uint8_t radios_ready;  /* bitmap of FH_OD_RADIO_* that initialised OK */
} fh_od_ping_resp_t;

/* CC1101_REQ (0x22) — pick a sub-GHz action */
typedef struct __attribute__((packed)) {
    uint8_t  seq;
    uint8_t  action;       /* FH_OD_CC1101_ACT_* — which sub-GHz verb to run   */
    uint32_t freq_khz;     /* frequency in kHz (e.g. 433920 = 433.92 MHz)      */
    uint16_t dwell_ms;     /* how long to listen before giving up              */
} fh_od_cc1101_req_t;

/* CC1101 actions (the `action` field above). LISTEN/RX_ONE/RAW_OOK are capture
 * verbs; REPLAY (3) is BUILT in the blade — it re-transmits the blade's LAST
 * captured decode triple (its own g_lastValue/g_lastBits/g_lastProto). To
 * replay a SPECIFIC older capture, Medusa sends the implemented ext-band
 * CC1101 TX verb (0x34) with the stored triple instead. Adding a verb =
 * adding a value here (additive; does not change the struct size). */
#define FH_OD_CC1101_ACT_LISTEN   0u  /* open a listen window (cap)            */
#define FH_OD_CC1101_ACT_RX_ONE   1u  /* capture one packet then stop (cap)    */
#define FH_OD_CC1101_ACT_RAW_OOK  2u  /* raw OOK capture (cap raw)             */
#define FH_OD_CC1101_ACT_REPLAY   3u  /* RESERVED: TX a saved file (replay)    */

/* CC1101_RESP (0x23) — up to a bounded capture snippet */
typedef struct __attribute__((packed)) {
    uint8_t  seq;
    uint8_t  status;       /* fh_od_status_t */
    int8_t   rssi;         /* dBm of what was heard (if any)                   */
    uint8_t  len;          /* bytes in `data`                                  */
    uint8_t  data[32];     /* bounded raw payload snippet (LEN covers real len)*/
} fh_od_cc1101_resp_t;

/* NRF24_REQ (0x24) — pick a scan or a sniff (capture-to-dump) */
typedef struct __attribute__((packed)) {
    uint8_t  seq;
    uint8_t  mode;         /* FH_OD_NRF24_MODE_SCAN / _SNIFF                    */
    uint8_t  chan_lo;      /* scan: first channel; sniff: THE channel           */
    uint8_t  chan_hi;      /* scan: last channel (ignored in sniff)             */
    uint16_t dwell_ms;     /* scan: per-channel dwell; sniff: capture window    */
} fh_od_nrf24_req_t;       /* was 5 B, now 6 B (mode added after seq)           */

/* nRF24 request modes */
#define FH_OD_NRF24_MODE_SCAN   0u  /* sweep chan_lo..chan_hi for a carrier      */
#define FH_OD_NRF24_MODE_SNIFF  1u  /* dwell on chan_lo, capture-to-dump         */

/* NRF24_RESP (0x25) */
typedef struct __attribute__((packed)) {
    uint8_t  seq;
    uint8_t  status;
    uint8_t  active_chan;  /* channel where something was seen (0xFF = none)   */
    int8_t   rssi;
} fh_od_nrf24_resp_t;

/* NFC_REQ (0x26) */
typedef struct __attribute__((packed)) {
    uint8_t  seq;
    uint16_t timeout_ms;   /* how long to wait for a tag in the field          */
} fh_od_nfc_req_t;

/* NFC_RESP (0x27) */
typedef struct __attribute__((packed)) {
    uint8_t  seq;
    uint8_t  status;       /* NO_RESULT if no tag presented                    */
    uint8_t  uid_len;      /* 4, 7, or 10 for typical tags                     */
    uint8_t  uid[10];
    uint8_t  tag_type;     /* implementation enum (Mifare/NTAG/etc.)           */
} fh_od_nfc_resp_t;

/* LORA_TX_REQ (0x28) — relay a payload to the sensor stick */
typedef struct __attribute__((packed)) {
    uint8_t  seq;
    uint8_t  flags;        /* priority / ack-request bits                      */
    uint8_t  len;          /* bytes in `data`                                  */
    uint8_t  data[200];    /* opaque sensor-stick payload (LEN covers real len)*/
} fh_od_lora_tx_req_t;

/* LORA_TX_RESP (0x29) */
typedef struct __attribute__((packed)) {
    uint8_t  seq;
    uint8_t  status;
    uint16_t toa_ms;       /* time-on-air of the transmit, ms                  */
} fh_od_lora_tx_resp_t;

/* LORA_RX_IND (0x2A) — UNSOLICITED inbound LoRa from the sensor stick.
 * seq here is the Pico's own running counter (hub just logs/acks). */
typedef struct __attribute__((packed)) {
    uint8_t  seq;
    int8_t   rssi;
    int8_t   snr;
    uint8_t  len;
    uint8_t  data[200];
} fh_od_lora_rx_ind_t;

/* LORA_LISTEN_REQ (0x2B) / LORA_LISTEN_RESP (0x2C) */
typedef struct __attribute__((packed)) {
    uint8_t  seq;
    uint8_t  enable;       /* 1 = enter RX window, 0 = leave                   */
    uint16_t window_ms;    /* 0 = continuous until told to stop                */
} fh_od_lora_listen_req_t;

typedef struct __attribute__((packed)) {
    uint8_t  seq;
    uint8_t  status;         /* fh_od_status_t                                  */
    uint32_t uptime_s;       /* seconds since hub boot (esp_timer)              */
    uint8_t  interactive;    /* 1 = bridge in INTERACTIVE mode (host attached)  */
    uint8_t  sd_mounted;     /* 1 = SD card up (fh_board_sd_mounted)            */
    uint32_t sd_free_mb;     /* free space in MiB (0xFFFFFFFF = unknown)        */
    uint32_t sd_used_mb;     /* used space in MiB (0xFFFFFFFF = unknown)        */
    uint32_t rows_staged;    /* logger: rows accepted, not yet proven on SD     */
    uint32_t rows_committed; /* logger: bytes actually fwritten+flushed to SD   */
    uint32_t files_rolled;   /* logger: 128 MB rotations                        */
    uint32_t dropped_queue;  /* logger: rows dropped (queue full)               */
    uint32_t dropped_sd_down;/* logger: rows lost while SD down                 */
    uint32_t res_written;    /* on-demand result files/rows written             */
    uint32_t jobs_done;      /* requester: jobs completed                       */
    uint32_t job_timeouts;   /* requester: deadlines expired                    */
    uint32_t job_busy;       /* requester: BUSY RESPs seen from blades          */
    uint8_t  gps_rate;       /* cfg #1: 0=1Hz 1=5Hz (fh_gps_rate_t)             */
    uint8_t  brightness_pct; /* cfg #2: UI-scale 0..100                         */
    uint8_t  ble_scan_mode;  /* cfg #3: 0=passive 1=active                      */
    uint8_t  relog_mode;     /* cfg #4: 0=timed 1=once-per-session              */
    uint16_t relog_window_s; /* cfg #4b: window seconds                         */
    uint8_t  band_mask;      /* cfg #5: bit0=2.4 bit1=5 bit2=BLE                */
    uint16_t dwell_ms;       /* cfg #6: Wi-Fi per-channel dwell                 */
    uint8_t  cfg_modified;   /* cfg status: 0=STOCK 1=MODIFIED(N)               */
    uint16_t override_count; /* cfg status: settings differing from baseline    */
    uint8_t  fix_valid;      /* last GPS fix valid (fh_time_last_fix)           */
    int32_t  lat_e7;         /* degrees x 1e7 (0 if never)                      */
    int32_t  lon_e7;         /* degrees x 1e7 (0 if never)                      */
    uint8_t  sats;           /* satellites in the last fix                      */
} fh_od_sys_state_resp_t;    /* 74 B                                            */

/* SYS_STATE_REQ (0x2D) — 1 B, seq only. Answered by the HUB ITSELF (the bridge
 * builds the RESP; it is never routed to a blade). */
typedef struct __attribute__((packed)) {
    uint8_t seq;
} fh_od_sys_state_req_t;

/* DUMP_IND (0x2F) — UNSOLICITED capture-to-dump chunk stream from a blade.
 * A blade doing a capture-to-dump job stages one chunk per SPI transaction,
 * chunks FIRST (more=1), the summary RESP LAST (the hub requester's flight
 * completes on the RESP, so every chunk is already mirrored to Medusa).
 * The `radio` tag says WHICH blade produced the stream and thus the record
 * layout inside data[]:
 *   radio = FH_DUMP_RADIO_CC1101: 5-byte records — u32 dur_us LE, u8 level
 *           (the raw OOK edge capture: each entry is one half-pulse).
 *   radio = FH_DUMP_RADIO_NRF24:  40-byte records — u32 t_ms LE (since sniff
 *           start), u8 len, payload[32], 3 bytes reserved (len covers the
 *           real payload; nRF24L01+ payloads are 1..32 B).
 */
#define FH_DUMP_RADIO_CC1101 1u
#define FH_DUMP_RADIO_NRF24  2u
typedef struct __attribute__((packed)) {
    uint8_t  seq;          /* echoes the job's REQ seq                          */
    uint8_t  radio;        /* FH_DUMP_RADIO_* — record layout inside data[]     */
    uint8_t  chunk;        /* 0-based chunk index within this stream            */
    uint8_t  more;         /* 1 = more chunks follow, 0 = this is the last      */
    uint8_t  len;          /* valid bytes in data[] (<= 250)                    */
    uint8_t  data[250];
} fh_od_dump_ind_t;        /* 255 B = the frame-payload maximum                 */

/* ---- EXTERN VERB BAND (0x30..0x3F): one generic REQ/RESP shape ------------
 * The blade decodes `verb_id` (author-defined within its band) and the param
 * words. `tail` carries any author-defined extra bytes — the frame LEN covers
 * only the bytes actually sent, so a tail-less REQ is just 15 B on the wire.
 * p3 = duration_ms convention for timed ops (0 = instant; hub deadline floor).
 * The generic RESP echoes seq + status; data[] is author-defined. */
typedef struct __attribute__((packed)) {
    uint8_t  seq;
    uint8_t  verb_id;      /* author-defined (frozen table above)               */
    uint32_t p1;           /* author-defined param 1 (freq, channel, ...)       */
    uint32_t p2;           /* author-defined param 2 (count, dwell, ...)        */
    uint32_t p3;           /* duration_ms for timed ops (0 = instant)           */
    uint8_t  tail[232];    /* author-defined extras (LEN covers the real len)   */
} fh_od_ext_req_t;         /* 246 B max                                         */

typedef struct __attribute__((packed)) {
    uint8_t  seq;
    uint8_t  status;       /* fh_od_status_t — FH_OD_EXT_STUB until authored    */
    uint8_t  len;          /* valid bytes in data[] (0 = no author data)        */
    uint8_t  data[250];
} fh_od_ext_resp_t;        /* 253 B                                             */

typedef struct __attribute__((packed)) {
    uint8_t  seq;
    uint8_t  status;       /* fh_od_status_t                                   */
    uint8_t  listening;    /* current state after the change                   */
} fh_od_lora_listen_resp_t;

/* Compile-time guards: if any on-demand struct drifts from its specced size,
 * the build FAILS here rather than silently shipping a wrong wire layout.
 * (Same discipline as fh_frame.h's _Static_asserts.) Every payload must also
 * fit in a single frame: LEN is one byte, so each struct must be <= 255 B. */
_Static_assert(sizeof(fh_od_ping_req_t)        == 1,  "fh_od_ping_req_t must be 1 byte");
_Static_assert(sizeof(fh_od_ping_resp_t)       == 3,  "fh_od_ping_resp_t must be 3 bytes");
_Static_assert(sizeof(fh_od_cc1101_req_t)      == 8,  "fh_od_cc1101_req_t must be 8 bytes");
_Static_assert(sizeof(fh_od_cc1101_resp_t)     == 36, "fh_od_cc1101_resp_t must be 36 bytes");
_Static_assert(sizeof(fh_od_nrf24_req_t)       == 6,  "fh_od_nrf24_req_t must be 6 bytes (mode added)");
_Static_assert(sizeof(fh_od_nrf24_resp_t)      == 4,  "fh_od_nrf24_resp_t must be 4 bytes");
_Static_assert(sizeof(fh_od_nfc_req_t)         == 3,  "fh_od_nfc_req_t must be 3 bytes");
_Static_assert(sizeof(fh_od_nfc_resp_t)        == 14, "fh_od_nfc_resp_t must be 14 bytes");
_Static_assert(sizeof(fh_od_lora_tx_req_t)     == 203, "fh_od_lora_tx_req_t must be 203 bytes");
_Static_assert(sizeof(fh_od_lora_tx_resp_t)    == 4,  "fh_od_lora_tx_resp_t must be 4 bytes");
_Static_assert(sizeof(fh_od_lora_rx_ind_t)     == 204, "fh_od_lora_rx_ind_t must be 204 bytes");
_Static_assert(sizeof(fh_od_lora_listen_req_t) == 4,  "fh_od_lora_listen_req_t must be 4 bytes");
_Static_assert(sizeof(fh_od_lora_listen_resp_t)== 3,  "fh_od_lora_listen_resp_t must be 3 bytes");
_Static_assert(sizeof(fh_od_sys_state_resp_t) == 74,  "fh_od_sys_state_resp_t must be 74 bytes");
_Static_assert(sizeof(fh_od_sys_state_req_t)  == 1,   "fh_od_sys_state_req_t must be 1 byte");
_Static_assert(sizeof(fh_od_dump_ind_t)       == 255, "fh_od_dump_ind_t must be 255 bytes");
_Static_assert(sizeof(fh_od_ext_req_t)        == 246, "fh_od_ext_req_t must be 246 bytes");
_Static_assert(sizeof(fh_od_ext_resp_t)       == 253, "fh_od_ext_resp_t must be 253 bytes");
/* All on-demand payloads fit in one frame (LEN is a single byte, max 255). */
_Static_assert(sizeof(fh_od_lora_rx_ind_t) <= FH_MAX_PAYLOAD,
               "largest on-demand payload must fit FH_MAX_PAYLOAD");

/* ---- one-at-a-time contract (documented so both ends agree) ----------------
 * The Pico services ONE radio action at a time. If a REQ arrives while another
 * op is running, it answers FH_OD_BUSY rather than queueing deeply. The hub
 * should serialize its prompts and wait for the RESP (or a timeout) before
 * issuing the next. A long LoRa TX (high SF) can hold the node busy for up to
 * ~1 s; the hub must tolerate that latency on the other on-demand radios. None
 * of these radios is a CONTINUOUS capture stream (those live on the C5/S3/hub),
 * so serialization is acceptable by design. (The RP2040 is dual-core, so a
 * future revision COULD service a long LoRa TX on core1 while core0 handles
 * other radios — the one-at-a-time contract here is a simplicity choice, not a
 * hardware limit, and both ends may keep assuming it.) */

#ifdef __cplusplus
}
#endif
