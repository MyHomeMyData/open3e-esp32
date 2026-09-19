/* KNXnet/IP frames, without a socket in sight.
 *
 * Byte layouts from the KNX standard, volume 3/8/2 (Core) and 3/8/4
 * (Tunnelling), plus the cEMI message format in 3/6/3. Kept separate from
 * knx.c so the bytes can be checked against the specification on a
 * workstation -- a telegram with a wrong length octet is discarded by the
 * gateway without a word, so there is nothing to see on a device.
 */
#include "knx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every KNXnet/IP frame starts with these six bytes: header length, protocol
 * version, service type, total length including the header itself. */
#define HDR_LEN     6
#define HDR_VERSION 0x10

static size_t put_header(uint8_t *out, uint16_t service, uint16_t total)
{
    out[0] = HDR_LEN;
    out[1] = HDR_VERSION;
    out[2] = (uint8_t)(service >> 8);
    out[3] = (uint8_t)(service & 0xFF);
    out[4] = (uint8_t)(total >> 8);
    out[5] = (uint8_t)(total & 0xFF);
    return HDR_LEN;
}

/* HPAI -- where the gateway should send its answers. Always IPv4 over UDP
 * here; the protocol code would be 0x02 for TCP, which only KNX IP Secure
 * uses. */
#define HPAI_LEN 8

static size_t put_hpai(uint8_t *out, uint32_t ip, uint16_t port)
{
    out[0] = HPAI_LEN;
    out[1] = 0x01;                      /* IPv4 UDP */
    out[2] = (uint8_t)(ip >> 24);
    out[3] = (uint8_t)(ip >> 16);
    out[4] = (uint8_t)(ip >> 8);
    out[5] = (uint8_t)(ip);
    out[6] = (uint8_t)(port >> 8);
    out[7] = (uint8_t)(port & 0xFF);
    return HPAI_LEN;
}

/* ------------------------------------------------------------------ */
/* addresses                                                            */

/* Both address kinds are sixteen bits with different field widths, and both
 * are written by people, so both are parsed strictly: a typo that silently
 * becomes a different address is worse than a refusal. */
static bool parse_parts(const char *s, char sep, int n_parts,
                        const long *limits, uint16_t *out_parts)
{
    if (!s || !*s) {
        return false;
    }
    const char *p = s;
    for (int i = 0; i < n_parts; i++) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v > limits[i]) {
            return false;
        }
        out_parts[i] = (uint16_t)v;
        p = end;
        if (i < n_parts - 1) {
            if (*p != sep) {
                return false;
            }
            p++;
        }
    }
    while (*p == ' ') {
        p++;
    }
    return *p == '\0';
}

bool knx_ga_parse(const char *s, uint16_t *out)
{
    if (!s || !out) {
        return false;
    }
    /* Count separators first: three-level "1/2/3", two-level "1/2", or the
     * raw number the ETS also accepts. */
    int slashes = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '/') {
            slashes++;
        }
    }
    uint16_t v[3];
    if (slashes == 2) {
        static const long lim[3] = { 31, 7, 255 };
        if (!parse_parts(s, '/', 3, lim, v)) {
            return false;
        }
        *out = (uint16_t)((v[0] << 11) | (v[1] << 8) | v[2]);
        return true;
    }
    if (slashes == 1) {
        static const long lim[2] = { 31, 2047 };
        if (!parse_parts(s, '/', 2, lim, v)) {
            return false;
        }
        *out = (uint16_t)((v[0] << 11) | v[1]);
        return true;
    }
    if (slashes == 0) {
        static const long lim[1] = { 65535 };
        if (!parse_parts(s, '/', 1, lim, v)) {
            return false;
        }
        /* Group address 0 is the broadcast address, never a target. */
        if (v[0] == 0) {
            return false;
        }
        *out = v[0];
        return true;
    }
    return false;
}

void knx_ga_format(uint16_t ga, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%u/%u/%u", (ga >> 11) & 0x1F, (ga >> 8) & 0x07, ga & 0xFF);
}

bool knx_pa_parse(const char *s, uint16_t *out)
{
    static const long lim[3] = { 15, 15, 255 };
    uint16_t v[3];
    if (!s || !out || !parse_parts(s, '.', 3, lim, v)) {
        return false;
    }
    *out = (uint16_t)((v[0] << 12) | (v[1] << 8) | v[2]);
    return true;
}

void knx_pa_format(uint16_t pa, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%u.%u.%u", (pa >> 12) & 0x0F, (pa >> 8) & 0x0F, pa & 0xFF);
}

/* ------------------------------------------------------------------ */
/* cEMI                                                                 */

size_t knx_cemi_write_bool(uint8_t *out, size_t out_sz, uint16_t src,
                           uint16_t ga, bool on)
{
    if (out_sz < 11) {
        return 0;
    }
    out[0] = 0x11;   /* L_Data.req */
    out[1] = 0x00;   /* no additional information */
    /* 0xBC: standard frame, do not repeat, system broadcast, low priority.
     * Low priority is deliberate -- a doorbell is not more important than the
     * installation's own traffic, and anything higher would be rude on a bus
     * this module is only a guest on. */
    out[2] = 0xBC;
    /* 0xE0: the destination is a group address, hop count 6, standard frame
     * format. Without the top bit the gateway would read the destination as a
     * physical address and deliver it nowhere. */
    out[3] = 0xE0;
    out[4] = (uint8_t)(src >> 8);
    out[5] = (uint8_t)(src & 0xFF);
    out[6] = (uint8_t)(ga >> 8);
    out[7] = (uint8_t)(ga & 0xFF);
    /* One octet after the TPCI byte. A one-bit value rides inside the APCI
     * byte itself rather than following it, which is why this is 1 and not 2
     * -- the single most common mistake in a hand-written KNX telegram. */
    out[8] = 0x01;
    out[9] = 0x00;                                  /* TPCI: numbered data */
    out[10] = (uint8_t)(0x80 | (on ? 0x01 : 0x00)); /* A_GroupValue_Write */
    return 11;
}

/* ------------------------------------------------------------------ */
/* frames                                                               */

size_t knx_frame_tunnel(uint8_t *out, size_t out_sz, uint8_t channel,
                        uint8_t seq, const uint8_t *cemi, size_t cemi_len)
{
    size_t total = HDR_LEN + 4 + cemi_len;
    if (out_sz < total || !cemi || !cemi_len) {
        return 0;
    }
    size_t o = put_header(out, KNX_TUNNEL_REQ, (uint16_t)total);
    out[o++] = 0x04;      /* connection header length */
    out[o++] = channel;
    out[o++] = seq;
    out[o++] = 0x00;      /* reserved */
    memcpy(out + o, cemi, cemi_len);
    return total;
}

size_t knx_frame_routing(uint8_t *out, size_t out_sz,
                         const uint8_t *cemi, size_t cemi_len)
{
    size_t total = HDR_LEN + cemi_len;
    if (out_sz < total || !cemi || !cemi_len) {
        return 0;
    }
    size_t o = put_header(out, KNX_ROUTING_IND, (uint16_t)total);
    memcpy(out + o, cemi, cemi_len);
    return total;
}

size_t knx_frame_connect(uint8_t *out, size_t out_sz, uint32_t ip, uint16_t port)
{
    size_t total = HDR_LEN + HPAI_LEN + HPAI_LEN + 4;
    if (out_sz < total) {
        return 0;
    }
    size_t o = put_header(out, KNX_CONNECT_REQ, (uint16_t)total);
    /* Control endpoint, then data endpoint. The same socket serves both: one
     * port means one thing to keep open and one thing to firewall. */
    o += put_hpai(out + o, ip, port);
    o += put_hpai(out + o, ip, port);
    out[o++] = 0x04;      /* CRI length */
    out[o++] = 0x04;      /* TUNNEL_CONNECTION */
    out[o++] = 0x02;      /* TUNNEL_LINKLAYER -- raw link layer, not busmonitor */
    out[o++] = 0x00;      /* reserved */
    return total;
}

static size_t frame_channel_hpai(uint8_t *out, size_t out_sz, uint16_t service,
                                 uint8_t channel, uint32_t ip, uint16_t port)
{
    size_t total = HDR_LEN + 2 + HPAI_LEN;
    if (out_sz < total) {
        return 0;
    }
    size_t o = put_header(out, service, (uint16_t)total);
    out[o++] = channel;
    out[o++] = 0x00;      /* reserved */
    put_hpai(out + o, ip, port);
    return total;
}

size_t knx_frame_connstate(uint8_t *out, size_t out_sz, uint8_t channel,
                           uint32_t ip, uint16_t port)
{
    return frame_channel_hpai(out, out_sz, KNX_CONNSTATE_REQ, channel, ip, port);
}

size_t knx_frame_disconnect(uint8_t *out, size_t out_sz, uint8_t channel,
                            uint32_t ip, uint16_t port)
{
    return frame_channel_hpai(out, out_sz, KNX_DISCONNECT_REQ, channel, ip, port);
}

size_t knx_frame_ack(uint8_t *out, size_t out_sz, uint8_t channel, uint8_t seq)
{
    size_t total = HDR_LEN + 4;
    if (out_sz < total) {
        return 0;
    }
    size_t o = put_header(out, KNX_TUNNEL_ACK, (uint16_t)total);
    out[o++] = 0x04;
    out[o++] = channel;
    out[o++] = seq;
    out[o++] = 0x00;      /* E_NO_ERROR */
    return total;
}

/* ------------------------------------------------------------------ */
/* parsing                                                              */

bool knx_frame_peek(const uint8_t *buf, size_t len, uint16_t *service,
                    uint16_t *total)
{
    if (!buf || len < HDR_LEN || buf[0] != HDR_LEN || buf[1] != HDR_VERSION) {
        return false;
    }
    uint16_t t = (uint16_t)((buf[4] << 8) | buf[5]);
    /* The length the frame claims has to match what actually arrived. A
     * shorter datagram would otherwise let every parser below read past the
     * end of the buffer. */
    if (t < HDR_LEN || t > len) {
        return false;
    }
    if (service) {
        *service = (uint16_t)((buf[2] << 8) | buf[3]);
    }
    if (total) {
        *total = t;
    }
    return true;
}

bool knx_parse_connect_res(const uint8_t *buf, size_t len, uint8_t *channel,
                           uint8_t *status, uint16_t *assigned)
{
    uint16_t service, total;
    if (!knx_frame_peek(buf, len, &service, &total) || service != KNX_CONNECT_RES) {
        return false;
    }
    if (total < HDR_LEN + 2) {
        return false;
    }
    *channel = buf[HDR_LEN];
    *status  = buf[HDR_LEN + 1];
    *assigned = 0;
    /* A refusal carries no endpoint and no CRD, so stop here rather than
     * reading fields the gateway never sent. */
    if (*status != 0x00) {
        return true;
    }
    /* header + channel + status + data HPAI + CRD(4), and the assigned
     * physical address sits in the last two bytes of the CRD. */
    size_t crd = HDR_LEN + 2 + HPAI_LEN;
    if (total < crd + 4) {
        return false;
    }
    *assigned = (uint16_t)((buf[crd + 2] << 8) | buf[crd + 3]);
    return true;
}

bool knx_parse_conn_header(const uint8_t *buf, size_t len, uint8_t *channel,
                           uint8_t *seq, uint8_t *status)
{
    uint16_t total;
    if (!knx_frame_peek(buf, len, NULL, &total) || total < HDR_LEN + 4) {
        return false;
    }
    if (buf[HDR_LEN] != 0x04) {
        return false;
    }
    if (channel) { *channel = buf[HDR_LEN + 1]; }
    if (seq)     { *seq     = buf[HDR_LEN + 2]; }
    if (status)  { *status  = buf[HDR_LEN + 3]; }
    return true;
}

const char *knx_status_text(uint8_t status)
{
    switch (status) {
    case 0x00: return "ok";
    case 0x21: return "the gateway does not know this connection";
    case 0x22: return "the gateway has no free tunnel channel";
    case 0x23: return "the gateway refused the tunnelling layer";
    case 0x24: return "the gateway is out of resources";
    case 0x26: return "the connection was closed by the gateway";
    case 0x29: return "the gateway does not support this connection type";
    case 0x2A: return "the gateway does not support this option";
    default:   return "the gateway refused the connection";
    }
}
