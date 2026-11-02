/*
 * fh_hub_bridge.h — USB host bridge: the TUI/Medusa <-> hub link over the UART0 console (onboard CH343 USB-UART on the USB-C, 921600).
 *
 * TWO-MODE CONSOLE (owner decision 2026-10-22):
 *   MONITOR mode (boot default): human-readable ESP_LOG text to USB. `idf.py
 *     monitor` works normally; you watch boot/status. No binary on the wire.
 *   INTERACTIVE mode: the hub mirrors every validated fh_protocol frame OUT to
 *     USB (so Medusa sees the live stream) and reads command frames IN from USB
 *     (ping / on-demand REQ → forwarded to the Pico, honoring hub-as-arbiter).
 *
 * HANDSHAKE:
 *   host --FH_BR_ATTACH(0x70)--> hub     : hub switches MONITOR -> INTERACTIVE,
 *                                          replies FH_BR_ATTACH_ACK(0x72).
 *   host --FH_BR_HOST_HB(0x73)--> hub    : keep-alive; hub reverts to MONITOR if
 *                                          none seen for FH_BR_ATTACH_TIMEOUT_MS.
 *   host --FH_BR_DETACH(0x71)--> hub     : clean exit; hub reverts immediately.
 *
 * These control TYPE codes live in the 0x70..0x7B band that fh_frame.h leaves
 * free (it uses 0x7C..0x7F). They are a HUB<->HOST concern, intentionally kept
 * OUT of the frozen fh_frame.h (which is the inter-board contract).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "fh_frame.h"

/* host-bridge control TYPE codes (free band; not in frozen fh_frame.h) */
#define FH_BR_ATTACH       0x70u   /* host -> hub : enter interactive mode      */
#define FH_BR_DETACH       0x71u   /* host -> hub : leave, revert to monitor    */
#define FH_BR_ATTACH_ACK   0x72u   /* hub  -> host: ack, now in interactive mode*/
#define FH_BR_HOST_HB      0x73u   /* host -> hub : keep-alive heartbeat        */

#define FH_BR_ATTACH_TIMEOUT_MS  5000u  /* revert to monitor after this silence  */

/* Start the bridge subsystem (spawns the USB reader task). Boots in MONITOR
 * mode. Safe to call once from app_main after the aggregator is up. */
bool fh_bridge_start(void);

/* True when a host (Medusa/TUI) is attached and the hub is in INTERACTIVE mode.
 * The UI/status layer checks this to SUPPRESS human-text prints while attached
 * (so binary frames and log text never interleave on the USB console). */
bool fh_bridge_is_interactive(void);

/* Mirror one validated inbound frame OUT to the host, IFF interactive. Called
 * from the aggregator's on_frame for every frame (cheap no-op in monitor mode).
 * `type` is the frame TYPE; payload/len is its body. The bridge re-encodes a
 * full frame (SYNC/TYPE/LEN/PAYLOAD/CRC) to USB. */
void fh_bridge_mirror_frame(fh_type_t type, const uint8_t *payload, uint8_t len);

/* Emit an arbitrary frame to the host (used for ATTACH_ACK and for RESPs the
 * hub forwards back). No-op unless interactive. */
void fh_bridge_emit(uint8_t type, const uint8_t *payload, uint8_t len);
