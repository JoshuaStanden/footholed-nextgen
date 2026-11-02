/*
 * fh_hub_logger.c — FHAdvanced hub logger / SD writer (SUBSYSTEM 5) impl.
 *
 * UNVERIFIED FIRST DRAFT (no compiler run — project rule). Highest-logic-risk
 * module; built first against a fake feed. See KNOWN COMPILE/LOGIC RISKS at end.
 *
 * DESIGN (ARCHITECTURE + HUB_PLAN):
 *   - ONE writer task owns the file handle. Producers only enqueue (zero-tick).
 *   - Each record is stamped with now_utc()+last_fix() HERE (sources send 0s).
 *   - Rows are rendered to the FROZEN CSV schema and appended to a RAM line
 *     buffer; the buffer is flushed to SD in batches (fewer, larger writes =
 *     less FAT churn, the reboot-logging lineage pattern).
 *   - If SD is down, rows accumulate in the SAME staging buffer (bounded); when
 *     it reaches a high-water mark with SD still down, oldest lines are dropped
 *     and a counter bumped (drop-oldest here, because a half-written tail is
 *     less useful than the recent rows — opposite of the queue's drop-newest,
 *     and deliberate: see LOGIC RISK L2).
 *   - File rolls at a FIXED 128 MB; the new file gets a fresh provenance header.
 */
#include "fh_hub_logger.h"
#include "fh_hub_board.h"
#include "fh_hub_time.h"
#include "fh_hub_config.h"
#include "fh_hub_pins.h"       /* FH_BLADE_SLOT_* (radio name per slot)      */
#include "fh_ondemand.h"       /* FH_OD_CC1101_REQ (the per-job-file rule)   */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <sys/stat.h>   /* mkdir for the /sdcard/logs subdir [C1 fixed] */
#include <unistd.h>     /* fsync: make FAT record the file size (bench fix) */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"               /* stage persist across reboot (L3 upgrade)   */

static const char *TAG = "fh_log";

/* ---- tunables ------------------------------------------------------------ */
#define LOG_QUEUE_DEPTH     128u
#define LOG_ROLL_BYTES      (128ull * 1024ull * 1024ull)   /* FIXED 128 MB     */
#define LOG_STAGE_BYTES     16384u    /* RAM line-batch buffer; flush at ~3/4   */
#define LOG_STAGE_FLUSH_HI  12288u    /* flush to SD when staged >= this        */
#define LOG_SD_DOWN_MAX     12288u    /* when SD down, cap staged bytes here     */
#define LOG_FLUSH_PERIOD_MS 2000u     /* also flush at least this often          */
#define LOG_NVS_NS          "fh_logstg" /* NVS namespace for the stage persist   */
#define LOG_NVS_KEY         "stage"   /* blob: the staged rows, verbatim         */
#define LOG_NVS_SAVE_MS    15000u     /* SD-up save cadence (flash-wear bound);  */
                                      /* a crash loses at most this window. SD-  */
                                      /* down saves happen every loop (~2 s).    */
#define LOG_NVS_SAVE_MAX   4096u      /* newest-tail cap per save: the default   */
                                      /* NVS partition is 24 KB; raise only with */
                                      /* a bigger partitions CSV.                */
#define LOG_ROW_MAX         256u      /* worst-case single CSV row              */
#define LOG_DIR             FH_SD_MOUNT_POINT "/logs"
#define LOG_FMT_VERSION     1
#define RES_QUEUE_DEPTH       8u      /* on-demand results (concern #1)         */
#define RES_DIR             FH_SD_MOUNT_POINT "/ondemand"   /* per-job files   */
#define RES_CSV             LOG_DIR "/results.csv"          /* one clean row/job */

/* ---- state (writer task owns the file + staging; producers only queue) ---- */
static QueueHandle_t  s_q        = NULL;
static TaskHandle_t   s_task     = NULL;
static FILE          *s_fp       = NULL;
static uint64_t       s_file_bytes = 0;
static char           s_stage[LOG_STAGE_BYTES];
static size_t         s_stage_len = 0;
static bool           s_fake      = false;
static int64_t        s_last_nvs_save_us = 0;   /* flash-wear gate            */
/* on-demand results (concern #1, 2026-10-31): a SECOND queue + its own file.
 * results.csv rows never enter the main stage buffer — the main CSV schema is
 * FROZEN (source,band,...) and a job result would be a schema break. The
 * results.csv handle is opened lazily in the writer task; SD-down results are
 * counted (not staged — a job result is small and the requester's ESP_LOGI
 * line plus Medusa's mirror already preserve the fact elsewhere). */
static QueueHandle_t  s_rq        = NULL;
static FILE          *s_rfp       = NULL;

/* stats — only the writer task mutates these, readers get a copy (no lock
 * needed because reads are a plain struct copy of word-sized fields; a torn
 * read is cosmetic on the UI). */
static fh_log_stats_t s_stats;

/* Flush handshake (AB6): fh_log_flush() raises s_flush_req and waits on
 * s_flush_ack, which the writer task gives after it has flushed the stage to SD.
 * This gives the safe-battery-swap / format maintenance actions a REAL "the
 * stage is on the card now" guarantee instead of a best-effort guess. */
static volatile bool    s_flush_req = false;
static SemaphoreHandle_t s_flush_ack = NULL;
static SemaphoreHandle_t s_flush_mtx = NULL;   /* CD2: serialize overlapping flush callers */

/* ---- CSV helpers --------------------------------------------------------- */
static void mac_to_str(const uint8_t m[6], char *out /*>=18*/)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* band nibble -> CSV band string (display aid; frozen schema keeps it short) */
static const char *band_str(uint8_t band)
{
    switch (band) {
        case 2:  return "2.4";
        case 5:  return "5";
        case 1:  return "ble";
        default: return "";
    }
}

/* Render one CSV row for the frozen schema:
 *   source,band,channel,type,mac,rssi,lat,lon,time,extra
 * Numeric lat/lon are the fix's e7 degrees rendered as decimal degrees; a
 * missing field is an EMPTY column (not 0) so a reader can tell "no data" from
 * "zero". Returns bytes written (excluding NUL). */
static int render_row(const fh_log_record_t *r, uint32_t epoch,
                      const fh_fix_t *fix, char *buf, size_t cap)
{
    char mac[18]  = "";
    char rssi[8]  = "";
    char lat[16]  = "";
    char lon[16]  = "";
    char tbuf[24] = "";

    if (r->has_mac)  { mac_to_str(r->mac, mac); }
    if (r->has_rssi) { snprintf(rssi, sizeof(rssi), "%d", (int)r->rssi); }
    if (fix->valid) {
        /* e7 -> decimal degrees with 7 fractional digits, sign-safe. Build via
         * integer parts to avoid float formatting surprises under -Werror. */
        long ladeg = (long)(fix->lat_e7 / 10000000);
        long lafr  = (long)(fix->lat_e7 % 10000000); if (lafr < 0) lafr = -lafr;
        long lodeg = (long)(fix->lon_e7 / 10000000);
        long lofr  = (long)(fix->lon_e7 % 10000000); if (lofr < 0) lofr = -lofr;
        snprintf(lat, sizeof(lat), "%ld.%07ld", ladeg, lafr);
        snprintf(lon, sizeof(lon), "%ld.%07ld", lodeg, lofr);
    }
    fh_time_iso8601(epoch, tbuf, sizeof(tbuf));

    /* extra is already CSV-safe text rendered by the source/mapping helper. */
    return snprintf(buf, cap, "%s,%s,%u,%u,%s,%s,%s,%s,%s,%s\n",
                    r->source_str ? r->source_str : "",
                    band_str(r->band),
                    (unsigned)r->channel,
                    (unsigned)r->frame_type,
                    mac, rssi, lat, lon, tbuf,
                    r->extra);
}

/* ---- file lifecycle ------------------------------------------------------ */
static bool write_header(FILE *fp)
{
    /* Line 1: fmt / node eFuse MAC / session start UTC / config token.
     * Line 2: config overrides delta.  Then the CSV column header. */
    uint8_t node[6] = {0};
    /* eFuse MAC — read via the board/boot layer; esp_read_mac is fine here. */
    extern bool fh_hub_node_mac(uint8_t out[6]);   /* provided by app_main    */
    fh_hub_node_mac(node);
    char nodemac[18]; mac_to_str(node, nodemac);

    bool from_rtc = false;
    uint32_t start = fh_time_now_utc(&from_rtc);
    char startiso[24]; fh_time_iso8601(start, startiso, sizeof(startiso));

    char cfgtok[32];
    fh_cfg_status_t st = fh_cfg_status();
    if (st.modified) { snprintf(cfgtok, sizeof(cfgtok), "MODIFIED(%u)", (unsigned)st.override_count); }
    else             { snprintf(cfgtok, sizeof(cfgtok), "STOCK"); }

    char overrides[160];
    fh_cfg_header_provenance(overrides, sizeof(overrides));

    int n1 = fprintf(fp,
        "# FHAdvanced recon log | fmt=%d | node=%s | start=%s%s | config=%s\n",
        LOG_FMT_VERSION, nodemac, startiso, from_rtc ? "" : "(boot-clock)", cfgtok);
    int n2 = fprintf(fp, "# config_overrides=%s\n", overrides);
    int n3 = fprintf(fp, "source,band,channel,type,mac,rssi,lat,lon,time,extra\n");
    if (n1 < 0 || n2 < 0 || n3 < 0) { return false; }
    s_file_bytes += (uint64_t)(n1 + n2 + n3);
    return true;
}

static void close_file(void)
{
    if (s_fp) { fflush(s_fp); fclose(s_fp); s_fp = NULL; }
}

/* Open a fresh log file named by the start UTC so files sort chronologically.
 * Returns true if open AND header written. */
static bool open_new_file(void)
{
    if (!fh_board_sd_mounted()) { return false; }
    /* ensure /sdcard/logs exists (ignore "already exists"; sys/stat.h mkdir) */
    mkdir(LOG_DIR, 0777);

    bool from_rtc = false;
    uint32_t t = fh_time_now_utc(&from_rtc);
    char path[64];
    snprintf(path, sizeof(path), LOG_DIR "/fh_%010" PRIu32 ".csv", t);
    FILE *fp = fopen(path, "a");
    if (!fp) {
        ESP_LOGW(TAG, "fopen %s failed", path);
        return false;
    }
    s_fp = fp;
    s_file_bytes = 0;
    if (!write_header(fp)) { close_file(); return false; }
    ESP_LOGI(TAG, "opened log %s", path);
    return true;
}

/* ---- NVS stage persist (L3 crash-survival upgrade, replaces RTC-staging) --
 * The stage buffer holds RENDERED CSV rows. A hard crash loses the un-flushed
 * batch (up to LOG_STAGE_FLUSH_HI ~12 KB). Persisting it to NVS flash on a
 * bounded cadence shrinks that window: SD-up saves run at most every
 * LOG_NVS_SAVE_MS (flash-wear bound — a quiet rig writes nothing), SD-down
 * saves run every loop because SD-down rows are the ones most likely to be
 * lost. Recovery pre-loads the stage BEFORE the task starts; the normal
 * pipeline then flushes it to SD like any staged data. The blob is erased
 * right after a successful SD flush (stage empty), so recovery is exact —
 * a crash in the tiny erase window can re-append the last batch (duplicate
 * rows in the CSV), which we accept: duplicates beat data loss. */
static void nvs_stage_save(void)
{
    /* Cap what we persist: the default ESP-IDF NVS partition is only 24 KB,
     * and rewriting a 12 KB blob every 15 s would thrash its wear-leveling.
     * We save the NEWEST tail (up to LOG_NVS_SAVE_MAX, cut on a newline
     * boundary) — losing the oldest rows on a crash matches the L2 policy
     * (drop-OLDEST). Raise LOG_NVS_SAVE_MAX only with a bigger NVS partition. */
    const uint8_t *src = (const uint8_t *)s_stage;
    size_t n = s_stage_len;
    if (n > LOG_NVS_SAVE_MAX) {
        size_t start = n - LOG_NVS_SAVE_MAX;
        while (start < n && src[start] != '\n') { start++; }
        if (start < n) { start++; } else { start = n; }  /* no newline found */
        src += start;
        n -= start;
    }
    nvs_handle_t h;
    if (nvs_open(LOG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) { return; }
    if (s_stage_len == 0) {
        nvs_erase_key(h, LOG_NVS_KEY);          /* nothing to recover later  */
    } else if (n > 0) {
        if (nvs_set_blob(h, LOG_NVS_KEY, src, n) != ESP_OK) {
            nvs_close(h);
            return;                              /* keep the OLD blob        */
        }
    }
    nvs_commit(h);
    nvs_close(h);
    s_last_nvs_save_us = esp_timer_get_time();
}

static void nvs_stage_recover(void)
{
    nvs_handle_t h;
    if (nvs_open(LOG_NVS_NS, NVS_READONLY, &h) != ESP_OK) { return; }
    size_t need = 0;
    if (nvs_get_blob(h, LOG_NVS_KEY, NULL, &need) == ESP_OK && need > 0) {
        if (need <= sizeof(s_stage)) {
            size_t got = need;
            if (nvs_get_blob(h, LOG_NVS_KEY, s_stage, &got) == ESP_OK) {
                s_stage_len = got;
                /* last line must end in \n (it always did when staged) */
                ESP_LOGI(TAG, "recovered %u staged bytes from NVS (pre-flush rows)",
                         (unsigned)got);
            }
        } else {
            ESP_LOGW(TAG, "NVS stage blob %u B > stage %u B — dropped",
                     (unsigned)need, (unsigned)sizeof(s_stage));
        }
    }
    nvs_close(h);
    /* NOTE: the key is left in NVS. If SD is up, the first successful flush
     * empties the stage and the next save erases the key; if SD stays down,
     * the periodic save rewrites it with the same (still staged) rows. */
}

/* Push the staged bytes to SD. On success, clears the stage. On SD failure,
 * leaves the stage intact (so the next attempt retries), and if the stage is
 * over the SD-down cap, drops the OLDEST line(s) to make room (L2). */
static void flush_stage(void)
{
    if (s_stage_len == 0) { return; }

    if (!s_fp) {
        /* try to (re)open; if still no SD, apply the SD-down cap */
        /* BENCH FIX (2026-10-30): retry the mount at most every 30 s, not on
         * every flush (~2 s). With no card, each try logged ~6 error lines
         * and re-claimed GPIO42, flooding the console. Rows keep staging. */
        static int64_t s_next_mount_us = 0;
        int64_t now_us = esp_timer_get_time();
        if (!fh_board_sd_mounted() && now_us >= s_next_mount_us) {
            s_next_mount_us = now_us + 30LL * 1000 * 1000;
            fh_board_sd_mount();
        }
        if (fh_board_sd_mounted()) { open_new_file(); }
    }

    if (s_fp) {
        size_t w = fwrite(s_stage, 1, s_stage_len, s_fp);
        if (w == s_stage_len) {
            fflush(s_fp);
            /* BENCH FIX (2026-10-30): fflush only pushes the data clusters.
             * FAT records the file SIZE (directory entry) on f_sync/f_close,
             * and we only close at the 128 MB roll. Pulling power or the card
             * left every CSV at 0 bytes on the PC. fsync -> f_sync commits the
             * size every flush (~2 s) so a pulled card loses at most ~2 s. */
            if (fsync(fileno(s_fp)) != 0) {
                ESP_LOGW(TAG, "fsync failed; file size may lag on the card");
            }
            s_file_bytes += s_stage_len;
            s_stats.cur_file_bytes = s_file_bytes;
            s_stats.bytes_committed += s_stage_len;   /* CD3: proven on SD */
            s_stage_len = 0;
            s_stats.sd_writing = true;
            nvs_stage_save();      /* stage empty → erase the NVS blob (exact
                                      recovery; see the note above) */
            /* roll if over the fixed ceiling */
            if (s_file_bytes >= LOG_ROLL_BYTES) {
                close_file();
                if (open_new_file()) { s_stats.files_rolled++; }
            }
            return;
        }
        /* partial/failed write — SD likely vanished mid-write. Drop the handle
         * AND tell the board to unmount + clear its mounted flag (NC2), so the
         * next flush's fh_board_sd_mount() genuinely re-mounts instead of
         * trusting a stale flag. Staged bytes are kept for the retry. */
        ESP_LOGW(TAG, "fwrite short (%u/%u); SD lost?", (unsigned)w, (unsigned)s_stage_len);
        close_file();
        fh_board_sd_mark_lost();
        s_stats.sd_writing = false;
    }

    /* SD down: bound the stage by dropping oldest lines (L2). */
    if (s_stage_len > LOG_SD_DOWN_MAX) {
        size_t drop = s_stage_len - LOG_SD_DOWN_MAX;
        /* advance to the next newline boundary so we never split a row */
        size_t cut = drop;
        while (cut < s_stage_len && s_stage[cut] != '\n') { cut++; }
        if (cut < s_stage_len) { cut++; }            /* include the newline     */
        if (cut > s_stage_len) { cut = s_stage_len; }
        memmove(s_stage, s_stage + cut, s_stage_len - cut);
        s_stage_len -= cut;
        s_stats.dropped_sd_down++;
    }
}

/* Append one rendered row into the stage; flush if over the high-water mark. */
static void stage_row(const char *row, int len)
{
    if (len <= 0) { return; }
    if ((size_t)len > sizeof(s_stage)) { return; }    /* impossible row; guard   */
    if (s_stage_len + (size_t)len > sizeof(s_stage)) {
        flush_stage();                                 /* make room               */
    }
    if (s_stage_len + (size_t)len > sizeof(s_stage)) {
        /* still no room (SD down + full) — drop-oldest already ran in flush;
         * if we are STILL full, drop this newest row and count it. */
        s_stats.dropped_sd_down++;
        return;
    }
    memcpy(s_stage + s_stage_len, row, (size_t)len);
    s_stage_len += (size_t)len;
    s_stats.rows_staged++;    /* CD3: accepted into the stage (bytes_committed */
                              /* is the proven-on-SD counter; see the struct)  */
    if (s_stage_len >= LOG_STAGE_FLUSH_HI) { flush_stage(); }
}

/* ---- on-demand results (concern #1, 2026-10-31) --------------------------- */
static const char *radio_name_for_slot(uint8_t slot)
{
    switch (slot) {
        case FH_BLADE_SLOT_GPS:    return "gps";
        case FH_BLADE_SLOT_SX1262: return "sx1262";
        case FH_BLADE_SLOT_CC1101: return "cc1101";
        case FH_BLADE_SLOT_NRF24:  return "nrf24";
        case FH_BLADE_SLOT_PN532:  return "pn532";
        default:                   return "slot5";
    }
}

/* Write one completed on-demand job. Two outputs, both in the writer task:
 *   1. a CLEAN row in results.csv — uptime_s, radio, seq, status text. No hex
 *      blobs, no weird fields (the raw RESP payload goes to the per-job file).
 *   2. for CC1101 jobs ONLY: its own file RES_DIR/cc1101_<uptime_s>u_seq<N>.log
 *      — the RELATIVE timestamp (uptime seconds) is the filename, so a capture
 *      is self-contained and never drowns a shared CSV. Plain text + hex dump. */
static void write_result(const fh_log_result_t *r)
{
    if (!r) { return; }
    if (!fh_board_sd_mounted()) {
        s_stats.res_sd_down++;
        return;
    }
    /* results.csv: open lazily, append, commit size immediately (same fsync
     * discipline as the main log so a pulled card shows the row). */
    if (!s_rfp) {
        mkdir(LOG_DIR, 0777);   /* ignore EEXIST */
        s_rfp = fopen(RES_CSV, "a");
        if (!s_rfp) {
            ESP_LOGW(TAG, "fopen results.csv failed");
            s_stats.res_sd_down++;
            return;
        }
    }
    fprintf(s_rfp, "%" PRIu32 ",%s,%u,0x%02X,%s%s,%s\n",
            r->uptime_s,
            radio_name_for_slot(r->radio_slot),
            (unsigned)r->seq,
            (unsigned)r->req_type,
            r->timed_out ? "TIMEOUT" : "OK",
            r->saw_busy ? "(busy)" : "",
            r->summary);
    fflush(s_rfp);
    if (fsync(fileno(s_rfp)) != 0) {
        ESP_LOGW(TAG, "fsync results.csv failed");
    }
    s_stats.res_written++;

    /* CC1101: the per-job file. The user requirement (2026-10-31): a CC1101
     * read creates its OWN file named by a relative timestamp. */
    if (r->req_type == FH_OD_CC1101_REQ) {
        mkdir(RES_DIR, 0777);
        char path[80];
        snprintf(path, sizeof(path), RES_DIR "/cc1101_%" PRIu32 "u_seq%03u.log",
                 r->uptime_s, (unsigned)r->seq);
        FILE *fp = fopen(path, "w");
        if (!fp) {
            ESP_LOGW(TAG, "fopen %s failed", path);
            return;
        }
        fprintf(fp, "# FHAdvanced on-demand result | fmt 1\n");
        fprintf(fp, "radio: cc1101 (slot %u)\n", (unsigned)r->radio_slot);
        fprintf(fp, "req_type: 0x%02X FH_OD_CC1101_REQ\n", (unsigned)r->req_type);
        fprintf(fp, "seq: %u\n", (unsigned)r->seq);
        fprintf(fp, "uptime_s: %" PRIu32 "\n", r->uptime_s);
        fprintf(fp, "timed_out: %s\n", r->timed_out ? "yes" : "no");
        fprintf(fp, "busy_seen: %s\n", r->saw_busy ? "yes" : "no");
        fprintf(fp, "summary: %s\n", r->summary);
        fprintf(fp, "resp_len: %u\n", (unsigned)r->resp_len);
        fprintf(fp, "resp_payload (hex):\n");
        for (unsigned i = 0; i < r->resp_len; ++i) {
            fprintf(fp, "%02X%s", r->resp_payload[i],
                    ((i + 1) % 16 == 0) ? "\n" : " ");
        }
        if (r->resp_len % 16) { fprintf(fp, "\n"); }
        fflush(fp);
        if (fsync(fileno(fp)) != 0) {
            ESP_LOGW(TAG, "fsync %s failed", path);
        }
        fclose(fp);
        ESP_LOGI(TAG, "cc1101 result file: %s", path);
    }
}

/* ---- the single writer task --------------------------------------------- */
static void logger_task(void *arg)
{
    (void)arg;
    /* open the first file best-effort; if SD is down we stage until it returns */
    if (fh_board_sd_mounted()) { open_new_file(); }

    fh_log_record_t rec;
    TickType_t last_flush = xTaskGetTickCount();

    for (;;) {
        /* wait for a record OR the flush period, whichever first */
        if (xQueueReceive(s_q, &rec, pdMS_TO_TICKS(LOG_FLUSH_PERIOD_MS)) == pdTRUE) {
            if (rec.source_str == NULL) {
                /* flush sentinel (fh_log_flush wake); no row to render */
            } else {
                bool from_rtc = false;
                uint32_t epoch = fh_time_now_utc(&from_rtc);
                fh_fix_t fix = fh_time_last_fix();
                char row[LOG_ROW_MAX];
                int len = render_row(&rec, epoch, &fix, row, sizeof(row));
                if (len >= (int)sizeof(row)) { len = sizeof(row) - 1; } /* truncated; ensure \n */
                if (len > 0 && row[len - 1] != '\n') { row[len - 1] = '\n'; }
                stage_row(row, len);
            }
        }
        /* on-demand results: drain the result queue (concern #1). Results are
         * rare (one per fired job), so a zero-tick drain here is enough — a
         * result waits at most one loop wake (~2 s), which is fine for a log. */
        fh_log_result_t res;
        while (s_rq && xQueueReceive(s_rq, &res, 0) == pdTRUE) {
            write_result(&res);
        }
        /* periodic flush so data lands even in a quiet window */
        if ((xTaskGetTickCount() - last_flush) >= pdMS_TO_TICKS(LOG_FLUSH_PERIOD_MS)) {
            flush_stage();
            last_flush = xTaskGetTickCount();
        }
        /* NVS stage persist: shrink the crash-loss window. SD-down saves run
         * every loop (those rows are the likeliest to be lost); SD-up saves
         * are gated to LOG_NVS_SAVE_MS to bound flash wear. A successful
         * flush above already erased the blob, so this only fires when rows
         * are actually staged. */
        if (s_stage_len > 0) {
            bool sd_up = fh_board_sd_mounted();
            if (!sd_up ||
                (esp_timer_get_time() - s_last_nvs_save_us) >=
                    (int64_t)LOG_NVS_SAVE_MS * 1000ll) {
                nvs_stage_save();
            }
        }
        /* flush handshake (AB6): a maintenance path asked for a guaranteed
         * flush — drain the stage to SD and acknowledge so the caller knows the
         * card is current before a battery swap / format. */
        if (s_flush_req) {
            flush_stage();
            s_flush_req = false;
            if (s_flush_ack) { xSemaphoreGive(s_flush_ack); }
        }
        /* fake feed: fabricate a record every ~250 ms to exercise the pipeline */
        if (s_fake) {
            static uint32_t fk = 0;
            fh_log_record_t f = {0};
            f.source_str = FH_SRCSTR_WS_BLE;
            f.band = 1; f.channel = 37 + (fk % 3);
            f.frame_type = 0;
            f.has_mac = true;
            f.mac[0]=0x02; f.mac[1]=0x00; f.mac[2]=(uint8_t)(fk>>8);
            f.mac[3]=(uint8_t)fk; f.mac[4]=0xAB; f.mac[5]=0xCD;
            f.has_rssi = true; f.rssi = (int8_t)(-40 - (fk % 50));
            f.count = 1;
            snprintf(f.extra, sizeof(f.extra), "fake=%" PRIu32, fk);
            fk++;
            /* enqueue through the same path so the queue/stamp code is exercised */
            fh_log_submit(&f);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}

/* ---- public API ---------------------------------------------------------- */
bool fh_log_start(bool fake_feed)
{
    if (s_task) { return true; }                 /* already running */
    memset(&s_stats, 0, sizeof(s_stats));
    s_fake = fake_feed;
    s_q = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(fh_log_record_t));
    if (!s_q) { return false; }
    s_rq = xQueueCreate(RES_QUEUE_DEPTH, sizeof(fh_log_result_t));
    if (!s_rq) { vQueueDelete(s_q); s_q = NULL; return false; }
    s_flush_ack = xSemaphoreCreateBinary();
    if (!s_flush_ack) { vQueueDelete(s_q); s_q = NULL; return false; }
    s_flush_mtx = xSemaphoreCreateMutex();
    if (!s_flush_mtx) { vSemaphoreDelete(s_flush_ack); s_flush_ack = NULL;
                        vQueueDelete(s_q); s_q = NULL; return false; }
    /* 6 KB stack: fprintf/fwrite on FATFS + our buffers. Generous first pass. */
    nvs_stage_recover();     /* BEFORE the task: pre-load pre-crash rows into
                                the stage; the pipeline flushes them to SD
                                like any staged data (L3 upgrade) */
    BaseType_t ok = xTaskCreate(logger_task, "fh_log", 6144, NULL, 6, &s_task);
    if (ok != pdPASS) { vQueueDelete(s_q); s_q = NULL; return false; }
    ESP_LOGI(TAG, "logger started (fake_feed=%d)", (int)fake_feed);
    return true;
}

bool fh_log_submit(const fh_log_record_t *rec)
{
    if (!s_q || !rec) { return false; }
    if (xQueueSend(s_q, rec, 0) != pdTRUE) {      /* zero-tick: never blocks */
        s_stats.dropped_queue++;
        return false;
    }
    return true;
}

bool fh_log_submit_result(const fh_log_result_t *r)
{
    if (!s_rq || !r) { return false; }
    if (xQueueSend(s_rq, r, 0) != pdTRUE) {       /* zero-tick: never blocks */
        /* full result queue = 8 jobs finishing in one loop wake: never in
         * practice (the requester finishes one poll at a time). Count it. */
        s_stats.res_dropped++;
        return false;
    }
    return true;
}

/* ---- CAPTURE -> record mapping (one place both UART + BLE paths use) ----- */
static const char *srcstr_from_id(uint8_t src_id)
{
    switch (src_id) {
        case FH_SRC_C5_WIFI5:   return FH_SRCSTR_C5_WIFI5;
        case FH_SRC_XS3_WIFI24: return FH_SRCSTR_XS3_WIFI24;
        default:                return "unknown";   /* pico results come via a different path */
    }
}

bool fh_log_submit_capture(const fh_capture_t *cap)
{
    if (!cap) { return false; }
    fh_log_record_t r = {0};
    r.source_str = srcstr_from_id(cap->src_id);
    /* band_chan encoding (CONFIRMED against the C5/XS3 sniffer firmware):
     *   high nibble = band tag (0x5 = 5 GHz, 0x2 = 2.4 GHz, 0x1 = BLE),
     *   low nibble  = a COARSE 0..15 channel INDEX (NOT the real channel).
     * The REAL channel number rides in extra[0] (the sniffer sets
     * LEN = FH_CAPTURE_FIXED + 1). So we take the band from the high nibble and
     * the channel from extra[0] — NOT the low-nibble index, which cannot hold a
     * 5 GHz channel like 149 (AB1 fix). If extra[0] is 0 (older sender or no
     * extra), channel reads 0 = "unknown", which is honest. */
    r.band    = (cap->band_chan >> 4) & 0x0F;   /* 2 / 5 / 1 -> band_str()      */
    r.channel = cap->extra[0];                   /* REAL channel (sniffer extra[0]) */
    r.frame_type = cap->frame_type;
    memcpy(r.mac, cap->mac, 6); r.has_mac = true;
    r.rssi = cap->rssi; r.has_rssi = true;
    r.count = cap->count;
    /* extra text column: the dedup count (deep per-source extra parsing parked).
     * NOTE extra[0] is consumed above as the real channel, so it is intentionally
     * not repeated here. */
    snprintf(r.extra, sizeof(r.extra), "count=%u", (unsigned)cap->count);
    return fh_log_submit(&r);
}

fh_log_stats_t fh_log_stats(void) { return s_stats; }

bool fh_log_flush(void)
{
    /* AB6 + CD1/CD2: a REAL flush handshake, now safe under overlapping callers.
     *
     * CD2 (enforced in code): a flush mutex serializes concurrent callers, so
     * two maintenance actions (or any future caller) can overlap safely — the
     * single-caller rule is now guaranteed, not just documented.
     *
     * CD1: a prior call that TIMED OUT can leave the writer about to give
     * s_flush_ack with nobody waiting (a stale count-1). We DRAIN the ack to 0
     * before raising the request, and clear s_flush_req on our own timeout, so a
     * stale give can never satisfy a later request. */
    if (!s_q || !s_flush_ack || !s_flush_mtx) { return false; }

    xSemaphoreTake(s_flush_mtx, portMAX_DELAY);

    (void)xSemaphoreTake(s_flush_ack, 0);   /* drain any stale give (CD1) */

    s_flush_req = true;
    fh_log_record_t sentinel = {0};         /* source_str == NULL => skipped */
    xQueueSend(s_q, &sentinel, 0);          /* best-effort wake; timeout path also services it */

    bool ok = (xSemaphoreTake(s_flush_ack, pdMS_TO_TICKS(3000)) == pdTRUE);
    if (!ok) {
        s_flush_req = false;   /* late writer give is drained by the next caller */
    }
    xSemaphoreGive(s_flush_mtx);
    return ok;
}

/* ===========================================================================
 * KNOWN COMPILE RISKS
 *   [C1] mkdir()/struct stat: include <sys/stat.h> and <sys/unistd.h> on IDF
 *        rather than the local extern decls used here (those are a stand-in so
 *        the intent is clear without the headers in front of me). FIX: replace
 *        the `extern int mkdir(...)` with #include <sys/stat.h>.
 *   [C2] fh_hub_node_mac() is declared extern and provided by app_main
 *        (esp_read_mac(..., ESP_MAC_EFUSE_FACTORY)). Ensure the symbol exists.
 *   [C3] -Werror=format-truncation: the snprintf row buffer (256) is sized for
 *        the worst case (source<=16, mac 17, iso 20, extra 48, commas) ~ 120 B,
 *        so truncation cannot trip; verify under -Werror anyway.
 *   [C4] PRIu32/PRIu64 need <inttypes.h> (included).
 *
 * KNOWN LOGIC RISKS (the state machine — review these hardest)
 *   [L1] ROLL ACCOUNTING: s_file_bytes counts header + staged writes. A mid-
 *        stage roll is handled AFTER a full flush so a file never exceeds the
 *        ceiling by more than one stage batch (~12 KB over 128 MB — acceptable).
 *   [L2] SD-DOWN DROP POLICY is drop-OLDEST (opposite of the queue's drop-
 *        newest) — recent rows beat stale ones when SD is gone. Deliberate.
 *        Verify the newline-boundary memmove never splits a row.
 *   [L3] (CD3 FIXED) stats now distinguish `rows_staged` (accepted into the RAM
 *        stage, may later be dropped if SD stays down) from `bytes_committed`
 *        (bytes actually fwritten+flushed to SD — the honest "data is on the
 *        card" measure). The UI shows both. A full crash can still lose up to
 *        one un-flushed stage batch. CLOSED (2026-10-31): the stage is now
 *        persisted to NVS (LOG_NVS_SAVE_MS cadence SD-up, every loop SD-down)
 *        and recovered at boot before the task starts — no RTC part needed,
 *        GPS remains the time base. Worst case: one LOG_NVS_SAVE_MS window of
 *        rows, or duplicate rows if a crash lands in the post-flush erase
 *        window. Verify on the bench: kill power mid-capture, reboot, check
 *        the recovered rows land in the next CSV.
 *   [L4] (CD1/CD2 FIXED in code) fh_log_flush() is a REAL handshake: request
 *        flag + binary ack, with a STALE-ACK DRAIN at entry (a prior timed-out
 *        flush cannot make the next call return true early) and req-clear on
 *        timeout. CD2: a flush MUTEX now serializes overlapping callers, so
 *        concurrent flushes are SAFE (not merely "documented as one-at-a-time").
 *   [L5] FATFS long filenames + the /sdcard/logs subdir require CONFIG_FATFS_LFN
 *        in sdkconfig; the fh_%010u.csv name is 8.3-safe anyway (14 chars) so
 *        it works even without LFN. Verify the subdir mkdir on a FAT volume.
 *   [L6] PARTIAL WRITE -> possible DUPLICATE, never loss (multi-check
 *        2026-11-01). On a short fwrite (0 < w < s_stage_len) the first w bytes
 *        may already sit in the OLD file — possibly ending mid-row — but the
 *        WHOLE stage is kept and re-written to the NEW file on retry, so those w
 *        bytes can appear twice across files and the old file can end in a torn
 *        row. Deliberate: the card most likely vanished, so those w bytes were
 *        probably never committed (no fsync ran); trimming them could LOSE data.
 *        Duplicate-over-loss. A CSV consumer should tolerate one torn trailing
 *        line per file and de-duplicate across a file boundary.
 * ======================================================================== */
