/*
 * fh_hub_link.c — FHAdvanced-REMIX hub SPI link layer (the aggregator) impl.
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule). P4 / ESP-IDF 6.1.
 * See KNOWN RISKS at EOF. This is PASS 1 (framework-first): bring up both SPI
 * buses, prove fh_protocol frames move both directions, route them to the
 * logger/health/time sinks. NO Wi-Fi capture interpretation, NO blade radio
 * logic — those are the modules that plug in later.
 *
 * ---------------------------------------------------------------------------
 * FIXED-SIZE TRANSACTION MODEL (owner decision 2026-10-25, option A)
 * ---------------------------------------------------------------------------
 * The simplest thing that provably moves a frame: every SPI transaction clocks
 * a FIXED block of FH_SPI_XFER_BYTES. A slave with a shorter frame pads with
 * 0x00; the resync-safe fh_decoder_t skips pad bytes while hunting for SYNC, so
 * padding costs nothing but a few wasted clocks. Bus A (the fast quad bus) has
 * a documented upgrade path to a two-phase length-header / multi-frame drain
 * once bench-proven — PARKED (FUTURE-A1). Pass 1 proves the base form first.
 * ======================================================================== */
#include "fh_hub_link.h"
#include "fh_hub_pins.h"
#include "fh_hub_bridge.h"
#include "fh_hub_req.h"        /* fh_req_on_frame: RESP matching + flight clear */
#include "fh_hub_logger.h"
#include "fh_hub_health.h"
#include "fh_hub_time.h"       /* fh_time_on_gps_fix (0x02 consumer)           */
#include "fh_hub_config.h"
#include "fh_frame.h"
#include "fh_ondemand.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"       /* esp_rom_delay_us (blade two-phase gap) */
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_serial_slave_link/essl_spi.h"   /* Bus A: ESSL HD master to the peers */
#include <string.h>
#include <stdio.h>

static const char *TAG = "fh_link";

/* BENCH PIN-MAP TEST: uncomment to flash the pin-map test build (GPIO31/32/33/46
 * blink as plain GPIOs ~1 Hz; probe the header pins with a meter). Recomment
 * (and rebuild) to restore normal firmware. See fh_link_start(). */
/* #define FH_BENCH_PIN_MAP 1 */

#define FH_BENCH_RX_DUMP 1   /* BENCH: log raw first bytes of each blade response */

/* One fixed transaction size. 261 = FH_MAX_FRAME (one largest frame). Clocking
 * a full max frame per transaction guarantees any single frame fits in one go. */
#define FH_SPI_XFER_BYTES   FH_MAX_FRAME   /* 261 */

/* SPI host assignment on the P4. SPI2 and SPI3 are the general-purpose hosts
 * (SPI0/1 serve flash/PSRAM). Bus A (quad peers) on SPI2; Bus B (blades) on
 * SPI3 — two independent controllers => they run concurrently, no shared lock
 * between buses. VERIFY the host-to-pin IOMUX legality at bring-up (both buses
 * route through the GPIO matrix, so arbitrary GPIO is accepted; see the pin
 * budget baseline). */
/* BENCH FIX (2026-10-30): swapped. On the P4, SPI2 has 6 hardware CS lines but
 * SPI3 has only 3 (SPI_LL_PERIPH_CS_NUM in esp_hal_gpspi/esp32p4 spi_ll.h).
 * Bus B needs 6 blade CS, so on SPI3 the 4th spi_bus_add_device failed
 * (ESP_ERR_NO_MEM). Bus A has exactly 3 peers, so it fits SPI3. VERIFY that
 * SPI3 accepts SPICOMMON_BUSFLAG_QUAD; if Bus A init fails, that is why. */
#define FH_BUSA_HOST        SPI3_HOST
#define FH_BUSB_HOST        SPI2_HOST

/* Bus A quad clock. GPIO-matrix routing caps ~26-40 MHz; start conservative at
 * 10 MHz for first bring-up (plenty for distilled events) and raise once the
 * link is proven. Bus B single-SPI at 8 MHz (radios are low-rate). */
#define FH_BUSA_CLK_HZ      (10 * 1000 * 1000)
#define FH_BUSB_CLK_HZ      (1  * 1000 * 1000)  /* BENCH: temporarily 1 MHz for */
                                    /* jumper-wired bring-up. RAISE BACK TO     */
                                    /* 8 MHz once the blades answer reliably    */
                                    /* (8*1000*1000) — blades are configured    */
                                    /* for 8 MHz; GPS proved it. See            */
                                    /* CONTINUITY.md §5 (clock-raise reminder).  */

/* ============================================================ per-link state */
typedef struct {
    fh_peer_t        id;
    int              cs_pin;
    int              irq_pin;
    spi_device_handle_t dev;
    fh_decoder_t     dec;
    fh_link_stats_t  stats;
    uint8_t          src_id;      /* wire src_id this peer carries (0 = none)   */
    volatile bool    irq_pending; /* set in ISR, cleared by the service task    */
    bool             enabled;     /* gated by config band enables               */
} peer_link_t;

typedef struct {
    uint8_t             slot;
    int                 cs_pin;
    spi_device_handle_t dev;
    fh_decoder_t        dec;
    fh_link_stats_t     stats;
    uint8_t             src_id;   /* wire src_id this blade carries (0 = none;   */
                                  /* the Pico on-demand blade carries FH_SRC_PICO)*/
    bool                present;  /* a device was configured on this slot       */
} blade_link_t;

static peer_link_t  s_peers[FH_PEER_COUNT];
static blade_link_t s_blades[FH_BLADE_COUNT];

/* Bus mutexes: one per bus. Every transaction on a bus takes its bus's mutex,
 * so IRQ-drain and PING/display-push on the SAME bus never interleave on the
 * shared MOSI/MISO/CLK. Separate buses => separate mutexes, never nested
 * (the carried gen1 concurrency rule: new sink -> new mutex). */
static SemaphoreHandle_t s_busA_mtx;
static SemaphoreHandle_t s_busB_mtx;

/* Task notification target: the Bus-A service task, woken by any peer ISR. */
static TaskHandle_t s_busA_task;

/* ====================================================== the routing callback
 * PORTED VERBATIM in intent from gen1 fh_hub_agg.c on_frame(). The ONLY change
 * vs gen1 is the byte source (SPI transaction vs uart_read_bytes); the routing
 * of a validated frame to its sink is identical, because the frame format is
 * frozen and transport-independent. `user` is the peer_link_t* or blade_link_t*
 * whose decoder produced the frame (we only read ->stats/->src_id, shape-common
 * in the first fields; we pass the src_id explicitly where needed). */
static void route_frame(fh_type_t type, const uint8_t *payload, uint8_t len,
                        uint8_t src_id)
{
    /* Mirror every validated frame to the host bridge (Medusa) when attached;
     * cheap no-op in monitor mode. Keeps Medusa working on the P4 hub. */
    fh_bridge_mirror_frame(type, payload, len);

    /* On-demand RESP tracking (concern #1): match a radio-blade RESP against
     * the requester's flight table and complete the job (result file + clear).
     * Runs in whichever task clocked the frame in; for radio blades that is
     * ONLY the requester's own polls (R1 in fh_hub_req.c). */
    fh_req_on_frame((uint8_t)type, payload, len);

    /* Liveness: ANY validated frame proves the satellite is alive. Without
     * this the Pico blade (no HEARTBEATs — polled/fired) is never marked seen
     * and reads "dn" forever. src_id 0 = link has no owner, skip. Heartbeat
     * frames also call on_heartbeat below, which refines the state with the
     * capture-progress check (HEALTHY vs QUIET_BUT_ALIVE). */
    if (src_id != 0) { fh_health_on_frame(src_id); }

    switch (type) {
        case FH_TYPE_CAPTURE: {
            if (len >= FH_CAPTURE_FIXED) {
                fh_capture_t cap;
                memset(&cap, 0, sizeof(cap));
                memcpy(&cap, payload, len > sizeof(cap) ? sizeof(cap) : len);
                fh_log_submit_capture(&cap);
            }
            break;
        }
        case FH_TYPE_SUMMARY:
            /* per-device counts; first pass folds into the capture mapping.
             * Parked: a distinct SUMMARY row shape (same as gen1). */
            break;
        case FH_TYPE_SSID: {
            /* BSSID -> SSID mapping (fh_ssid_t, 39 B). Persist it to the SD
             * recon log so the SSID<->BSSID association is available offline
             * (and for duplicate-SSID analysis), not just live to Medusa. */
            if (len >= sizeof(fh_ssid_t)) {
                fh_ssid_t sd;
                memcpy(&sd, payload, sizeof(sd));
                fh_log_record_t r = {0};
                /* source reflects which Wi-Fi sniffer reported it (link src_id) */
                r.source_str = (src_id == FH_SRC_C5_WIFI5)   ? FH_SRCSTR_C5_WIFI5
                             : (src_id == FH_SRC_XS3_WIFI24) ? FH_SRCSTR_XS3_WIFI24
                             : FH_SRCSTR_XS3_WIFI24;   /* default to 2.4 label */
                memcpy(r.mac, sd.mac, 6);
                r.has_mac = true;
                r.has_rssi = false;
                /* render the SSID into extra[], CSV-safe + bounded. ssid_len is
                 * clamped to the 32-byte field; 0 => hidden. */
                uint8_t n = sd.ssid_len > 32 ? 32 : sd.ssid_len;
                if (n == 0) {
                    snprintf(r.extra, sizeof(r.extra), "ssid=<hidden>");
                } else {
                    char tmp[33];
                    for (uint8_t i = 0; i < n; ++i) {
                        uint8_t ch = sd.ssid[i];
                        /* keep it CSV-safe: strip commas/newlines/control */
                        tmp[i] = (ch >= 0x20 && ch < 0x7F && ch != ',') ? (char)ch : '.';
                    }
                    tmp[n] = '\0';
                    snprintf(r.extra, sizeof(r.extra), "ssid=%s", tmp);
                }
                fh_log_submit(&r);
            }
            break;
        }
        case FH_TYPE_GPS_FIX: {
            /* GPS blade (polled on a schedule) reports a FROZEN fh_gps_fix_t
             * (16 B). Hand to the time base: caches it + sets RTC on first fix.
             * Use the frame length as the authority. */
            if (len >= sizeof(fh_gps_fix_t)) {
                fh_gps_fix_t gf;
                memcpy(&gf, payload, sizeof(gf));
                fh_time_on_gps_fix(&gf);
            }
            break;
        }
        case FH_TYPE_HEARTBEAT: {
            if (len >= sizeof(fh_heartbeat_t)) {
                fh_heartbeat_t hb;
                memcpy(&hb, payload, sizeof(hb));
                fh_health_on_heartbeat(&hb);
            }
            break;
        }
        case FH_TYPE_PONG: {
            if (len >= sizeof(fh_ping_t)) {
                const fh_ping_t *pg = (const fh_ping_t *)payload;
                fh_health_on_pong(src_id, pg->seq);
            }
            break;
        }
        case FH_TYPE_LORA_RELAY_RX: {
            /* Inbound LoRa from the sensor stick (opt-in comms-event log). */
            fh_log_record_t r = {0};
            r.source_str = FH_SRCSTR_PICO_LORA;
            r.has_mac = false; r.has_rssi = false;
            snprintf(r.extra, sizeof(r.extra), "lora_rx len=%u", (unsigned)len);
            fh_log_submit(&r);
            break;
        }
        default:
            /* on-demand RESP frames (0x20..0x3F) from a fired blade land here;
             * the hub's on-demand requester state machine (PARKED, later pass)
             * will match seq and turn a RESP into a pico-<radio> CSV row.
             * Counted as a good frame already. */
            break;
    }
}

/* Decoder trampolines: the fh_decoder_t callback hands us `user`; we recover
 * the src_id from the link and call route_frame. Separate trampolines so a
 * peer carries its wire src_id and a blade carries none (0). */
static void peer_cb(fh_type_t type, const uint8_t *payload, uint8_t len, void *user)
{
    peer_link_t *lk = (peer_link_t *)user;
    lk->stats.good_frames = lk->dec.good_frames;
    lk->stats.crc_errors  = lk->dec.crc_errors;
    lk->stats.resyncs     = lk->dec.resyncs;
    route_frame(type, payload, len, lk->src_id);
}
static void blade_cb(fh_type_t type, const uint8_t *payload, uint8_t len, void *user)
{
    blade_link_t *bl = (blade_link_t *)user;
    bl->stats.good_frames = bl->dec.good_frames;
    bl->stats.crc_errors  = bl->dec.crc_errors;
    bl->stats.resyncs     = bl->dec.resyncs;
    route_frame(type, payload, len, bl->src_id /* FH_SRC_PICO for the Pico blade, else 0 */);
}

/* ============================================================ Bus A — peers */

/* GPIO ISR: a peer asserted its IRQ. Mark it pending and wake the service
 * task. Kept minimal (no SPI in ISR context — SPI runs in the task). */
static void IRAM_ATTR peer_irq_isr(void *arg)
{
    peer_link_t *lk = (peer_link_t *)arg;
    lk->irq_pending = true;
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_busA_task, &hp);
    if (hp == pdTRUE) { portYIELD_FROM_ISR(); }
}

/* Drain one peer: take Bus-A mutex, select CS, clock a fixed block, release,
 * push through the peer's decoder. A quad-SPI half-duplex read clocks the
 * slave's outbox in. (Pass 1 reads a single fixed block per IRQ; a multi-block
 * drain for a bursty peer is FUTURE-A1.) */
static void busA_drain_peer(peer_link_t *lk)
{
    static uint8_t rx[FH_SPI_XFER_BYTES];   /* task-context static: one drainer */

    /* ESSL Rd_DMA: pull the peer's staged outbound frame (it raised its IRQ to
     * say a TX DMA buffer is ready). HD is half-duplex, so this is a read-only
     * segment. The bus mutex serializes Bus-A access. */
    xSemaphoreTake(s_busA_mtx, portMAX_DELAY);
    /* Segment-mode Rd_DMA against the slave's staged TX. The slaves use
     * spi_slave_hd_queue_trans (segment mode), so the matching master call is
     * essl_spi_rddma (NOT the Append get_packet path). seg_len = -1 sends it in
     * one segment. No length is returned (segment mode); we read the fixed
     * FH_SPI_XFER_BYTES and let the fh_frame decoder find the SYNC+CRC. flags 0
     * = 1-bit HD; add SPI_TRANS_MODE_QIO when widened (decision doc / #9823). */
    esp_err_t e = essl_spi_rddma(lk->dev, rx, FH_SPI_XFER_BYTES,
                                 -1 /*one segment*/, 0 /*flags*/);
    xSemaphoreGive(s_busA_mtx);

    if (e == ESP_OK) {
        lk->stats.transactions++;
        lk->stats.bytes_rx += FH_SPI_XFER_BYTES;
        fh_decoder_push(&lk->dec, rx, FH_SPI_XFER_BYTES, peer_cb, lk);
    } else {
        ESP_LOGW(TAG, "busA Rd_DMA peer %d err %s", lk->id, esp_err_to_name(e));
    }
}

/* Bus-A service task: sleeps on a task notification; on wake, drains every peer
 * whose irq_pending is set. One task serves all peers (the bus is shared, so
 * serial drain is correct and simplest). */
static void busA_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* block until an ISR wakes us */
        for (int i = 0; i < FH_PEER_COUNT; ++i) {
            peer_link_t *lk = &s_peers[i];
            if (lk->enabled && lk->irq_pending) {
                lk->irq_pending = false;
                lk->stats.irq_events++;
                busA_drain_peer(lk);
            }
        }
    }
}

bool fh_link_send_peer(fh_peer_t peer, const uint8_t *frame, size_t n)
{
    if (peer >= FH_PEER_COUNT || !frame || n == 0 || n > FH_SPI_XFER_BYTES) {
        return false;
    }
    peer_link_t *lk = &s_peers[peer];
    if (!lk->enabled) { return false; }

    static uint8_t tx[FH_SPI_XFER_BYTES];

    /* ESSL Wr_DMA: push a control frame to the peer (the peer keeps an RX DMA
     * buffer queued for exactly this). Fill the shared tx buffer INSIDE the bus
     * mutex (shared by every send_peer caller). */
    xSemaphoreTake(s_busA_mtx, portMAX_DELAY);
    memset(tx, 0, sizeof(tx));
    memcpy(tx, frame, n);                 /* pad to fixed size with 0x00        */
    esp_err_t e = essl_spi_wrdma(lk->dev, tx, FH_SPI_XFER_BYTES,
                                 -1 /*one segment*/, 0 /*flags; QIO when widened*/);
    xSemaphoreGive(s_busA_mtx);
    if (e == ESP_OK) { lk->stats.transactions++; }
    else { ESP_LOGW(TAG, "busA Wr_DMA peer %d err %s", peer, esp_err_to_name(e)); }
    return e == ESP_OK;
}

/* ============================================================ Bus B — blades */

bool fh_link_blade_xfer(uint8_t slot, const uint8_t *req, size_t req_n)
{
    if (slot >= FH_BLADE_COUNT || !s_blades[slot].present) { return false; }
    if (req_n > FH_SPI_XFER_BYTES) { return false; }
    blade_link_t *bl = &s_blades[slot];

    static uint8_t tx[FH_SPI_XFER_BYTES];
    static uint8_t rx[FH_SPI_XFER_BYTES];
    /* These statics are safe ONLY because s_busB_mtx (taken below) serializes
     * every Bus-B xfer — one caller is inside this function at a time. Any
     * future Bus-B path that bypasses the mutex would race these buffers. */
    /* TWO-PHASE (multi-check #3): a SPI-slave blade cannot compute its answer
     * within the SAME clocked frame that delivers the request — it stages the
     * response for the NEXT transaction (see the blade firmwares' on_data ->
     * service_pending -> hub_stage flow). So:
     *   phase 1: clock the request OUT (what the blade clocks IN this frame is
     *            whatever it had staged before — we ignore it here).
     *   phase 2: clock an idle frame; what comes back is the blade's response
     *            to our phase-1 request (staged between the two transactions).
     * Both phases take the Bus-B mutex individually so another Bus-B caller
     * cannot interleave between our two transactions. A short gap lets the
     * blade's loop() stage the reply. For a LONG fired op (e.g. a multi-second
     * cc1101 capture) phase 2 will read BUSY or stale bytes; the on-demand
     * REQUESTER state machine (parked) polls again later until the matching
     * seq arrives. This primitive is the correct one-exchange foundation. */

    xSemaphoreTake(s_busB_mtx, portMAX_DELAY);

    /* phase 1 — deliver the request */
    memset(tx, 0, sizeof(tx));
    if (req && req_n) { memcpy(tx, req, req_n); }
    spi_transaction_t t1 = {0};
    t1.length    = FH_SPI_XFER_BYTES * 8;
    t1.rxlength  = FH_SPI_XFER_BYTES * 8;
    t1.tx_buffer = tx;
    t1.rx_buffer = rx;               /* ignored (pre-request staged data) */
    esp_err_t e1 = spi_device_transmit(bl->dev, &t1);
#ifdef FH_BENCH_RX_DUMP
    static uint8_t rx1[FH_SPI_XFER_BYTES];
    memcpy(rx1, rx, FH_SPI_XFER_BYTES);   /* keep phase-1 MISO for the bench dump */
#endif
    /* If phase 1 failed, the bus/blade is down — don't burn the 5 ms gap and a
     * doomed phase-2 clocking into a dead device. Release + report now; the
     * requester retries. (Multi-check 2026-11-01.) */
    if (e1 != ESP_OK) {
        xSemaphoreGive(s_busB_mtx);
        ESP_LOGW(TAG, "busB blade %u phase-1 spi err %s", slot, esp_err_to_name(e1));
        return false;
    }

    /* brief gap for the blade to stage its reply (its loop() runs at ~ms) */
    /* NOTE: held across the mutex deliberately — this exchange is atomic on
     * Bus B; the gap is short and Bus B is low-rate (fired/polled). BENCH FIX
     * (2026-10-31): the gap was 500 us, but the comment says the blade's
     * loop() stages its reply at ~ms granularity — so phase 2 usually read
     * PRE-REQUEST stale data and the one-exchange primitive almost never
     * closed in one exchange. 5 ms costs nothing (one 261 B transaction at
     * 8 MHz is ~260 us, so the gap is ~2% occupancy at a 1 Hz poll rate) and
     * makes phase 2 actually see the staged reply most of the time. A blade
     * that is still slower than the gap merely returns stale bytes again —
     * the parked on-demand requester polls until the matching seq arrives. */
    esp_rom_delay_us(5000);

    /* phase 2 — clock an idle frame; read the staged response */
    memset(tx, 0, sizeof(tx));
    spi_transaction_t t2 = {0};
    t2.length    = FH_SPI_XFER_BYTES * 8;
    t2.rxlength  = FH_SPI_XFER_BYTES * 8;
    t2.tx_buffer = tx;               /* idle (all-zero) poll */
    t2.rx_buffer = rx;
    esp_err_t e2 = spi_device_transmit(bl->dev, &t2);

    xSemaphoreGive(s_busB_mtx);

    if (e1 == ESP_OK && e2 == ESP_OK) {
        bl->stats.transactions += 2;
        bl->stats.bytes_rx += FH_SPI_XFER_BYTES;   /* count the response read */
#ifdef FH_BENCH_RX_DUMP
        /* BENCH: dump the FIRST 32 bytes of EACH phase's MISO, every
         * transaction pair. Phase 1 = what the blade had staged; Phase 2 = the
         * staged response. Frame format: A5 5A type len payload.. crc16. */
        ESP_LOGI(TAG, "BENCH p1: %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x",
                 rx1[0], rx1[1], rx1[2], rx1[3], rx1[4], rx1[5], rx1[6], rx1[7],
                 rx1[8], rx1[9], rx1[10], rx1[11], rx1[12], rx1[13], rx1[14], rx1[15],
                 rx1[16], rx1[17], rx1[18], rx1[19], rx1[20], rx1[21], rx1[22], rx1[23],
                 rx1[24], rx1[25], rx1[26], rx1[27], rx1[28], rx1[29], rx1[30], rx1[31]);
        ESP_LOGI(TAG, "BENCH p2: %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x",
                 rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7],
                 rx[8], rx[9], rx[10], rx[11], rx[12], rx[13], rx[14], rx[15],
                 rx[16], rx[17], rx[18], rx[19], rx[20], rx[21], rx[22], rx[23],
                 rx[24], rx[25], rx[26], rx[27], rx[28], rx[29], rx[30], rx[31]);
#endif
        fh_decoder_push(&bl->dec, rx, FH_SPI_XFER_BYTES, blade_cb, bl);
        return true;
    }
    ESP_LOGW(TAG, "busB blade %u spi err e1=%s e2=%s", slot,
             esp_err_to_name(e1), esp_err_to_name(e2));
    return false;
}

bool fh_link_send_blade(uint8_t slot, const uint8_t *frame, size_t n)
{
    /* Fire-and-forget: run an xfer but ignore the response drain's content.
     * (xfer already pushes any response through the decoder, which is fine —
     * a blade that answers a fire-and-forget command still gets routed.) */
    return fh_link_blade_xfer(slot, frame, n);
}

/* Map an on-demand REQ frame TYPE to the blade slot that hosts that radio
 * (REMIX one-radio-per-blade). Returns 0xFF for a type with no radio blade.
 * The host bridge / on-demand requester uses this to route a REQ to the right
 * blade instead of assuming a single shared Pico slot. */
uint8_t fh_link_slot_for_radio(uint8_t od_req_type)
{
    switch (od_req_type) {
        case FH_OD_CC1101_REQ:   return FH_BLADE_SLOT_CC1101;
        case FH_OD_NRF24_REQ:    return FH_BLADE_SLOT_NRF24;
        case FH_OD_NFC_REQ:      return FH_BLADE_SLOT_PN532;
        case FH_OD_LORA_TX_REQ:  return FH_BLADE_SLOT_SX1262;
        case FH_OD_LORA_LISTEN_REQ: return FH_BLADE_SLOT_SX1262;
        default: break;
    }
    /* EXTERN band (Tier 3, fh_ondemand.h): routing by band — 0x30..0x37 =
     * CC1101 blade, 0x38..0x3F = nRF24 blade. The blade answers the generic
     * EXT RESP (FH_OD_EXT_STUB until the external author's implementation). */
    if (od_req_type >= FH_OD_EXT_CC_JAM_REQ  && od_req_type <= 0x37) return FH_BLADE_SLOT_CC1101;
    if (od_req_type >= FH_OD_EXT_NRF_JAM_REQ && od_req_type <= 0x3F) return FH_BLADE_SLOT_NRF24;
    return 0xFF;   /* not a per-radio blade REQ */
}

/* GPS poll task: PERIODIC, not fired. Wakes on a cadence derived from
 * cfg.gps_rate (1 Hz => 1000 ms, 5 Hz => 200 ms) and polls the GPS blade slot.
 * The blade answers with a FROZEN 0x02 frame, which blade_xfer pushes through
 * the decoder -> route_frame -> fh_time_on_gps_fix(). Keeping a live walking/
 * driving fix is just this poll running at the configured rate; the bus cost
 * is ~16 B per poll (<0.1% occupancy even at 5 Hz), so fired radios sharing
 * Bus B are unaffected. The GPS REQUEST frame is a short on-demand poll; until
 * the SPI GPS-blade firmware is written (later this session) we send an empty
 * request and let the blade stream its latest fix on any clocked transaction. */
static void gps_poll_task(void *arg)
{
    (void)arg;
    /* A minimal GPS poll request frame. The GPS blade may ignore the body and
     * simply return its latest fix on any transaction; the exact on-demand
     * REQ opcode is finalized when the SPI GPS blade is written. */
    uint8_t req[FH_MAX_FRAME];
    size_t  req_n = fh_frame_encode(FH_TYPE_GPS_FIX, NULL, 0, req, sizeof(req));

    for (;;) {
        const fh_hub_cfg_t *cfg = fh_cfg_get();
        TickType_t period = (cfg && cfg->gps_rate == FH_GPS_5HZ)
                              ? pdMS_TO_TICKS(200)    /* 5 Hz */
                              : pdMS_TO_TICKS(1000);  /* 1 Hz (default)        */
        if (s_blades[FH_BLADE_SLOT_GPS].present) {
            /* BENCH: one log line per poll — proves the poll task fires and
             * reports the xfer result. REMOVE once the link is proven. */
            bool ok = fh_link_blade_xfer(FH_BLADE_SLOT_GPS, req, req_n);
            static uint32_t poll_n = 0;
            ESP_LOGI(TAG, "BENCH gps poll #%u xfer=%s", (unsigned)(++poll_n),
                     ok ? "ok" : "FAIL");
        }
        vTaskDelay(period);
    }
}

/* ============================================================ PING TX hook */

/* The health monitor pings a satellite by src_id. On the P4, the satellites
 * that send heartbeats are the Bus-A peers (XS3/XC5) and any heartbeating
 * blade (Pico). Map src_id -> the right bus/link and send down it. */
static bool link_send_ping(uint8_t src_id, uint8_t seq)
{
    uint8_t frame[FH_MAX_FRAME];
    fh_ping_t p = { .seq = seq };
    size_t n = fh_frame_encode(FH_TYPE_PING, (uint8_t *)&p, sizeof(p),
                               frame, sizeof(frame));
    if (n == 0) { return false; }

    /* Peers first (Bus A): match the wire src_id carried by a peer. */
    for (int i = 0; i < FH_PEER_COUNT; ++i) {
        if (s_peers[i].enabled && s_peers[i].src_id == src_id) {
            return fh_link_send_peer((fh_peer_t)i, frame, n);
        }
    }
    /* Blades (Bus B): match a blade that carries this src_id (the Pico blade
     * carries FH_SRC_PICO). Send the PING down its slot; its PONG returns on a
     * later Bus-B transaction and routes via blade_cb -> fh_health_on_pong. */
    for (int i = 0; i < FH_BLADE_COUNT; ++i) {
        if (s_blades[i].present && s_blades[i].src_id == src_id && src_id != 0) {
            return fh_link_send_blade((uint8_t)i, frame, n);
        }
    }
    return false;
}

/* ============================================================ bring-up */

static esp_err_t busA_init(void)
{
    /* Bus A = QUAD SPI, SPI Slave HD peers (committed 2026-11-01, see
     * ../remix-BUS-A-QUAD-HD-DECISION.md). The hub is an ESSL HD MASTER:
     * each peer is an spi_device_handle_t configured for HD command/address/
     * dummy phases, wrapped by an ESSL device context that issues Rd_DMA
     * (pull the peer's staged frame) and Wr_DMA (push control). FROZEN HD
     * params MUST match the peers' spi_slave_hd_slot_config_t:
     *   mode 0, command_bits=8, address_bits=8, dummy_bits=8, 261 B segments. */
    spi_bus_config_t bus = {0};
    bus.sclk_io_num     = FH_BUSA_CLK;
    bus.data0_io_num    = FH_BUSA_DQ0;   /* MOSI/DQ0 */
    bus.data1_io_num    = FH_BUSA_DQ1;   /* MISO/DQ1 */
    bus.data2_io_num    = FH_BUSA_DQ2;   /* DQ2 (QIO) */
    bus.data3_io_num    = FH_BUSA_DQ3;   /* DQ3 (QIO) */
    bus.data4_io_num    = -1;
    bus.data5_io_num    = -1;
    bus.data6_io_num    = -1;
    bus.data7_io_num    = -1;
    bus.max_transfer_sz = FH_SPI_XFER_BYTES;
    bus.flags           = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
    esp_err_t e = spi_bus_initialize(FH_BUSA_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) { return e; }

    const int cs[FH_PEER_COUNT]  = { FH_BUSA_CS_HEAD, FH_BUSA_CS_XS3, FH_BUSA_CS_XC5 };
    const int irq[FH_PEER_COUNT] = { FH_BUSA_IRQ_HEAD, FH_BUSA_IRQ_XS3, FH_BUSA_IRQ_XC5 };
    const uint8_t sid[FH_PEER_COUNT] = { FH_SRC_HEAD, FH_SRC_XS3_WIFI24, FH_SRC_C5_WIFI5 };

    for (int i = 0; i < FH_PEER_COUNT; ++i) {
        peer_link_t *lk = &s_peers[i];
        lk->id = (fh_peer_t)i; lk->cs_pin = cs[i]; lk->irq_pin = irq[i];
        lk->src_id = sid[i];
        fh_decoder_init(&lk->dec);

        /* The SPI master device for this peer. HD phases are carried by the
         * ESSL layer, but the device itself must allow a command+address+dummy
         * and HALF-DUPLEX (HD is half-duplex). Clock from FH_BUSA_CLK_HZ.
         * NOTE: for 1-bit-HD bring-up leave SPI_DEVICE_HALFDUPLEX only; to widen
         * to QIO, add the per-transaction QIO flag in the ESSL calls (see the
         * decision doc L2 / Espressif #9823 dummy-cycle item). */
        spi_device_interface_config_t dc = {0};
        dc.clock_speed_hz = FH_BUSA_CLK_HZ;
        dc.mode           = 0;                 /* FROZEN: CPOL0/CPHA0            */
        dc.spics_io_num   = cs[i];
        dc.queue_size     = 4;
        dc.command_bits   = 8;                 /* FROZEN HD params — match peers */
        dc.address_bits   = 8;
        dc.dummy_bits     = 8;
        dc.flags          = SPI_DEVICE_HALFDUPLEX;
        e = spi_bus_add_device(FH_BUSA_HOST, &dc, &lk->dev);
        if (e != ESP_OK) {
            /* Partial-init cleanup: remove the peers added so far + free the
             * bus, so a failed boot leaves no orphaned device handles. */
            for (int j = 0; j < i; ++j) {
                if (s_peers[j].dev) { spi_bus_remove_device(s_peers[j].dev); s_peers[j].dev = NULL; }
            }
            spi_bus_free(FH_BUSA_HOST);
            return e;
        }

        /* NOTE: no essl_spi_init_dev / essl_handle_t needed. That handle is for
         * the ESSL *Append* mode (essl_spi_get_packet/send_packet) and the
         * generic essl.h path. We use the SEGMENT-mode DMA calls
         * essl_spi_rddma()/essl_spi_wrdma(), which operate directly on the
         * spi_device_handle_t (lk->dev) — matching the slaves' segment-mode
         * spi_slave_hd_queue_trans. The device's command/address/dummy=8 +
         * HALFDUPLEX config above is the whole master-side HD setup. */
    }
    return ESP_OK;
}

static esp_err_t busB_init(void)
{
    spi_bus_config_t bus = {0};
    bus.sclk_io_num     = FH_BUSB_SCLK;
    bus.mosi_io_num     = FH_BUSB_MOSI;
    bus.miso_io_num     = FH_BUSB_MISO;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.data4_io_num    = -1;   /* see busA_init: {0} would mean GPIO 0 */
    bus.data5_io_num    = -1;
    bus.data6_io_num    = -1;
    bus.data7_io_num    = -1;
    bus.max_transfer_sz = FH_SPI_XFER_BYTES;
    esp_err_t e = spi_bus_initialize(FH_BUSB_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) { return e; }

    const int cs[FH_BLADE_COUNT] = {
        FH_BUSB_CS1, FH_BUSB_CS2, FH_BUSB_CS3,
        FH_BUSB_CS4, FH_BUSB_CS5, FH_BUSB_CS6
    };
    for (int i = 0; i < FH_BLADE_COUNT; ++i) {
        blade_link_t *bl = &s_blades[i];
        bl->slot = (uint8_t)i; bl->cs_pin = cs[i];
        /* src_id: the Pico blades carry the FROZEN wire src_id FH_SRC_PICO (3) —
         * BOTH the radio blade (heartbeats + answers PINGs) and the GPS blade
         * (polled). All other blade slots carry none (0). Without the GPS slot
         * carrying 3, its GPS_FIX frames route with src_id 0 and the health
         * monitor never sees the blade -> PICO=dn forever despite a working
         * link (bench 2026-11-01). */
        bl->src_id = (i == FH_BLADE_SLOT_PICO || i == FH_BLADE_SLOT_GPS)
                     ? FH_SRC_PICO : 0;
        fh_decoder_init(&bl->dec);

        spi_device_interface_config_t dc = {0};
        dc.clock_speed_hz = FH_BUSB_CLK_HZ;
        /* MODE 1 (CPOL=0, CPHA=1): the RP2040 SSP slave transmits only ONE byte
         * per CS assertion in MODE 0 (slave-select freezes the serial
         * peripheral register when SPH=0 — RP2040 datasheet / pico-examples
         * issue #115). Bus B's blades are RP2040-based, so the hub must clock
         * them in MODE 1. Bus A peers are unaffected (left at MODE 0). */
        dc.mode           = 1;
        dc.spics_io_num   = cs[i];
        dc.queue_size     = 2;
        e = spi_bus_add_device(FH_BUSB_HOST, &dc, &bl->dev);
        if (e != ESP_OK) {
            /* Partial-init cleanup: remove blades added so far + free the bus. */
            for (int j = 0; j < i; ++j) {
                if (s_blades[j].dev) { spi_bus_remove_device(s_blades[j].dev); s_blades[j].dev = NULL; }
            }
            spi_bus_free(FH_BUSB_HOST);
            return e;
        }
        /* PASS 1: mark every slot "present" so a wired blade enumerates. A real
         * presence probe (does a device ACK on this CS?) is FUTURE work; until
         * then a poll/xfer to an empty slot simply reads pad bytes (harmless,
         * the decoder finds no SYNC). GPS slot stays present for the poll task. */
        bl->present = true;
    }
    return ESP_OK;
}

bool fh_link_start(void)
{
#ifdef FH_BENCH_PIN_MAP
    /* BENCH PIN-MAP TEST: bypass SPI entirely. Drive GPIO31/32/33/46 as plain
     * GPIO outputs, toggling ~1 Hz. Probe each header pin you BELIEVE is
     * 31/32/33/46 with a meter: a blinking 0<->3.3 V = correct pin. A flat
     * pin = wrong pin (or not routed to the header). Flash this build, probe,
     * then rebuild WITHOUT the define to restore normal firmware. */
    {
        gpio_config_t io = {0};
        io.pin_bit_mask = (1ULL << FH_BUSB_SCLK) | (1ULL << FH_BUSB_MOSI) |
                          (1ULL << FH_BUSB_MISO) | (1ULL << FH_BUSB_CS1);
        io.mode = GPIO_MODE_OUTPUT;
        gpio_config(&io);
        int level = 0;
        for (;;) {
            level = !level;
            gpio_set_level(FH_BUSB_SCLK, level);
            gpio_set_level(FH_BUSB_MOSI, level);
            gpio_set_level(FH_BUSB_MISO, level);
            gpio_set_level(FH_BUSB_CS1, level);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
#endif
    s_busA_mtx = xSemaphoreCreateMutex();
    s_busB_mtx = xSemaphoreCreateMutex();
    if (!s_busA_mtx || !s_busB_mtx) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return false;
    }

    /* Config gates which capture peers we service (display head always on). */
    const fh_hub_cfg_t *cfg = fh_cfg_get();
    s_peers[FH_PEER_HEAD].enabled = true;
    s_peers[FH_PEER_XS3 ].enabled = cfg ? cfg->band_24_en : true;
    s_peers[FH_PEER_XC5 ].enabled = cfg ? cfg->band_5_en  : true;

    /* Health monitor FIRST, before anything here can fail. BENCH FIX
     * (2026-10-30): it used to start at the end, so a Bus B failure left its
     * mutex NULL and the UI task crashed in fh_health_node (boot loop). */
    if (!fh_health_start(link_send_ping)) {
        ESP_LOGW(TAG, "health monitor start failed");
    }

    /* Each *_init() cleans up its OWN devices + bus on failure (no orphaned
     * handles). NOTE (degraded boot, intentional): if Bus A comes up but Bus B
     * fails, we return false here while Bus A's bus/devices remain initialized.
     * That is deliberate — tearing down a WORKING Bus A because Bus B's blades
     * failed would be worse (the sniffer peers + head should still run). The
     * caller (fh_hub_main) logs and continues on false; a half-alive hub with
     * live Bus A is the preferred degraded state. */
    esp_err_t be = busA_init();
    if (be != ESP_OK) { ESP_LOGE(TAG, "Bus A init failed: %s", esp_err_to_name(be)); return false; }
    be = busB_init();
    if (be != ESP_OK) { ESP_LOGE(TAG, "Bus B init failed: %s", esp_err_to_name(be)); return false; }

    /* Bus-A service task must exist before we install the ISRs (they notify it).
     * Order is safe either way: the task blocks on ulTaskNotifyTake(portMAX_DELAY)
     * and ONLY the ISRs notify it, so it cannot run a drain before an ISR exists. */
    if (xTaskCreate(busA_task, "fh_busA", 4096, NULL, 12, &s_busA_task) != pdPASS) {
        ESP_LOGE(TAG, "busA task create failed");
        return false;
    }

    /* Install per-peer IRQ GPIOs (rising edge = "I have data"). */
    gpio_install_isr_service(0);
    for (int i = 0; i < FH_PEER_COUNT; ++i) {
        peer_link_t *lk = &s_peers[i];
        if (!lk->enabled) { continue; }
        gpio_config_t io = {0};
        io.pin_bit_mask = 1ULL << lk->irq_pin;
        io.mode         = GPIO_MODE_INPUT;
        io.pull_down_en = GPIO_PULLDOWN_ENABLE;   /* idle low; peer drives high  */
        io.intr_type    = GPIO_INTR_POSEDGE;
        gpio_config(&io);
        gpio_isr_handler_add(lk->irq_pin, peer_irq_isr, lk);
    }

    /* GPS poll task (periodic, cadence from cfg.gps_rate). */
    if (xTaskCreate(gps_poll_task, "fh_gps", 4096, NULL, 8, NULL) != pdPASS) {
        ESP_LOGW(TAG, "gps poll task create failed — GPS fixes will not update");
    }

    ESP_LOGI(TAG, "SPI link up: Bus A (quad, %d peers) + Bus B (%d blades)",
             FH_PEER_COUNT, FH_BLADE_COUNT);
    return true;
}

fh_link_stats_t fh_link_peer_stats(fh_peer_t peer)
{
    if (peer >= FH_PEER_COUNT) { fh_link_stats_t z = {0}; return z; }
    return s_peers[peer].stats;
}
fh_link_stats_t fh_link_blade_stats(uint8_t slot)
{
    if (slot >= FH_BLADE_COUNT) { fh_link_stats_t z = {0}; return z; }
    return s_blades[slot].stats;
}

/* ===========================================================================
 * KNOWN RISKS / PARKED (pass 1, write-by-reading only — NOT bench-verified)
 * ---------------------------------------------------------------------------
 * R1  SPI SLAVE SIDE UNWRITTEN. This is the MASTER. Each peer/blade must run an
 *     SPI *slave* that presents its outbox on a clocked read and accepts a
 *     frame on a clocked write. The slave-side firmware (XIAO-S3, XIAO-C5,
 *     Waveshare head, Pico blades) is separate work this session. Until it
 *     exists, a transaction reads pad bytes and the decoder finds no SYNC —
 *     harmless, proves the master path without a partner.
 * R2  QUAD HALF-DUPLEX ENUMERATION. SPI_DEVICE_HALFDUPLEX + a quad bus: confirm
 *     the IDF 6.1 spi_master accepts data0..3 on SPI2 over the GPIO matrix and
 *     that a plain spi_device_transmit drives 4-bit reads. If the driver wants
 *     an explicit SPI_TRANS_MODE_QIO flag per transaction, add it. BENCH.
 * R3  CS OWNERSHIP. The driver drives CS (spics_io_num). A shared-bus multidrop
 *     where several slaves share DQ lines needs each slave to TRISTATE its data
 *     outputs unless its CS is low — a SLAVE-firmware requirement, not a master
 *     one. Note it for the slave authors so two peers never both drive DQ.
 * R4  FIXED-SIZE WASTE / FUTURE-A1. Clocking 261 B per IRQ is wasteful when a
 *     peer has one 34 B capture. Fine for pass 1 (prove it moves). Upgrade path
 *     for Bus A: a 2-phase length header, or a multi-frame drain while IRQ
 *     stays asserted. PARKED until the base form is bench-proven.
 * R5  GPS REQUEST OPCODE PROVISIONAL. gps_poll_task sends an encoded 0x02 as a
 *     placeholder poll; the real on-demand GPS REQ opcode is finalized when the
 *     SPI GPS blade is written (later this session). The POLL CADENCE (1/5 Hz
 *     from cfg) and the routing of the returned 0x02 are correct now.
 * R6  CONCURRENCY. Bus A and Bus B each have ONE dedicated mutex; every
 *     transaction on a bus takes it (drain, send_peer, blade_xfer, gps poll,
 *     ping). No mutex is nested inside another. Verified by inspection: the
 *     only lock any path takes is its own bus mutex, held only across the
 *     spi_device_transmit, released before the decoder push. The decoder push
 *     (and its route_frame sinks) run OUTSIDE the bus mutex, so a slow sink
 *     never stalls the bus. Carried gen1 rule: new sink -> new mutex.
 * ======================================================================== */
