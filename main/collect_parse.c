#include "collect_parse.h"

#include <string.h>

/* Complete the message in progress, if it is complete. */
static bool finish(collect_asm_t *st, collect_msg_t *out, uint32_t *abandoned)
{
    if (!st->active) {
        return false;
    }
    st->active = false;
    if (st->have < st->need) {
        if (abandoned) {
            (*abandoned)++;
        }
        return false;
    }
    out->did = st->did;
    out->len = st->need;
    memcpy(out->data, st->buf, st->need);
    return true;
}

/* Begin a segmented message whose payload starts at `off` in this frame. */
static bool begin(collect_asm_t *st, const uint8_t *d, uint8_t dlc,
                  uint16_t did, uint16_t len, uint8_t off,
                  collect_msg_t *out, uint32_t *abandoned)
{
    st->active = true;
    st->did = did;
    st->need = len;
    st->expect = (uint8_t)(d[0] + 1);
    st->have = (dlc > off) ? (uint16_t)(dlc - off) : 0;
    if (st->have > len) {
        st->have = len;
    }
    if (st->have) {
        memcpy(st->buf, d + off, st->have);
    }
    return (st->have >= st->need) ? finish(st, out, abandoned) : false;
}

bool collect_feed(collect_asm_t *st, const uint8_t *d, uint8_t dlc,
                  collect_msg_t *out, uint32_t *abandoned)
{
    if (dlc < 1) {
        return false;
    }

    /* Continuation of a segmented message. */
    if (st->active) {
        if (d[0] == st->expect) {
            uint16_t take = (uint16_t)(dlc - 1);
            if (st->have + take > COLLECT_MAX_LEN) {
                take = (uint16_t)(COLLECT_MAX_LEN - st->have);
            }
            memcpy(st->buf + st->have, d + 1, take);
            st->have += take;
            st->expect = (st->expect >= 0x2F) ? 0x20 : (uint8_t)(st->expect + 1);
            return (st->have >= st->need) ? finish(st, out, abandoned) : false;
        }
        /* Anything else ends the message; a short one is simply lost. */
        finish(st, out, abandoned);
    }

    /* Start of a message: 0x21, identifier, then the type byte. */
    if (dlc <= 4 || d[0] != 0x21 || d[3] < 0xB0 || d[3] >= 0xC0) {
        return false;
    }
    uint16_t did = (uint16_t)(d[1] | (d[2] << 8));
    if (did == 0 || did >= 10000) {
        return false;
    }
    uint8_t type = d[3];

    if (type >= 0xB1 && type <= 0xB4) {
        /* One to four payload bytes, entirely in this frame. */
        uint16_t len = (uint16_t)(type - 0xB0);
        if (4 + len > dlc) {
            return false;
        }
        out->did = did;
        out->len = len;
        memcpy(out->data, d + 4, len);
        return true;
    }

    if (type == 0xB0) {
        /* The length follows, one byte later still if that byte is 0xC1. */
        uint16_t len;
        uint8_t off;
        if (d[4] == 0xC1) {
            if (dlc < 6) {
                return false;
            }
            len = d[5];
            off = 6;
        } else {
            len = d[4];
            off = 5;
        }
        if (len == 0 || len > COLLECT_MAX_LEN) {
            return false;
        }
        return begin(st, d, dlc, did, len, off, out, abandoned);
    }

    /* 0xB5..0xBF: the low nibble is the length, five to fifteen bytes. Only
     * four of them fit beside the header, so the rest arrives in the next
     * frame -- segmented, but without the 0xB0 length byte. */
    return begin(st, d, dlc, did, (uint16_t)(type - 0xB0), 4, out, abandoned);
}
