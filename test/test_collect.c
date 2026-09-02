/* The collect frame parser against real traffic.
 *
 * A gap in this state machine is silent: a type byte with no branch makes
 * those messages vanish with no error anywhere, which is how 0xB5..0xBF went
 * unnoticed while four messages in ten -- including two of the three
 * datapoints the manufacturer's backend steers the storage with -- were being
 * dropped. The fixture therefore covers every type byte the bus actually uses.
 *
 * Its framing comes from a real capture; its payload bytes do not. What is
 * under test is which messages come out and how long they are, and the real
 * bytes were somebody's meter readings and serial numbers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/collect_parse.h"

typedef struct {
    uint16_t cid, did, len;
    uint8_t  data[COLLECT_MAX_LEN];
} exp_t;

static int    fail;
static exp_t  expect[512];
static size_t n_expect, n_got;
static uint32_t expect_abandoned;

static void check(const char *what, int ok, const char *detail)
{
    if (!ok) {
        fail++;
        printf("  FAIL %-28s %s\n", what, detail ? detail : "");
    }
}

static size_t unhex(const char *s, uint8_t *out, size_t max)
{
    size_t n = 0;
    while (s[0] && s[1] && n < max) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "test/collect_frames.txt";
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 1; }

    /* One reassembly state per channel, exactly as the firmware keeps them. */
    struct { uint16_t cid; collect_asm_t st; } chan[8] = { 0 };
    size_t n_chan = 0;
    uint32_t abandoned = 0;

    char line[512];
    /* Expectations are listed after the frames, so read the file twice. */
    while (fgets(line, sizeof(line), f)) {
        unsigned cid, did;
        char hex[512];
        if (line[0] == 'M' &&
            sscanf(line, "M %x %u %511s", &cid, &did, hex) == 3) {
            exp_t *e = &expect[n_expect++];
            e->cid = (uint16_t)cid;
            e->did = (uint16_t)did;
            e->len = (uint16_t)unhex(hex, e->data, sizeof(e->data));
        } else if (line[0] == 'A') {
            sscanf(line, "A %u", &expect_abandoned);
        }
    }
    rewind(f);

    while (fgets(line, sizeof(line), f)) {
        unsigned cid;
        char hex[64];
        if (line[0] != 'F' || sscanf(line, "F %x %63s", &cid, hex) != 2) {
            continue;
        }
        uint8_t d[8];
        size_t dlc = unhex(hex, d, sizeof(d));

        collect_asm_t *st = NULL;
        for (size_t i = 0; i < n_chan; i++) {
            if (chan[i].cid == cid) { st = &chan[i].st; break; }
        }
        if (!st && n_chan < 8) {
            chan[n_chan].cid = (uint16_t)cid;
            st = &chan[n_chan++].st;
        }

        collect_msg_t m;
        if (!collect_feed(st, d, (uint8_t)dlc, &m, &abandoned)) {
            continue;
        }
        char detail[160];
        if (n_got >= n_expect) {
            snprintf(detail, sizeof(detail), "extra message did %u", m.did);
            check("message count", 0, detail);
            n_got++;
            continue;
        }
        const exp_t *e = &expect[n_got];
        snprintf(detail, sizeof(detail), "#%zu: got did %u len %u, want did %u len %u",
                 n_got, m.did, m.len, e->did, e->len);
        check("did", m.did == e->did, detail);
        check("length", m.len == e->len, detail);
        if (m.len == e->len) {
            check("payload", memcmp(m.data, e->data, m.len) == 0, detail);
        }
        n_got++;
    }
    fclose(f);

    char detail[128];
    snprintf(detail, sizeof(detail), "got %zu, want %zu", n_got, n_expect);
    check("all messages seen", n_got == n_expect, detail);
    snprintf(detail, sizeof(detail), "got %u, want %u", abandoned, expect_abandoned);
    check("abandoned count", abandoned == expect_abandoned, detail);

    /* A truncated start frame must not begin a message. */
    collect_asm_t st = { 0 };
    collect_msg_t m;
    const uint8_t stub[] = { 0x21, 0x06, 0x05, 0xB0 };
    collect_feed(&st, stub, sizeof(stub), &m, NULL);
    check("short start ignored", !st.active, "0x21 with no length byte");

    /* An identifier outside the database's range is framing noise. */
    memset(&st, 0, sizeof(st));
    const uint8_t bogus[] = { 0x21, 0xFF, 0xFF, 0xB2, 1, 2, 0, 0 };
    check("absurd did rejected",
          !collect_feed(&st, bogus, sizeof(bogus), &m, NULL), "did 65535");

    printf("%s: %zu messages from %zu expected, %u abandoned\n",
           fail ? "FAILED" : "collect", n_got, n_expect, abandoned);
    return fail ? 1 : 0;
}
