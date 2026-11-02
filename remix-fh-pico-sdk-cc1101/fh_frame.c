/*
 * fh_frame.c — FHAdvanced shared UART wire protocol implementation.
 * Plain C, no ESP-IDF/Arduino dependencies. See fh_frame.h for the contract.
 *
 * NOTE: unmodified copy of the repo's components/fh_protocol/fh_frame.c,
 * included here so this C5 project is self-contained for a build. The
 * authoritative copy lives in the repo; keep them identical.
 */
#include "fh_frame.h"
#include <string.h>

/* ----------------------------------------------------------------- CRC16 */
uint16_t fh_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = FH_CRC16_INIT;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ FH_CRC16_POLY);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* ----------------------------------------------------------------- encode */
size_t fh_frame_encode(fh_type_t type, const uint8_t *payload,
                       uint8_t payload_len, uint8_t *out, size_t out_cap)
{
    if (out == NULL) { return 0; }
    /* payload_len is uint8_t, so it can never exceed FH_MAX_PAYLOAD (255);
     * the old range check was always false and -Werror=type-limits rejects it. */
    if (payload == NULL && payload_len != 0) { return 0; }

    const size_t total = (size_t)FH_HDR_LEN + payload_len + FH_CRC_LEN;
    if (out_cap < total) { return 0; }

    out[0] = FH_SYNC0;
    out[1] = FH_SYNC1;
    out[2] = (uint8_t)type;
    out[3] = payload_len;
    if (payload_len) {
        memcpy(&out[4], payload, payload_len);
    }
    /* CRC covers TYPE..end-of-PAYLOAD (bytes [2 .. 4+payload_len), i.e. NOT
     * the two SYNC bytes). */
    const uint16_t crc = fh_crc16(&out[2], (size_t)(2u + payload_len));
    /* little-endian trailer */
    out[4 + payload_len]     = (uint8_t)(crc & 0xFFu);
    out[4 + payload_len + 1] = (uint8_t)((crc >> 8) & 0xFFu);
    return total;
}

/* ---------------------------------------------------- streaming decoder */
/* Internal parse states. */
enum {
    ST_SYNC0 = 0,  /* waiting for 0xA5 */
    ST_SYNC1,      /* saw 0xA5, waiting for 0x5A */
    ST_TYPE,       /* read TYPE */
    ST_LEN,        /* read LEN */
    ST_PAYLOAD,    /* collecting LEN payload bytes */
    ST_CRC0,       /* first CRC byte */
    ST_CRC1,       /* second CRC byte -> validate & deliver */
};

void fh_decoder_init(fh_decoder_t *d)
{
    if (!d) { return; }
    memset(d, 0, sizeof(*d));
    d->state = ST_SYNC0;
}

/* buf layout while assembling: [0]=TYPE [1]=LEN [2..2+LEN) payload
 * [2+LEN]=CRClo [3+LEN]=CRChi. We keep TYPE..payload contiguous so the CRC can
 * be computed over a single contiguous span. */
void fh_decoder_push(fh_decoder_t *d, const uint8_t *bytes, size_t n,
                     fh_frame_cb_t cb, void *user)
{
    if (!d || !bytes) { return; }
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = bytes[i];
        switch (d->state) {
        case ST_SYNC0:
            if (c == FH_SYNC0) { d->state = ST_SYNC1; }
            /* else: stay, scanning for the anchor */
            break;
        case ST_SYNC1:
            if (c == FH_SYNC1) {
                d->have = 0; d->want = 0; d->state = ST_TYPE;
            } else if (c == FH_SYNC0) {
                /* 0xA5 0xA5 … stay primed for the next byte */
                d->state = ST_SYNC1;
            } else {
                d->state = ST_SYNC0;
            }
            break;
        case ST_TYPE:
            d->buf[0] = c;  /* TYPE */
            d->have = 1;
            d->state = ST_LEN;
            break;
        case ST_LEN:
            d->buf[1] = c;  /* LEN */
            d->have = 2;
            d->want = c;
            d->state = (c == 0) ? ST_CRC0 : ST_PAYLOAD;
            break;
        case ST_PAYLOAD:
            d->buf[d->have++] = c;
            /* payload complete when we have TYPE+LEN(2) + want bytes */
            if (d->have >= (uint16_t)(2u + d->want)) {
                d->state = ST_CRC0;
            }
            break;
        case ST_CRC0:
            d->buf[d->have++] = c;  /* CRC low */
            d->state = ST_CRC1;
            break;
        case ST_CRC1: {
            d->buf[d->have++] = c;  /* CRC high */
            /* Validate: CRC covers TYPE..end-of-PAYLOAD = buf[0 .. 2+want). */
            const uint16_t calc = fh_crc16(d->buf, (size_t)(2u + d->want));
            const uint16_t got  = (uint16_t)d->buf[2 + d->want] |
                                  (uint16_t)(d->buf[3 + d->want] << 8);
            if (calc == got) {
                d->good_frames++;
                if (cb) {
                    const fh_type_t type = (fh_type_t)d->buf[0];
                    const uint8_t   len  = d->buf[1];
                    const uint8_t  *pl   = (len ? &d->buf[2] : NULL);
                    cb(type, pl, len, user);
                }
            } else {
                d->crc_errors++;
                d->resyncs++;
            }
            d->state = ST_SYNC0;  /* done — hunt for the next frame */
            break;
        }
        default:
            d->state = ST_SYNC0;
            break;
        }
    }
}
