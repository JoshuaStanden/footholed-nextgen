/*
 * fh_ws_link.h — Waveshare HEAD Bus-A SPI-Slave-HD transport (to the P4 hub).
 *
 * Same contract + transport as the XIAO peers (spi_slave_hd, quad-capable).
 * The head is a Bus-A peer: it RECEIVES the hub's render/control frames
 * (on_control) and SENDS touch events + PONG upstream (pop). See
 * ../../remix-BUS-A-QUAD-HD-DECISION.md.
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
 * if nothing to send. Provided by main (drains the head's upstream queue:
 * touch events, PONG). */
typedef size_t (*fh_ws_pop_fn)(uint8_t *out, size_t cap);

/* called for each validated inbound frame the hub pushed (render model update,
 * PING, time, config...). */
typedef void (*fh_ws_control_fn)(fh_type_t type, const uint8_t *payload, uint8_t len);

typedef struct {
    fh_ws_pop_fn     pop;
    fh_ws_control_fn on_control;
} fh_ws_link_cfg_t;

/* Init the Bus-A SPI-Slave-HD link + IRQ-out GPIO + link task. */
bool fh_ws_link_start(const fh_ws_link_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
