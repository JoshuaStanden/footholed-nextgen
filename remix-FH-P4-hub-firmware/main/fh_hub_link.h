/*
 * fh_hub_link.h — FHAdvanced-REMIX hub SPI link layer (the aggregator)
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule). P4 / ESP-IDF 6.1.
 *
 * REPLACES the old UART aggregator (fh_hub_agg) one-for-one in ROLE, but the
 * byte SOURCE changes from UART to SPI. The frozen fh_protocol frame format is
 * UNCHANGED: the same fh_decoder_t streaming decoder and the same per-frame
 * routing (CAPTURE->logger, HEARTBEAT/PONG->health, 0x02->time, LoRa->logger)
 * carry over verbatim. The decoder does not care whether bytes came off a UART
 * or an SPI transaction — it is a resync-safe byte stream either way.
 *
 * ---------------------------------------------------------------------------
 * WHY SPI NEEDS A DIFFERENT DRAIN MODEL THAN UART
 * ---------------------------------------------------------------------------
 * UART is symmetric: a satellite TX'd whenever it liked and the hub's RX task
 * blocked on uart_read_bytes(). SPI is MASTER-DRIVEN: a slave peer/blade cannot
 * put bytes on the wire until the hub (master) clocks it. So the hub must know
 * WHEN a slave has something to say. The mechanism (owner decision 2026-10-25):
 *
 *   BUS A (peers): each peer has a dedicated IRQ line into the hub. The peer
 *     asserts IRQ when it has one or more fh_protocol frames queued. The hub's
 *     ISR notes which peer, a service task selects that peer's CS, runs an SPI
 *     transaction to drain its outbox, and pushes the received bytes through
 *     THAT peer's fh_decoder_t. One decoder per peer (independent resync).
 *
 *   BUS B (blades): request/response, hub-initiated, one blade at a time (the
 *     frozen BUSY contract, fh_ondemand.h). The hub selects a blade's CS,
 *     clocks out a request frame, clocks in the response, pushes it through
 *     that blade's decoder. No free-running IRQ drain in pass 1 — a blade only
 *     speaks when asked. (A blade MAY also have an IRQ for "result ready" on a
 *     long op; parked for a later pass — pass 1 is poll-after-request.)
 *
 * ---------------------------------------------------------------------------
 * CONCURRENCY (carried lesson — the hub-bridge race that bit us 3x in gen1):
 * ---------------------------------------------------------------------------
 * Bus A and Bus B are SEPARATE SPI host controllers and never share a wire, so
 * they run concurrently with NO shared lock between them. But WITHIN a bus, the
 * shared MOSI/MISO/CLK are a single sink: every transaction on one bus is
 * serialized by that bus's own dedicated mutex. A peer's IRQ-drain and a
 * health PING down the same bus take the SAME bus mutex. New sink -> new mutex,
 * never nested. This is designed in from the start, not discovered later.
 * ======================================================================== */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "fh_frame.h"
#include "fh_hub_pins.h"   /* fh_peer_t, FH_BLADE_COUNT */

#ifdef __cplusplus
extern "C" {
#endif

/* Per-link decoder diagnostics for the health UI (same shape as gen1 agg). */
typedef struct {
    uint32_t good_frames;
    uint32_t crc_errors;
    uint32_t resyncs;
    uint32_t bytes_rx;
    uint32_t irq_events;   /* Bus A: times this peer asserted IRQ (0 on blades)*/
    uint32_t transactions; /* SPI transactions run against this link           */
} fh_link_stats_t;

/* Bring up BOTH SPI buses and start the drain machinery:
 *   - Bus A: init the quad-SPI master, configure the 3 peer CS lines, install
 *     the 3 IRQ GPIO ISRs, spawn the Bus-A service task + one decoder per peer.
 *   - Bus B: init the conventional SPI master, configure the 6 blade CS lines.
 *     No blade is probed at start (on-demand only); the bus sits idle until a
 *     request is issued.
 * Reads the config (band enables) to decide which peers to service: XC5 gated
 * by band_5_en, XS3 by band_24_en; the HEAD peer is always serviced (display).
 * Returns false only on a resource failure (SPI host install / task / mutex).
 * ======================================================================== */
bool fh_link_start(void);

/* ---- Bus A: peer drain is IRQ-driven internally. These are for the health
 * monitor's PING hook and for the display push to the head. --------------- */

/* Send a pre-encoded frame to a Bus-A peer (selects its CS, one transaction).
 * Serialized on the Bus-A mutex. Used by: the health PING hook (down any peer)
 * and the display feed to the HEAD peer. Returns true if the transaction ran. */
bool fh_link_send_peer(fh_peer_t peer, const uint8_t *frame, size_t n);

/* ---- Bus B: blade request/response (BUSY one-at-a-time). ----------------- */

/* Run one on-demand exchange with blade `slot` (0..FH_BLADE_COUNT-1): select
 * its CS, clock out `req` (a pre-encoded fh_protocol/ondemand frame), clock in
 * the response, and push the received bytes through that blade's decoder (so a
 * RESP/GPS/etc. frame is routed exactly like any other). Serialized on the
 * Bus-B mutex so only one blade is ever addressed at a time. Returns false if
 * the slot is out of range or the transaction failed. */
bool fh_link_blade_xfer(uint8_t slot, const uint8_t *req, size_t req_n);

/* Forward a pre-encoded frame to a blade (host-bridge command relay, mirrors
 * gen1 fh_agg_send_to_pico but blade-addressed). Thin wrapper over xfer with
 * no response drain for a fire-and-forget command. */
bool fh_link_send_blade(uint8_t slot, const uint8_t *frame, size_t n);

/* Map an on-demand REQ frame TYPE to the per-radio blade slot hosting it
 * (REMIX one-radio-per-blade). Returns 0xFF if the type has no radio blade.
 * Used by the host bridge / on-demand requester to target the right blade. */
uint8_t fh_link_slot_for_radio(uint8_t od_req_type);

/* ---- diagnostics -------------------------------------------------------- */
fh_link_stats_t fh_link_peer_stats(fh_peer_t peer);
fh_link_stats_t fh_link_blade_stats(uint8_t slot);

#ifdef __cplusplus
}
#endif
