/* Replay E3onCAN's own decode results for the E380 energy meter.
 *
 * The meter's frames run through the same codec engine as the open3e
 * datapoints, but two definitions differ upstream and are easy to get subtly
 * wrong: E3onCAN's O3EFloat32 divides by a scale (open3e's does not), and
 * O3EcosPhi takes its magnitude from the second byte and its sign from the
 * first. Both are measured here rather than assumed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    const char *fx_path = argc > 2 ? argv[2] : "test/em_fixtures.json";

    if (!o3e_db_open(db_path)) {
        fprintf(stderr, "cannot open %s\n", db_path);
        return 2;
    }
    printf("%zu energy-meter frames in the database\n", o3e_db_em_count());

    char *raw = slurp(fx_path);
    cJSON *fx = raw ? cJSON_Parse(raw) : NULL;
    if (!cJSON_IsArray(fx)) {
        fprintf(stderr, "cannot read %s\n", fx_path);
        return 2;
    }

    size_t total = 0, pass = 0, shown = 0;
    int cached = -1;
    o3e_node_t *node = NULL;

    const cJSON *v;
    cJSON_ArrayForEach(v, fx) {
        int can_id = cJSON_GetObjectItem(v, "canId")->valueint;
        const char *name = cJSON_GetObjectItem(v, "name")->valuestring;
        const char *hex = cJSON_GetObjectItem(v, "payload")->valuestring;
        const char *want = cJSON_GetObjectItem(v, "expected")->valuestring;

        if (can_id != cached) {
            o3e_codec_free(node);
            char *json = o3e_db_em_json((uint16_t)can_id);
            node = json ? o3e_codec_compile(json) : NULL;
            free(json);
            cached = can_id;
        }
        total++;
        if (!node) {
            if (shown++ < 10) {
                printf("FAIL 0x%03X: not in the database\n", can_id);
            }
            continue;
        }

        uint8_t payload[16];
        size_t n = hex2bin(hex, payload, sizeof(payload));
        char *got = o3e_codec_decode_json(node, payload, n);

        if (got && strcmp(got, want) == 0) {
            pass++;
        } else if (shown++ < 10) {
            printf("FAIL 0x%03X %s\n  payload %s\n  want %s\n  got  %s\n",
                   can_id, name, hex, want, got ? got : "(null)");
        }
        free(got);
    }

    o3e_codec_free(node);
    cJSON_Delete(fx);
    free(raw);
    o3e_db_close();

    printf("\n%zu/%zu E380 vectors match E3onCAN\n", pass, total);
    return pass == total ? 0 : 1;
}
