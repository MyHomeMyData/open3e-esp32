/* Check the write path against open3e's own encode().
 *
 * Two claims are tested:
 *   1. For every datapoint open3e can encode, the C port produces the same
 *      bytes from the same JSON value.
 *   2. For every datapoint open3e refuses ("not implemented yet"), the C port
 *      refuses too.  Silently inventing an encoding and writing it to a heat
 *      pump is the worst outcome available here.
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

static void tohex(const uint8_t *d, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(out + i * 2, "%02x", d[i]);
    }
    out[n * 2] = '\0';
}

int main(int argc, char **argv)
{
    const char *db_path = argc > 1 ? argv[1] : "data/o3edb.bin";
    const char *fx_path = argc > 2 ? argv[2] : "test/encode_fixtures.json";
    setenv("TZ", "Europe/Berlin", 1);
    tzset();

    if (!o3e_db_open(db_path)) {
        fprintf(stderr, "cannot open %s\n", db_path);
        return 2;
    }
    char *raw = slurp(fx_path);
    cJSON *fx = raw ? cJSON_Parse(raw) : NULL;
    if (!fx) {
        fprintf(stderr, "cannot read %s\n", fx_path);
        return 2;
    }

    size_t total = 0, pass = 0, shown = 0;
    int cached_did = -1;
    o3e_node_t *node = NULL;

    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(fx, "encodable")) {
        int did = cJSON_GetObjectItem(v, "did")->valueint;
        const char *codec = cJSON_GetObjectItem(v, "codec")->valuestring;
        const char *val_json = cJSON_GetObjectItem(v, "value")->valuestring;
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
            continue;
        }

        cJSON *value = cJSON_Parse(val_json);
        uint8_t out[4096];
        char err[192] = "";
        bool ok = o3e_codec_encode(node, value, out, sizeof(out), err, sizeof(err));

        char got[8200];
        if (ok) {
            tohex(out, node->len, got);
        }
        if (ok && strcmp(got, want) == 0) {
            pass++;
        } else if (shown++ < 12) {
            printf("FAIL did %d %s\n  value %s\n  want  %s\n  got   %s%s\n",
                   did, codec, val_json, want, ok ? got : "(refused) ", err);
        }
        cJSON_Delete(value);
    }

    /* Second claim: refusals must match. Compared by actually attempting the
     * encode, because upstream refuses per value, not per codec type: an
     * unmapped enum text or a three-field list fails while the same codec
     * encodes fine with other input. */
    size_t refused_total = 0, refused_ok = 0;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(fx, "refused")) {
        int did = cJSON_GetObjectItem(v, "did")->valueint;
        const char *val_json = cJSON_GetObjectItem(v, "value")->valuestring;
        const char *reason = cJSON_GetObjectItem(v, "reason")->valuestring;

        o3e_codec_free(node);
        node = NULL;
        cached_did = -1;
        char *json = o3e_db_json((uint16_t)did, 0);
        o3e_node_t *n = json ? o3e_codec_compile(json) : NULL;
        free(json);
        if (!n) {
            continue;
        }
        refused_total++;

        cJSON *value = cJSON_Parse(val_json);
        uint8_t out[4096];
        char err[192] = "";
        bool ok = value && o3e_codec_encode(n, value, out, sizeof(out), err, sizeof(err));
        if (!ok) {
            refused_ok++;
        } else if (shown++ < 20) {
            printf("FAIL did %d %s: open3e refuses (%s), the C port encoded it\n",
                   did, cJSON_GetObjectItem(v, "codec")->valuestring, reason);
        }
        cJSON_Delete(value);
        o3e_codec_free(n);
    }

    o3e_codec_free(node);
    cJSON_Delete(fx);
    free(raw);
    o3e_db_close();

    printf("\n%zu/%zu encodable vectors match open3e\n", pass, total);
    printf("%zu/%zu refusals match open3e\n", refused_ok, refused_total);
    return (pass == total && refused_ok == refused_total) ? 0 : 1;
}
