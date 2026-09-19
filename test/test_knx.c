/* KNXnet/IP frames against the specification, on a workstation.
 *
 * This is the kind of code whose failures are invisible on the device: a
 * telegram with a wrong length octet or a swapped address is dropped by the
 * gateway without an error, without a log entry and without anything
 * appearing on the bus. There is nothing to observe and nothing to bisect --
 * so the bytes are checked here instead, against the layouts in KNX volumes
 * 3/8/2 (Core), 3/8/4 (Tunnelling) and 3/6/3 (cEMI).
 */
#include <stdio.h>
#include <string.h>

#include "../main/knx.h"

static int fail;

static void check(const char *what, int ok, const char *detail)
{
    if (!ok) {
        fail++;
        printf("  FAIL %-38s %s\n", what, detail ? detail : "");
    }
}

static void hex(const uint8_t *b, size_t n, char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 3 < out_sz; i++) {
        o += (size_t)snprintf(out + o, out_sz - o, "%02X", b[i]);
    }
    out[o] = '\0';
}

static void check_bytes(const char *what, const uint8_t *got, size_t got_len,
                        const char *want)
{
    char g[256], d[560];
    hex(got, got_len, g, sizeof(g));
    snprintf(d, sizeof(d), "ist %s, soll %s", g, want);
    check(what, strcmp(g, want) == 0, d);
}

int main(void)
{
    uint8_t buf[KNX_FRAME_MAX];
    char detail[160];
    uint16_t v;

    /* ---- group addresses ---- */
    struct { const char *s; int ok; uint16_t want; } ga[] = {
        { "1/2/3",     1, (1 << 11) | (2 << 8) | 3 },
        { "0/0/1",     1, 1 },
        { "31/7/255",  1, 0xFFFF },
        { "1/2",       1, (1 << 11) | 2 },
        { "2563",      1, 2563 },
        { "32/0/0",    0, 0 },      /* main is five bits */
        { "1/8/0",     0, 0 },      /* middle is three bits */
        { "1/2/256",   0, 0 },      /* sub is eight bits */
        { "0",         0, 0 },      /* 0 is broadcast, never a target */
        { "1/2/3/4",   0, 0 },
        { "1//3",      0, 0 },
        { "1/2/x",     0, 0 },
        { "",          0, 0 },
        { "-1/2/3",    0, 0 },
    };
    for (size_t i = 0; i < sizeof(ga) / sizeof(ga[0]); i++) {
        v = 0xDEAD;
        int got = knx_ga_parse(ga[i].s, &v) ? 1 : 0;
        snprintf(detail, sizeof(detail), "\"%s\" -> %s (0x%04X)",
                 ga[i].s, got ? "ok" : "abgelehnt", v);
        check("Gruppenadresse", got == ga[i].ok, detail);
        if (got && ga[i].ok) {
            check("Gruppenadresse Wert", v == ga[i].want, detail);
        }
    }
    char s[KNX_ADDR_MAX];
    knx_ga_format((1 << 11) | (2 << 8) | 3, s, sizeof(s));
    check("Gruppenadresse formatiert", strcmp(s, "1/2/3") == 0, s);

    /* ---- physical addresses ---- */
    check("physikalisch 1.1.250", knx_pa_parse("1.1.250", &v) && v == 0x11FA, NULL);
    check("physikalisch 15.15.255", knx_pa_parse("15.15.255", &v) && v == 0xFFFF, NULL);
    check("physikalisch 16.0.0 abgelehnt", !knx_pa_parse("16.0.0", &v), NULL);
    check("physikalisch 1.1 abgelehnt", !knx_pa_parse("1.1", &v), NULL);
    knx_pa_format(0x11FA, s, sizeof(s));
    check("physikalisch formatiert", strcmp(s, "1.1.250") == 0, s);

    /* ---- cEMI ----
     * L_Data.req, no additional info, 0xBC/0xE0, source 0.0.0 (the gateway
     * substitutes it), destination 1/2/3 = 0x0A03, one octet of payload,
     * TPCI 0x00, A_GroupValue_Write with the bit set. */
    size_t n = knx_cemi_write_bool(buf, sizeof(buf), 0, 0x0A03, true);
    check("cEMI Länge", n == 11, NULL);
    check_bytes("cEMI ON an 1/2/3", buf, n, "1100BCE000000A03010081");

    n = knx_cemi_write_bool(buf, sizeof(buf), 0, 0x0A03, false);
    check_bytes("cEMI OFF an 1/2/3", buf, n, "1100BCE000000A03010080");

    n = knx_cemi_write_bool(buf, sizeof(buf), 0x11FA, 0x0A03, true);
    check_bytes("cEMI mit Quelle 1.1.250", buf, n, "1100BCE011FA0A03010081");

    check("cEMI in zu kleinem Puffer", knx_cemi_write_bool(buf, 10, 0, 1, true) == 0, NULL);

    /* ---- frames ---- */
    uint8_t cemi[16];
    size_t cl = knx_cemi_write_bool(cemi, sizeof(cemi), 0, 0x0A03, true);

    n = knx_frame_tunnel(buf, sizeof(buf), 0x15, 0x07, cemi, cl);
    check("TUNNELLING_REQUEST Länge", n == 6 + 4 + 11, NULL);
    check_bytes("TUNNELLING_REQUEST", buf, n,
                "0610042000150415" "07" "00" "1100BCE000000A03010081");

    n = knx_frame_routing(buf, sizeof(buf), cemi, cl);
    check_bytes("ROUTING_INDICATION", buf, n,
                "06100530" "0011" "1100BCE000000A03010081");

    /* 192.168.178.24:52000 -> C0A8B218 CB20 */
    n = knx_frame_connect(buf, sizeof(buf), 0xC0A8B218, 52000);
    check("CONNECT_REQUEST Länge", n == 26, NULL);
    check_bytes("CONNECT_REQUEST", buf, n,
                "06100205001A" "0801C0A8B218CB20" "0801C0A8B218CB20" "04040200");

    n = knx_frame_connstate(buf, sizeof(buf), 0x15, 0xC0A8B218, 52000);
    check_bytes("CONNECTIONSTATE_REQUEST", buf, n,
                "061002070010" "15" "00" "0801C0A8B218CB20");

    n = knx_frame_disconnect(buf, sizeof(buf), 0x15, 0xC0A8B218, 52000);
    check_bytes("DISCONNECT_REQUEST", buf, n,
                "061002090010" "15" "00" "0801C0A8B218CB20");

    n = knx_frame_ack(buf, sizeof(buf), 0x15, 0x07);
    check_bytes("TUNNELLING_ACK", buf, n, "06100421000A" "04" "15" "07" "00");

    /* ---- parsing ---- */
    uint16_t service, total;
    const uint8_t ok_res[] = {
        0x06, 0x10, 0x02, 0x06, 0x00, 0x14,           /* CONNECT_RESPONSE, 20 */
        0x15, 0x00,                                    /* channel 0x15, ok */
        0x08, 0x01, 0xC0, 0xA8, 0xB2, 0x87, 0x0E, 0x57,/* gateway HPAI */
        0x04, 0x04, 0x11, 0xFB,                        /* CRD, assigned 1.1.251 */
    };
    uint8_t ch = 0, st = 0xFF;
    uint16_t assigned = 0;
    check("CONNECT_RESPONSE erkannt",
          knx_frame_peek(ok_res, sizeof(ok_res), &service, &total)
            && service == KNX_CONNECT_RES && total == 20, NULL);
    check("CONNECT_RESPONSE geparst",
          knx_parse_connect_res(ok_res, sizeof(ok_res), &ch, &st, &assigned)
            && ch == 0x15 && st == 0x00 && assigned == 0x11FB, NULL);

    /* A refusal carries neither endpoint nor CRD -- reading them anyway would
     * walk off the end of a legitimate frame. */
    const uint8_t busy[] = { 0x06, 0x10, 0x02, 0x06, 0x00, 0x08, 0x00, 0x22 };
    check("volle Gegenstelle geparst",
          knx_parse_connect_res(busy, sizeof(busy), &ch, &st, &assigned)
            && st == 0x22 && assigned == 0, NULL);
    check("Text zur Absage",
          strstr(knx_status_text(0x22), "no free tunnel channel") != NULL,
          knx_status_text(0x22));

    /* A frame claiming to be longer than the datagram must be refused, or
     * every parser after this reads past the buffer. */
    const uint8_t lying[] = { 0x06, 0x10, 0x04, 0x20, 0x00, 0xFF, 0x04, 0x15 };
    check("gelogene Länge abgelehnt",
          !knx_frame_peek(lying, sizeof(lying), &service, &total), NULL);
    const uint8_t stub[] = { 0x06, 0x10, 0x04 };
    check("Bruchstück abgelehnt", !knx_frame_peek(stub, sizeof(stub), NULL, NULL), NULL);
    const uint8_t wrongver[] = { 0x06, 0x11, 0x04, 0x20, 0x00, 0x06 };
    check("falsche Version abgelehnt",
          !knx_frame_peek(wrongver, sizeof(wrongver), NULL, NULL), NULL);

    const uint8_t ack[] = { 0x06, 0x10, 0x04, 0x21, 0x00, 0x0A, 0x04, 0x15, 0x07, 0x00 };
    uint8_t ach, aseq, ast;
    check("ACK-Kopf geparst",
          knx_parse_conn_header(ack, sizeof(ack), &ach, &aseq, &ast)
            && ach == 0x15 && aseq == 0x07 && ast == 0x00, NULL);
    const uint8_t badhdr[] = { 0x06, 0x10, 0x04, 0x21, 0x00, 0x0A, 0x06, 0x15, 0x07, 0x00 };
    check("falsche Kopflänge abgelehnt",
          !knx_parse_conn_header(badhdr, sizeof(badhdr), &ach, &aseq, &ast), NULL);

    printf("%s: KNXnet/IP-Telegramme\n", fail ? "FAILED" : "knx");
    return fail ? 1 : 0;
}
