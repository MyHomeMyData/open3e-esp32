#include "isotp.h"

#include <string.h>

/* Protocol Control Information, high nibble of byte 0. */
#define PCI_SF 0x00
#define PCI_FF 0x10
#define PCI_CF 0x20
#define PCI_FC 0x30

/* Flow status, low nibble of a flow control frame. */
#define FS_CTS   0x00
#define FS_WAIT  0x01
#define FS_OVFLW 0x02

const char *isotp_strerror(isotp_err_t e)
{
    switch (e) {
    case ISOTP_OK:           return "ok";
    case ISOTP_ERR_TIMEOUT:  return "timeout";
    case ISOTP_ERR_TOO_LONG: return "message too long";
    case ISOTP_ERR_BUS:      return "CAN transmit failed";
    case ISOTP_ERR_PROTOCOL: return "protocol violation";
    case ISOTP_ERR_OVERFLOW: return "peer buffer overflow";
    }
    return "unknown";
}

/* Every frame goes out padded to 8 bytes; open3e sets tx_padding=0 and E3
 * ECUs expect full-length frames. */
static bool send_padded(const isotp_link_t *l, const uint8_t *data, uint8_t n)
{
    uint8_t frame[8];
    memcpy(frame, data, n);
    memset(frame + n, ISOTP_PAD_BYTE, sizeof(frame) - n);
    return l->hal->send(l->hal->ctx, l->tx_id, frame, sizeof(frame));
}

/* STmin is either 0x00..0x7F milliseconds or 0xF1..0xF9 hundreds of
 * microseconds; anything else is reserved and ISO 15765-2 says to treat it as
 * the 127 ms maximum. */
uint32_t isotp_stmin_to_us(uint8_t stmin)
{
    if (stmin <= 0x7F) {
        return (uint32_t)stmin * 1000u;
    }
    if (stmin >= 0xF1 && stmin <= 0xF9) {
        return (uint32_t)(stmin - 0xF0) * 100u;
    }
    return 127000u;
}

/* Wait for a flow control frame from our peer, tolerating FS_WAIT. */
static isotp_err_t await_fc(const isotp_link_t *l, uint8_t *bs, uint32_t *stmin_us)
{
    uint32_t deadline = l->hal->now_ms(l->hal->ctx) + ISOTP_N_BS_MS;
    uint32_t hard_deadline = l->hal->now_ms(l->hal->ctx) + ISOTP_MSG_MS;
    uint8_t waits = 0;
    for (;;) {
        if (l->hal->now_ms(l->hal->ctx) >= hard_deadline) {
            return ISOTP_ERR_TIMEOUT;
        }
        uint32_t now = l->hal->now_ms(l->hal->ctx);
        if (now >= deadline) {
            return ISOTP_ERR_TIMEOUT;
        }
        uint32_t id;
        uint8_t data[8], len;
        if (!l->hal->recv(l->hal->ctx, &id, data, &len, deadline - now)) {
            return ISOTP_ERR_TIMEOUT;
        }
        if (id != l->rx_id || len < 3 || (data[0] & 0xF0) != PCI_FC) {
            continue;   /* not for us, or not flow control */
        }
        switch (data[0] & 0x0F) {
        case FS_CTS:
            *bs = data[1];
            *stmin_us = isotp_stmin_to_us(data[2]);
            return ISOTP_OK;
        case FS_WAIT:
            /* Peer asked for more time. Granting that without limit is how a
             * peer holds the bus forever, so it is granted a few times. */
            if (++waits > ISOTP_N_WFT_MAX) {
                return ISOTP_ERR_TIMEOUT;
            }
            deadline = l->hal->now_ms(l->hal->ctx) + ISOTP_N_BS_MS;
            continue;
        case FS_OVFLW:
            return ISOTP_ERR_OVERFLOW;
        default:
            return ISOTP_ERR_PROTOCOL;
        }
    }
}

isotp_err_t isotp_send(const isotp_link_t *l, const uint8_t *data, size_t len)
{
    if (len > ISOTP_MAX_PAYLOAD) {
        return ISOTP_ERR_TOO_LONG;
    }

    if (len <= 7) {
        uint8_t f[8];
        f[0] = PCI_SF | (uint8_t)len;
        memcpy(f + 1, data, len);
        return send_padded(l, f, (uint8_t)(1 + len)) ? ISOTP_OK : ISOTP_ERR_BUS;
    }

    /* First frame carries the 12-bit total length and the first 6 bytes. */
    uint8_t ff[8];
    ff[0] = PCI_FF | (uint8_t)((len >> 8) & 0x0F);
    ff[1] = (uint8_t)(len & 0xFF);
    memcpy(ff + 2, data, 6);
    if (!l->hal->send(l->hal->ctx, l->tx_id, ff, sizeof(ff))) {
        return ISOTP_ERR_BUS;
    }

    uint8_t bs = 0;
    uint32_t stmin_us = 0;
    isotp_err_t err = await_fc(l, &bs, &stmin_us);
    if (err != ISOTP_OK) {
        return err;
    }

    size_t off = 6;
    uint8_t sn = 1;
    uint8_t in_block = 0;
    uint32_t send_deadline = l->hal->now_ms(l->hal->ctx) + ISOTP_MSG_MS;
    while (off < len) {
        if (l->hal->now_ms(l->hal->ctx) >= send_deadline) {
            return ISOTP_ERR_TIMEOUT;
        }
        if (stmin_us) {
            l->hal->delay_us(l->hal->ctx, stmin_us);
        }

        size_t chunk = len - off;
        if (chunk > 7) {
            chunk = 7;
        }
        uint8_t cf[8];
        cf[0] = PCI_CF | (sn & 0x0F);
        memcpy(cf + 1, data + off, chunk);
        if (!send_padded(l, cf, (uint8_t)(1 + chunk))) {
            return ISOTP_ERR_BUS;
        }
        off += chunk;
        sn = (uint8_t)((sn + 1) & 0x0F);

        /* BlockSize 0 means "no further flow control expected"; otherwise the
         * peer gates us again after every bs frames. */
        if (bs && ++in_block == bs && off < len) {
            in_block = 0;
            err = await_fc(l, &bs, &stmin_us);
            if (err != ISOTP_OK) {
                return err;
            }
        }
    }
    return ISOTP_OK;
}

isotp_err_t isotp_recv(const isotp_link_t *l, uint8_t *buf, size_t buf_sz,
                       size_t *out_len, uint32_t timeout_ms)
{
    uint32_t id;
    uint8_t data[8], len;

    /* Wait for the opening frame, ignoring traffic addressed elsewhere. */
    uint32_t deadline = l->hal->now_ms(l->hal->ctx) + timeout_ms;
    for (;;) {
        uint32_t now = l->hal->now_ms(l->hal->ctx);
        if (now >= deadline) {
            return ISOTP_ERR_TIMEOUT;
        }
        if (!l->hal->recv(l->hal->ctx, &id, data, &len, deadline - now)) {
            return ISOTP_ERR_TIMEOUT;
        }
        if (id == l->rx_id && len >= 1) {
            uint8_t pci = data[0] & 0xF0;
            if (pci == PCI_SF || pci == PCI_FF) {
                break;
            }
        }
    }

    if ((data[0] & 0xF0) == PCI_SF) {
        size_t n = data[0] & 0x0F;
        if (n > buf_sz || n + 1 > len) {
            return ISOTP_ERR_PROTOCOL;
        }
        memcpy(buf, data + 1, n);
        *out_len = n;
        return ISOTP_OK;
    }

    /* First frame: total length, then 6 payload bytes. */
    size_t total = (size_t)(data[0] & 0x0F) << 8 | data[1];
    if (total > buf_sz) {
        /* Tell the sender we cannot take it rather than silently truncating. */
        uint8_t fc[3] = { PCI_FC | FS_OVFLW, 0, 0 };
        send_padded(l, fc, sizeof(fc));
        return ISOTP_ERR_TOO_LONG;
    }
    if (len < 8) {
        return ISOTP_ERR_PROTOCOL;
    }
    memcpy(buf, data + 2, 6);
    size_t off = 6;

    /* Clear to send: no block gating, and the link's STmin between frames. */
    uint8_t fc[3] = { PCI_FC | FS_CTS, ISOTP_BLOCKSIZE, l->stmin_ms };
    if (!send_padded(l, fc, sizeof(fc))) {
        return ISOTP_ERR_BUS;
    }

    uint8_t expect_sn = 1;
    /* A ceiling for the whole message, not just for each frame: foreign
     * traffic keeps the per-frame timeout from ever expiring. */
    uint32_t msg_deadline = l->hal->now_ms(l->hal->ctx) + ISOTP_MSG_MS;
    while (off < total) {
        if (l->hal->now_ms(l->hal->ctx) >= msg_deadline) {
            return ISOTP_ERR_TIMEOUT;
        }
        if (!l->hal->recv(l->hal->ctx, &id, data, &len, ISOTP_N_CR_MS)) {
            return ISOTP_ERR_TIMEOUT;
        }
        if (id != l->rx_id) {
            continue;   /* another node's traffic; the deadline above bounds it */
        }
        if ((data[0] & 0xF0) != PCI_CF) {
            return ISOTP_ERR_PROTOCOL;
        }
        if ((data[0] & 0x0F) != expect_sn) {
            return ISOTP_ERR_PROTOCOL;   /* a frame was lost or reordered */
        }
        expect_sn = (uint8_t)((expect_sn + 1) & 0x0F);

        size_t chunk = total - off;
        if (chunk > 7) {
            chunk = 7;
        }
        if (chunk + 1 > len) {
            return ISOTP_ERR_PROTOCOL;
        }
        memcpy(buf + off, data + 1, chunk);
        off += chunk;
    }

    *out_len = total;
    return ISOTP_OK;
}
