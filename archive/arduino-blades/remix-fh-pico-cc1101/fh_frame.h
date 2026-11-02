/*
 * fh_frame.h — FHAdvanced shared UART wire protocol (fh_protocol component)
 *
 * SINGLE SOURCE OF TRUTH for the inter-board UART frame format. Included by all
 * three firmwares (hub / XIAO-C5 / XIAO-S3) so the frame definitions cannot
 * drift. Transcribed verbatim from docs/ARCHITECTURE.md "UART frame format".
 *
 * Frame layout (self-synchronizing, length-driven, CRC-checked):
 *   off size field     notes
 *   0   2    SYNC      0xA5 0x5A  frame start marker (resync anchor)
 *   2   1    TYPE      message type (fh_type_t)
 *   3   1    LEN       payload length in bytes (0..255)
 *   4   LEN  PAYLOAD   type-specific body
 *  4+LEN 2   CRC16     CRC-16/CCITT over TYPE..end-of-PAYLOAD (NOT the SYNC)
 *
 * Max frame = 2 + 1 + 1 + 255 + 2 = 261 bytes.
 * Endianness: LITTLE-ENDIAN for all multi-byte fields (native to every ESP32
 * chip here — no byte-swapping). All payload structs are __attribute__((packed))
 * so compiler padding cannot misalign the specced byte layout.
 *
 * This header is plain C and framework-agnostic (no ESP-IDF or Arduino deps),
 * so it compiles identically in every project.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ framing */

#define FH_SYNC0            0xA5u   /* first  SYNC byte */
#define FH_SYNC1            0x5Au   /* second SYNC byte */

#define FH_HDR_LEN          4u      /* SYNC(2) + TYPE(1) + LEN(1)             */
#define FH_CRC_LEN          2u      /* CRC16 trailer                          */
#define FH_MAX_PAYLOAD      255u    /* LEN is a single byte                   */
#define FH_MAX_FRAME        (FH_HDR_LEN + FH_MAX_PAYLOAD + FH_CRC_LEN) /* 261 */

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout. */
#define FH_CRC16_POLY       0x1021u
#define FH_CRC16_INIT       0xFFFFu

/* -------------------------------------------------------------- TYPE codes */
/* Values are fixed by the wire protocol — never renumber. */
typedef enum {
    FH_TYPE_CAPTURE       = 0x01, /* daughterboard -> hub : capture record    */
    FH_TYPE_GPS_FIX       = 0x02, /* Pico -> hub : GPS position fix (fh_gps_fix_t,
                                   * 16 B). LIVE as of 2026-10-22: GPS moved from
                                   * the hub onto the Pico (it parses the NEO-6M
                                   * and sends this frame). Sniffer nodes never
                                   * send it. Value + struct are UNCHANGED.     */
    FH_TYPE_SUMMARY       = 0x03, /* daughterboard -> hub : per-device count   */
    FH_TYPE_SSID          = 0x04, /* daughterboard -> hub : BSSID -> full SSID
                                   * mapping (fh_ssid_t, 39 B). Sent occasionally
                                   * per AP (the mapping is stable), so CAPTURE
                                   * stays lean; the host JOINS by MAC. Enables
                                   * exact duplicate-SSID / rogue-AP detection.  */
    FH_TYPE_LORA_RELAY_TX = 0x10, /* hub -> Pico : LoRa relay payload to send
                                   * to the sensor stick. LIVE (LoRa now lives
                                   * on the Pico, not the XIAO-S3). The on-demand
                                   * CONTROL codes around this are 0x28-0x2C in
                                   * fh_ondemand.h.                            */
    FH_TYPE_LORA_RELAY_RX = 0x11, /* Pico -> hub : LoRa relay payload received
                                   * from the sensor stick. LIVE. See also
                                   * fh_ondemand.h FH_OD_LORA_RX_IND (0x2A).   */
    FH_TYPE_HEARTBEAT     = 0x7E, /* daughterboard -> hub : liveness counters  */
    FH_TYPE_PING          = 0x7D, /* hub -> daughterboard : health check       */
    FH_TYPE_PONG          = 0x7C, /* daughterboard -> hub : reply to PING      */
    FH_TYPE_ACK           = 0x7F, /* both ways            : ack a LORA_RELAY   */
} fh_type_t;

/* ------------------------------------------------------- src_id enumeration */
/* WIRE src_id — identifies the NODE a frame is from/about. Originally this was
 * "the CAPTURE sender", but it ALSO covers control-liveness nodes that never
 * send captures (HEARTBEAT / PING / PONG carry src_id too). These values are
 * FIXED by the wire protocol — never renumber. HEARTBEAT src_id matches:
 * 1=c5, 2=xs3, 3=pico, 4=head — one node-id scheme covers capture + liveness.
 *
 * NOTE: not every CSV `source` string has a wire src_id. The hub GENERATES its
 * own BLE rows, and it turns the Pico's on-demand radio RESULTS (cc1101/nrf24/
 * nfc) into CSV rows itself — those never arrive as CAPTURE frames, so they
 * carry NO wire src_id. The full CSV `source` vocabulary is the string set
 * below; the wire enum is the subset that identifies a frame sender OR a
 * liveness-tracked node. */
typedef enum {
    FH_SRC_C5_WIFI5   = 1,  /* XIAO-C5  : 5 GHz Wi-Fi sniffer   (CAPTURE sender) */
    FH_SRC_XS3_WIFI24 = 2,  /* XIAO-S3  : 2.4 GHz Wi-Fi sniffer (CAPTURE sender) */
    FH_SRC_PICO       = 3,  /* Pico     : on-demand radio node  (HEARTBEAT only;
                             * its per-radio results become hub-formatted CSV
                             * rows, not CAPTURE frames — see the strings below)*/
    FH_SRC_HEAD       = 4,  /* Waveshare-S3 display HEAD : NOT a capture sender —
                             * a Bus-A peer tracked for LIVENESS only. Answers the
                             * hub PING with a PONG (and/or sends a HEARTBEAT) so
                             * the hub confirms the head link bidirectionally. */
    FH_SRC_WS_BLE     = 5,  /* P4 hub : on-board BLE sniffer. A CAPTURE source
                             * like the Wi-Fi sniffers, just HUB-LOCAL — its
                             * captures carry this wire src_id (source string
                             * "ws-ble"). Streams/logs as an ordinary source. */
} fh_src_id_t;

/* CSV `source` column vocabulary (SD schema in ARCHITECTURE.md). ONE spelling,
 * shared so the hub writer and any reader cannot disagree. The first two map
 * 1:1 from the wire src_id above; the rest are hub-assigned (BLE is hub-local;
 * the Pico-radio sources are written by the hub from on-demand results).
 * FROZEN 2026-10-20 — adding a source means adding a string here.
 *
 * THE PICO HOSTS FOUR RADIOS, but they split into two kinds:
 *   CAPTURE radios (3) — CC1101 / nRF24 / PN532 — each SENSES something (a
 *     sub-GHz burst, a 2.4 GHz scan, an NFC tag) → a logged recon row. Three
 *     capture `source` strings: pico-cc1101 / pico-nrf24 / pico-nfc.
 *   COMMS radio (1) — SX1262 LoRa — is the sensor-stick LINK, not a sensor.
 *     It CARRIES data to/from the stick; it does not CAPTURE the environment,
 *     so by design it produces NO capture row (ARCHITECTURE.md: "LoRa relay
 *     traffic is comms, not capture"). That is why LoRa has no capture source
 *     string — a deliberate decision, NOT an oversight of the 4th radio.
 *
 * OPTIONAL comms-event logging: if you WANT a log trail of sensor-stick
 * activity (e.g. "relayed to stick", "stick ACK/RX"), use FH_SRCSTR_PICO_LORA
 * below. It is a COMMS-EVENT source, not a capture source — rows under it
 * record link events, not sensed devices. Logging it is OPT-IN (the hub may
 * choose to emit these); leaving it unused is valid and changes nothing. */
#define FH_SRCSTR_C5_WIFI5   "c5-wifi5"     /* FH_SRC_C5_WIFI5  (capture)       */
#define FH_SRCSTR_XS3_WIFI24 "xs3-wifi24"   /* FH_SRC_XS3_WIFI24 (capture)      */
#define FH_SRCSTR_WS_BLE     "ws-ble"       /* hub internal BLE radio (capture) */
#define FH_SRCSTR_PICO_CC1101 "pico-cc1101" /* Pico sub-GHz  (capture, from RESP)*/
#define FH_SRCSTR_PICO_NRF24  "pico-nrf24"  /* Pico 2.4 GHz  (capture, from RESP)*/
#define FH_SRCSTR_PICO_NFC    "pico-nfc"    /* Pico PN532    (capture, from RESP)*/
#define FH_SRCSTR_PICO_LORA   "pico-lora-event" /* Pico SX1262 LoRa: COMMS event,
                                             * NOT capture. Sensor-stick link
                                             * activity. OPT-IN logging.       */

/* ============================================================ payload structs
 * All little-endian, all packed. Sizes below match ARCHITECTURE.md exactly.
 * ========================================================================== */

/* CAPTURE (TYPE 0x01) — ~34 B. `extra` is bounded; the frame LEN carries the
 * real used length, so a sender MAY emit fewer than sizeof bytes of extra.
 * Receivers must use LEN, not sizeof, to know how many extra bytes are valid.
 * SENDER INVARIANT (enforce when the sniffer firmware is written): a sender
 * MUST set LEN = FH_CAPTURE_FIXED + E where E is the number of extra bytes it
 * actually filled (E in 0..FH_CAPTURE_EXTRA_MAX). Emitting LEN = sizeof(..)
 * while leaving extra[] partly uninitialized ships stack garbage on the wire. */
typedef struct __attribute__((packed)) {
    uint8_t  src_id;      /* fh_src_id_t                                      */
    uint8_t  band_chan;   /* high nibble = band (2.4/5), low = channel index  */
    uint8_t  frame_type;  /* mgmt/data/ctrl + subtype (probe-req, beacon, …)  */
    int8_t   rssi;        /* dBm                                              */
    uint8_t  mac[6];      /* 6-byte device MAC                                */
    uint32_t gps_time;    /* epoch s; C5 fills real GPS time, else 0          */
    int32_t  lat;         /* degrees x 1e7 (0 if no fix)                      */
    int32_t  lon;         /* degrees x 1e7 (0 if no fix)                      */
    uint16_t count;       /* occurrences since last summary (dedup count)     */
    uint8_t  extra[10];   /* type-specific; bounded, real length from LEN     */
} fh_capture_t;           /* 24 fixed + 10 extra = 34 B                        */

/* Fixed (non-extra) portion of a CAPTURE payload. A sender that emits E bytes
 * of extra sets LEN = FH_CAPTURE_FIXED + E (E in 0..10). */
#define FH_CAPTURE_FIXED   (offsetof(fh_capture_t, extra))   /* 24 */
#define FH_CAPTURE_EXTRA_MAX  10u

/* GPS_FIX (TYPE 0x02) — 16 B. Hub caches this as last-known position. */
typedef struct __attribute__((packed)) {
    uint32_t gps_time;    /* epoch seconds        */
    int32_t  lat;         /* deg x 1e7            */
    int32_t  lon;         /* deg x 1e7            */
    uint8_t  sats;        /* satellites in fix    */
    uint8_t  fix_qual;    /* fix quality / HDOP bucket */
    uint16_t reserved;    /* pad to 16            */
} fh_gps_fix_t;           /* 16 B */

/* SUMMARY (TYPE 0x03) — periodic per-device count. Same identity keying as a
 * CAPTURE row; carries the accumulated count since the last summary. */
typedef struct __attribute__((packed)) {
    uint8_t  src_id;
    uint8_t  band_chan;
    uint8_t  mac[6];
    uint16_t count;       /* occurrences accumulated in this summary window   */
    uint32_t last_seen_s; /* seconds-since-boot of last sighting (sender clk) */
} fh_summary_t;           /* 14 B */

/* SSID (TYPE 0x04) — 39 B. BSSID -> full SSID mapping, emitted occasionally per
 * AP (not per capture). The host joins it to CAPTURE rows by MAC for exact
 * duplicate-SSID / rogue-AP detection. ssid_len 0 = hidden SSID. */
typedef struct __attribute__((packed)) {
    uint8_t  mac[6];      /* BSSID this SSID belongs to                       */
    uint8_t  ssid_len;    /* valid bytes in ssid[] (0..32; 0 = hidden)        */
    uint8_t  ssid[32];    /* UTF-8 SSID; only ssid_len bytes are valid        */
} fh_ssid_t;              /* 39 B */

/* LORA_RELAY (TYPE 0x10 / 0x11). `data` is opaque; N = LEN - 2. */
typedef struct __attribute__((packed)) {
    uint8_t  seq;         /* sequence number (echoed in ACK 0x7F)             */
    uint8_t  flags;       /* transport/priority flags                         */
    uint8_t  data[FH_MAX_PAYLOAD - 2]; /* opaque; real length from LEN        */
} fh_lora_relay_t;
#define FH_LORA_RELAY_HDR  2u          /* seq + flags                          */
#define FH_LORA_DATA_MAX   (FH_MAX_PAYLOAD - FH_LORA_RELAY_HDR)

/* HEARTBEAT (TYPE 0x7E) — 14 B. Sent on a fixed cadence (1–2 s) whether or not
 * anything was captured. Carries proof-of-work, not just "alive". */
typedef struct __attribute__((packed)) {
    uint8_t  src_id;        /* which daughterboard                            */
    uint32_t uptime_s;      /* seconds since boot — reset to 0 = silent reboot*/
    uint32_t capture_count; /* records captured since boot — must keep rising */
    uint16_t dropped_keys;  /* seen-table evictions (saturation indicator)    */
    uint8_t  task_alive;    /* bitmask: capture / GPS / LoRa task liveness    */
    uint16_t free_heap_kb;  /* KB free — catch a slow leak before a crash     */
} fh_heartbeat_t;           /* 14 B */

/* task_alive bit assignments (daughterboard reports which tasks are ticking).
 * NOTE: in the current design GPS lives on the PICO (0x02 stream, not a task
 * report) and LoRa on the Pico too, so the GPS/LORA bits are legacy from the
 * 3-node split and go unused by the continuous sniffers (C5/S3). Kept for
 * wire-compat; do not renumber. */
#define FH_TASK_CAPTURE     (1u << 0)
#define FH_TASK_GPS         (1u << 1)   /* legacy (GPS is on the Pico, 0x02) */
#define FH_TASK_LORA        (1u << 2)   /* legacy (LoRa now on Pico) */

/* PING (0x7D) / PONG (0x7C) — 1 B. PONG echoes the PING's seq. */
typedef struct __attribute__((packed)) {
    uint8_t  seq;
} fh_ping_t;                /* also used for PONG */

/* ACK (0x7F) — 1 B. Echoes the seq of the LORA_RELAY being acked. */
typedef struct __attribute__((packed)) {
    uint8_t  seq;
} fh_ack_t;

/* Compile-time guards: if any struct drifts from the specced size, the build
 * FAILS here rather than silently shipping a wrong wire layout. */
_Static_assert(sizeof(fh_capture_t)   == 34, "fh_capture_t must be 34 bytes");
_Static_assert(sizeof(fh_gps_fix_t)   == 16, "fh_gps_fix_t must be 16 bytes");
_Static_assert(sizeof(fh_summary_t)   == 14, "fh_summary_t must be 14 bytes");
_Static_assert(sizeof(fh_ssid_t)      == 39, "fh_ssid_t must be 39 bytes");
_Static_assert(sizeof(fh_heartbeat_t) == 14, "fh_heartbeat_t must be 14 bytes");
_Static_assert(FH_CAPTURE_FIXED       == 24, "CAPTURE fixed part must be 24 bytes");

/* ===================================================================== CRC */

/* CRC-16/CCITT-FALSE over `len` bytes of `data`. Used over TYPE..PAYLOAD. */
uint16_t fh_crc16(const uint8_t *data, size_t len);

/* ============================================================ encode / decode
 * ========================================================================== */

/* Encode one frame into `out` (must hold >= FH_MAX_FRAME bytes).
 * `payload` may be NULL iff payload_len == 0.
 * Returns the total framed length (header+payload+crc), or 0 on error
 * (payload_len > FH_MAX_PAYLOAD, or out == NULL). */
size_t fh_frame_encode(fh_type_t type,
                       const uint8_t *payload, uint8_t payload_len,
                       uint8_t *out, size_t out_cap);

/* ---- streaming decoder (resync-safe) --------------------------------------
 * Feed bytes as they arrive from the UART; the decoder scans for SYNC, reads
 * TYPE/LEN, collects LEN+2 more bytes, checks CRC16, and hands back one
 * complete, validated frame via the callback. A dropped byte costs at most one
 * frame, never stream alignment (length-driven framing + SYNC re-anchor). */

typedef void (*fh_frame_cb_t)(fh_type_t type,
                              const uint8_t *payload, uint8_t len,
                              void *user);

/* Decoder state. Zero-initialize before first use (fh_decoder_init). */
typedef struct {
    uint8_t  buf[FH_MAX_FRAME]; /* working frame assembly buffer              */
    uint16_t have;              /* bytes currently in buf                     */
    uint8_t  want;              /* payload length once the header is parsed   */
    uint8_t  state;             /* internal parse state                       */
    /* diagnostics — handy on the hub UI */
    uint32_t good_frames;
    uint32_t crc_errors;
    uint32_t resyncs;
} fh_decoder_t;

void fh_decoder_init(fh_decoder_t *d);

/* Push `n` received bytes through the decoder. For every complete, CRC-valid
 * frame found, `cb` is invoked with the payload (a pointer INTO the decoder's
 * buffer — valid only for the duration of the callback; copy if you keep it). */
void fh_decoder_push(fh_decoder_t *d, const uint8_t *bytes, size_t n,
                     fh_frame_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
