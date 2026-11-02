/*
 * fh_hub_sysstate.h — FH_OD_SYS_STATE_RESP builder (0x2D/0x2E hub-answered).
 *
 * The SYS_STATE REQ is a HUB-answered verb: the bridge builds the RESP in its
 * host-frame handler (no blade, no one-at-a-time slot). This module assembles
 * the payload from the hub's own subsystems so fh_hub_bridge.c stays thin.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Build a complete SYS_STATE RESP payload (seq + status + 74 B body) into
 * `out` (caller's buffer, must be >= sizeof(fh_od_sys_state_resp_t) = 74 B).
 * `out_len` receives the payload length. Never blocks. */
void fh_sysstate_build(uint8_t seq, uint8_t *out, uint8_t *out_len);
