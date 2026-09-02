/* Replay open3e's own decode results against the C port.
 *
 * Fixtures come from tools/gen_fixtures.py, which runs the real Python
 * implementation.  A mismatch here means the port diverged from open3e, not
 * that a hand-written expectation is stale.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include <cjson/cJSON.h>

#include "../main/o3e_codec.h"
#include "../main/o3e_db.h"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    if (buf) {
        buf[n] = '\0';
    }
    fclose(f);
    return buf;
}

static size_t hex2bin(const char *hex, uint8_t *out, size_t out_sz)
{
    size_t n = strlen(hex) / 2;
    if (n > out_sz) {
        n = out_sz;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned byte;
        sscanf(hex + i * 2, "%2x", &byte);
        out[i] = (uint8_t)byte;
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *db_path = argc > 1 ? argv[1] : "data/o3edb.bin";
    const char *fx_path = argc > 2 ? argv[2] : "test/fixtures.json";
    /* Fixtures were generated under Europe/Berlin; O3EUtc and O3EDateTime are
     * local-time codecs upstream, so the comparison must use the same zone. */
    setenv("TZ", "Europe/Berlin", 1);
    tzset();

    if (!o3e_db_open(db_path)) {
        fprintf(stderr, "cannot open database %s\n", db_path);
        return 2;
    }
    printf("database version %s, %zu datapoints\n", o3e_db_version(), o3e_db_count());

    char *raw = slurp(fx_path);
    if (!raw) {
        return 2;
    }
    cJSON *fx = cJSON_Parse(raw);
    if (!cJSON_IsArray(fx)) {
        fprintf(stderr, "fixtures are not a JSON array\n");
        return 2;
    }

    size_t total = 0, pass = 0, shown = 0;
    /* Compiling per vector would dominate the runtime; DIDs are grouped in the
     * fixture file, so caching the last tree is enough. */
    int cached_did = -1;
    o3e_node_t *node = NULL;

    const cJSON *v;
    cJSON_ArrayForEach(v, fx) {
        int did = cJSON_GetObjectItem(v, "did")->valueint;
        const char *codec = cJSON_GetObjectItem(v, "codec")->valuestring;
        const char *hex = cJSON_GetObjectItem(v, "payload")->valuestring;
        const char *want = cJSON_GetObjectItem(v, "expected")->valuestring;

        if (did != cached_did) {
            o3e_codec_free(node);
            char *json = o3e_db_json((uint16_t)did, 0);
            node = json ? o3e_codec_compile(json) : NULL;
            free(json);
            cached_did = did;
        }
        total++;
        if (!node) {
            if (shown++ < 15) {
                printf("FAIL did %d (%s): no codec in database\n", did, codec);
            }
            continue;
        }

        uint8_t payload[4096];
        size_t n = hex2bin(hex, payload, sizeof(payload));
        char *got = o3e_codec_decode_json(node, payload, n);

        if (got && strcmp(got, want) == 0) {
            pass++;
        } else if (shown++ < 15) {
            printf("FAIL did %d %s\n  payload %s\n  want %s\n  got  %s\n",
                   did, codec, hex, want, got ? got : "(null)");
        }
        free(got);
    }

    o3e_codec_free(node);
    cJSON_Delete(fx);
    free(raw);
    o3e_db_close();

    printf("\n%zu/%zu vectors match open3e\n", pass, total);
    return pass == total ? 0 : 1;
}
