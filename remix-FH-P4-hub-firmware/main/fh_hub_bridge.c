/*
 * fh_hub_bridge.c — USB host bridge (Medusa/TUI <-> hub). See fh_hub_bridge.h.
 *
 * UNVERIFIED FIRST DRAFT (no compiler/flash here — project rule). Design-faithful
 * to the frozen fh_frame.h and the hub's existing patterns. EXPECT a compile-fix
 * pass. I/O is Option B: the console owns UART0 (CH343 USB-UART, 921600); we read/write the
 * shared stdin/stdout (no driver install). The remaining verify point on real
 * IDF 6.1 is CR/LF translation being OFF so binary frames are not mangled — see
 * fh_bridge_start() and HUB_BRIDGE.md.
 *
 * Model: boots MONITOR (human ESP_LOG). On FH_BR_ATTACH it goes INTERACTIVE —
 * mirrors every validated frame OUT and relays host command frames to the Pico.
 * Reverts to MONITOR on DETACH or after FH_BR_ATTACH_TIMEOUT_MS of no host HB.
 */
#include "fh_hub_bridge.h"
#include "fh_hub_link.h"           /* fh_link_send_blade + FH_BLADE_SLOT_PICO */
#include "fh_hub_req.h"            /* the requester owns on-demand REQ jobs   */
#include "fh_hub_sysstate.h"       /* SYS_STATE (0x2D/0x2E) is hub-answered   */
#include "fh_ondemand.h"           /* the frozen REQ/RESP band + status codes */
#include <string.h>
#include <stdio.h>
#include <unistd.h>               /* read() on STDIN_FILENO */
#include <fcntl.h>                /* non-blocking stdin */
#include <stdarg.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "fh_bridge";

static atomic_bool  s_interactive = ATOMIC_VAR_INIT(false);
static atomic_llong s_last_host_hb_us = ATOMIC_VAR_INIT(0);
static fh_decoder_t s_dec;            /* decodes bytes arriving FROM the host   */
static SemaphoreHandle_t s_usb_tx_mtx; /* serialize stdout writes (NICKEL-2): agg-task mirror vs bridge-task ack */

/* ---- USB I/O via the CONSOLE's standard streams (Option B) ----------------
 * The IDF console already OWNS UART0 (via the CH343 USB-UART) (sdkconfig sets
 * the console there), so we do NOT install the driver. We write framed binary
 * to stdout (fflush after each frame so it isn't buffered) and read command
 * bytes from a non-blocking stdin. This keeps `idf.py monitor` working over the
 * same USB-C in MONITOR mode; INTERACTIVE just means "emit binary, suppress text".
 * REQUIREMENT: the console's line-ending/CR translation must be OFF so binary is
 * not mangled — see fh_bridge_start(). */

/* While INTERACTIVE, esp_log must not write text to stdout (it would corrupt the
 * binary frame stream). We install a vprintf hook that drops log output when
 * interactive and passes through to the default in monitor mode. */
static vprintf_like_t s_prev_vprintf = NULL;
static int s_log_vprintf(const char *fmt, va_list ap)
{
    if (atomic_load(&s_interactive)) { return 0; }      /* swallow logs on the wire */
    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
}

static void usb_write(const uint8_t *buf, size_t n)
{
    if (s_usb_tx_mtx) { xSemaphoreTake(s_usb_tx_mtx, portMAX_DELAY); }
    fwrite(buf, 1, n, stdout);
    fflush(stdout);
    if (s_usb_tx_mtx) { xSemaphoreGive(s_usb_tx_mtx); }
}

bool fh_bridge_is_interactive(void)
{
    return atomic_load(&s_interactive);
}

void fh_bridge_emit(uint8_t type, const uint8_t *payload, uint8_t len)
{
    if (!atomic_load(&s_interactive)) { return; }
    uint8_t frame[FH_MAX_FRAME];
    size_t n = fh_frame_encode((fh_type_t)type, payload, len, frame, sizeof(frame));
    if (n) { usb_write(frame, n); }
}

void fh_bridge_mirror_frame(fh_type_t type, const uint8_t *payload, uint8_t len)
{
    if (!atomic_load(&s_interactive)) { return; }    /* cheap no-op in monitor mode */
    fh_bridge_emit((uint8_t)type, payload, len);
}

static void go_interactive(void)
{
    /* refresh keep-alive on every ATTACH (a repeat ATTACH acts as a heartbeat) */
    atomic_store(&s_last_host_hb_us, (long long)esp_timer_get_time());
    /* idempotent (HB-2): if already interactive, do NOT re-emit a spurious ACK
     * the host is not waiting for — just treat the repeat ATTACH as a keep-alive. */
    if (atomic_exchange(&s_interactive, true)) { return; }
    ESP_LOGI(TAG, "host attached -> INTERACTIVE (human logs suppressed)");
    /* ack AFTER flipping so the ack is the first binary frame the host sees */
    uint8_t seq = 0;
    fh_bridge_emit(FH_BR_ATTACH_ACK, &seq, 1);
}

static void go_monitor(const char *why)
{
    bool was = atomic_exchange(&s_interactive, false);
    if (was) { ESP_LOGI(TAG, "host detached (%s) -> MONITOR", why); }
}

/* Handle one frame that arrived FROM the host over USB. */
static void on_host_frame(fh_type_t type, const uint8_t *payload, uint8_t len, void *user)
{
    (void)user;   /* payload IS used below (forwarded to the Pico) */
    switch ((uint8_t)type) {
        case FH_BR_ATTACH:
            go_interactive();
            return;
        case FH_BR_DETACH:
            go_monitor("detach frame");
            return;
        case FH_BR_HOST_HB:
            /* BENCH FIX (2026-10-30): a heartbeat also ATTACHES. Opening COM5
             * pulses the CH343's DTR/RTS (the auto-reset lines), which can
             * reboot the P4. Medusa sends ATTACH once at open; if the hub was
             * rebooting, that ATTACH is lost and the hub stayed in MONITOR
             * forever while Medusa kept heartbeating. go_interactive() is
             * idempotent (refreshes keep-alive, ACKs only on the first flip). */
            go_interactive();
            return;
        default:
            break;
    }
        /* Any other frame from the host is a COMMAND for a node. On-demand REQs
         * (the frozen 0x20..0x2C band) go through the REQUESTER (concern #1):
         * it fires the REQ, polls the blade until the matching seq RESP lands
         * (or the deadline passes), writes the result to SD, and the RESP is
         * mirrored to the host by route_frame — round-trip closes. A REQ with
         * no per-radio mapping (a bare PING) falls back to the CC1101 slot.
         * If the slot already has a job in flight, the BUSY contract says
         * answer BUSY rather than queue deeply — so the host gets an
         * immediate synthetic BUSY RESP (echoing its seq) instead of silence.
         * Non-on-demand frames (none known today) keep the fire-and-forget
         * path so an unknown host command still reaches a blade. */
    if (atomic_load(&s_interactive)) {
        uint8_t req_type = (uint8_t)type;
        /* SYS_STATE (0x2D/0x2E) is a HUB-answered verb: the hub builds the
         * RESP itself (SD/uptime/config/job health). No blade, no slot, so
         * the one-at-a-time BUSY contract does not apply. */
        if (req_type == FH_OD_SYS_STATE_REQ) {
            uint8_t seq = (len > 0) ? payload[0] : 0;
            uint8_t pl[sizeof(fh_od_sys_state_resp_t)];
            uint8_t pl_len = 0;
            fh_sysstate_build(seq, pl, &pl_len);
            fh_bridge_emit((fh_type_t)FH_OD_SYS_STATE_RESP, pl, pl_len);
            return;
        }
        bool is_od_req = (req_type >= FH_OD_PING_REQ && req_type <= FH_OD_LORA_LISTEN_REQ)
                      && (req_type != FH_OD_LORA_RX_IND);   /* 0x2A is unsolicited */
        /* EXTERN band (Tier 3): 0x30..0x3E even = blade REQs, routed by band. */
        bool is_ext_req = (req_type >= FH_OD_EXT_CC_JAM_REQ && req_type <= 0x3F)
                       && ((req_type & 1u) == 0);            /* REQs are EVEN */
        if (is_od_req || is_ext_req) {
            uint8_t slot = fh_link_slot_for_radio(req_type);
            if (slot == 0xFF) { slot = FH_BLADE_SLOT_CC1101; }  /* generic fallback */
            uint8_t seq = (len > 0) ? payload[0] : 0;           /* payload[0] = seq */
            uint8_t frame[FH_MAX_FRAME];
            size_t n = fh_frame_encode(type, payload, len, frame, sizeof(frame));
            if (n) {
                if (!fh_req_submit(slot, frame, n, 0)) {
                    /* slot busy (or requester down): BUSY RESP, echo the seq */
                    uint8_t busy[2] = { seq, FH_OD_BUSY };
                    fh_bridge_emit(fh_req_resp_type_for(req_type), busy, 2);
                }
            }
        } else {
            uint8_t frame[FH_MAX_FRAME];
            size_t n = fh_frame_encode(type, payload, len, frame, sizeof(frame));
            if (n) {
                uint8_t slot = fh_link_slot_for_radio(req_type);
                if (slot == 0xFF) { slot = FH_BLADE_SLOT_CC1101; }
                fh_link_send_blade(slot, frame, n);
            }
        }
    }
}

static void bridge_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[128];
    fh_decoder_init(&s_dec);
    /* non-blocking stdin so the task can also service the heartbeat timeout */
    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
    for (;;) {
        int r = read(STDIN_FILENO, buf, sizeof(buf));   /* console VFS stdin */
        if (r > 0) {
            fh_decoder_push(&s_dec, buf, (size_t)r, on_host_frame, NULL);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));               /* nothing waiting */
        }
        /* heartbeat timeout: revert to monitor if the host went quiet */
        if (atomic_load(&s_interactive)) {
            long long now = (long long)esp_timer_get_time();
            long long last = atomic_load(&s_last_host_hb_us);
            if ((now - last) > (long long)FH_BR_ATTACH_TIMEOUT_MS * 1000) {
                go_monitor("heartbeat timeout");
            }
        }
    }
}

bool fh_bridge_start(void)
{
    /* Option B: the IDF console OWNS UART0, so we do NOT install the
     * driver. We just make the shared streams safe for binary:
     *   - stdout unbuffered (we fflush per frame anyway, belt-and-suspenders),
     *   - no CR/LF translation (binary must pass through untouched).
     * TRANSCRIBE/VERIFY on IDF 6.1: confirm the console VFS does not force CRLF.
     * If it does, disable it (CONFIG_NEWLIB_STDOUT_LINE_ENDING_LF / the
     * UART VFS CR config) so frames are not mangled. */
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!s_usb_tx_mtx) { s_usb_tx_mtx = xSemaphoreCreateMutex(); }
    s_prev_vprintf = esp_log_set_vprintf(s_log_vprintf);  /* drop logs while interactive */
    BaseType_t ok = xTaskCreate(bridge_rx_task, "fh_bridge", 4096, NULL, 6, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "bridge task create failed");
        return false;
    }
    ESP_LOGI(TAG, "bridge up (MONITOR mode; host sends FH_BR_ATTACH to go interactive)");
    return true;
}
