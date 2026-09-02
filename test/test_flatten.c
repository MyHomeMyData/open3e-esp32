/* Check the flattened MQTT mode against open3e's mqttdump().
 *
 * 23100 topic/value pairs derived from the same payloads the codec test uses,
 * so a divergence points at the flattening rather than at decoding.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cjson/cJSON.h>

#include "../main/o3e_codec.h"
#include "../main/o3e_db.h"
#include "../main/o3e_flatten.h"

typedef struct {
    const cJSON *want;   /* array of [topic, value] */
    int          i;
    int          mismatches;
    int          extra;
} cmp_t;

static void on_leaf(void *user, const char *topic, const char *value)
{
    cmp_t *c = user;
    const cJSON *pair = cJSON_GetArrayItem(c->want, c->i++);
    if (!pair) {
        c->extra++;
        return;
    }
    const char *wt = cJSON_GetArrayItem(pair, 0)->valuestring;
    const char *wv = cJSON_GetArrayItem(pair, 1)->valuestring;
    if (strcmp(wt, topic) != 0 || strcmp(wv, value) != 0) {
        if (c->mismatches < 10) {
            printf("  want %s = %s\n  got  %s = %s\n", wt, wv, topic, value);
        }
        c->mismatches++;
    }
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    if (b) {
        b[n] = '\0';
    }
    fclose(f);
    return b;
}

static size_t hex2bin(const char *hex, uint8_t *out, size_t out_sz)
{
    size_t n = strlen(hex) / 2;
    if (n > out_sz) {
        n = out_sz;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned b;
        sscanf(hex + i * 2, "%2x", &b);
        out[i] = (uint8_t)b;
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *db_path = argc > 1 ? argv[1] : "data/o3edb.bin";
    const char *fx_path = argc > 2 ? argv[2] : "test/flat_fixtures.json";
    setenv("TZ", "Europe/Berlin", 1);
    tzset();

    if (!o3e_db_open(db_path)) {
        fprintf(stderr, "cannot open %s\n", db_path);
        return 2;
    }
    char *raw = slurp(fx_path);
    cJSON *fx = raw ? cJSON_Parse(raw) : NULL;
    if (!cJSON_IsArray(fx)) {
        fprintf(stderr, "cannot read %s\n", fx_path);
        return 2;
    }

    size_t datapoints = 0, pairs = 0, bad = 0;
    int cached_did = -1;
    o3e_node_t *node = NULL;

    const cJSON *v;
    cJSON_ArrayForEach(v, fx) {
        int did = cJSON_GetObjectItem(v, "did")->valueint;
        const char *hex = cJSON_GetObjectItem(v, "payload")->valuestring;
        const cJSON *want = cJSON_GetObjectItem(v, "pairs");

        if (did != cached_did) {
            o3e_codec_free(node);
            char *json = o3e_db_json((uint16_t)did, 0);
            node = json ? o3e_codec_compile(json) : NULL;
            free(json);
            cached_did = did;
        }
        if (!node) {
            continue;
        }

        uint8_t payload[4096];
        size_t n = hex2bin(hex, payload, sizeof(payload));

        /* The base topic mirrors what gen_flat_fixtures.py used. */
        char base[256];
        const o3e_dp_entry_t *e = o3e_db_find((uint16_t)did);
        snprintf(base, sizeof(base), "open3e/%s", o3e_db_name(e));

        cmp_t c = { .want = want, .i = 0 };
        uint32_t emitted = o3e_flatten(node, payload, n, base, on_leaf, &c);

        datapoints++;
        pairs += emitted;
        int expected = cJSON_GetArraySize(want);
        if (c.mismatches || c.extra || (int)emitted != expected) {
            if (bad < 10) {
                printf("did %d: %u leaves (expected %d), %d mismatched, %d extra\n",
                       did, (unsigned)emitted, expected, c.mismatches, c.extra);
            }
            bad++;
        }
    }

    o3e_codec_free(node);
    cJSON_Delete(fx);
    free(raw);
    o3e_db_close();

    printf("\n%zu datapoints, %zu topic/value pairs, %zu datapoints with differences\n",
           datapoints, pairs, bad);
    return bad ? 1 : 0;
}
