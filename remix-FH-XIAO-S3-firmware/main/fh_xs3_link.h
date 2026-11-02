/*
 * fh_xs3_link.h — FHAdvanced-REMIX XIAO-S3 SPI-slave transport (to the P4 hub)
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule).
 *
 * This is the NEW transport that replaces the gen1 UART link. The XIAO-S3 is a
 * SPI SLAVE on the hub's Bus A. It cannot push, so:
 *   - it STAGES the next outbound frame (pulled via the pop() callback) into
 *     the slave transaction buffer and raises FH_XS3_IRQ_OUT ("come clock me");
 *   - when the hub clocks a transaction, the staged frame goes OUT (MISO) and
 *     whatever the hub sent comes IN (MOSI); inbound bytes are decoded and each
 *     validated control frame is handed to on_control();
 *   - when the outbound queue is empty, IRQ is lowered.
 *
 * The capture path (fh_xs3_main.c) is UNCHANGED; it only produces frames into a
 * queue and this link drains that queue — exactly the role the UART tx_task
 * had, now over SPI.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "fh_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* pop one outbound frame into `out` (>= FH_MAX_FRAME); return byte count, or 0
 * if the queue is empty. Provided by main (drains its TX queue). */
typedef size_t (*fh_xs3_pop_fn)(uint8_t *out, size_t cap);

/* called for each validated inbound control frame the hub clocked in. */
typedef void (*fh_xs3_control_fn)(fh_type_t type, const uint8_t *payload, uint8_t len);

typedef struct {
    fh_xs3_pop_fn     pop;
    fh_xs3_control_fn on_control;
} fh_xs3_link_cfg_t;

/* Init the SPI slave on the Bus-A pins, install the IRQ-out GPIO, spawn the
 * link task. Returns false on a resource failure. Call BEFORE the Wi-Fi
 * callback can fire (main enforces this). */
bool fh_xs3_link_start(const fh_xs3_link_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
