/* Port of open3e's codecs (src/open3e/Open3Ecodecs.py) to C.
 *
 * Usage is two-stage on purpose.  o3e_codec_compile() turns the JSON codec
 * description from the database into a compact tree once, when a datapoint is
 * selected; decoding then walks only that tree.  The polling path therefore
 * touches neither the filesystem nor a JSON parser, which is what makes
 * one-second intervals across dozens of datapoints affordable.
 *
 * Decoded output is byte-identical to open3e's, verified against fixtures
 * generated from the Python implementation (see tools/gen_fixtures.py).
 */
#ifndef O3E_CODEC_H
#define O3E_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "o3e_json.h"

struct cJSON;

typedef enum {
    O3E_K_UNKNOWN = 0,
    O3E_K_RAW,
    O3E_K_INT,
    O3E_K_FLOAT32,
    O3E_K_BYTEVAL,
    O3E_K_BOOL,
    O3E_K_UTF8,
    O3E_K_SOFTVERS,
    O3E_K_MACADDR,
    O3E_K_IP4ADDR,
    O3E_K_SDATE,
    O3E_K_DATETIME,
    O3E_K_STIME,
    O3E_K_UTC,
    O3E_K_ENUM,
    O3E_K_LIST,
    O3E_K_ARRAY,
    O3E_K_COMPLEX,
    O3E_K_SWITCH,
    /* From E3onCAN, for the E380 energy meter: two bytes where the second is
     * the magnitude and the first carries the sign as 0x04. */
    O3E_K_COSPHI,
} o3e_kind_t;

typedef enum { O3E_ACC_NONE = 0, O3E_ACC_RO, O3E_ACC_RW } o3e_acc_t;

typedef struct o3e_node o3e_node_t;

struct o3e_node {
    o3e_kind_t   kind;
    uint16_t     len;          /* payload width in bytes */
    char        *id;           /* field name */
    char        *unit;         /* degrees, percent, ... or NULL */
    char        *list_name;    /* enum list, for O3E_K_ENUM / O3E_K_SWITCH */
    double       scale;        /* O3E_K_INT: decoded = raw / scale */
    uint8_t      decimals;
    bool         signd;
    bool         ts_format;    /* O3E_K_DATETIME: false = "VM", true = "ts" */
    o3e_acc_t    acc;
    uint16_t     array_len;    /* O3E_K_ARRAY repeat count */
    o3e_node_t **kids;
    uint16_t     n_kids;
    int32_t     *case_vals;    /* O3E_K_SWITCH, parallel to kids */
    bool        *case_default; /* O3E_K_SWITCH, marks the fallback branch */
};

/* Build a codec tree from a database record (see o3e_db_json()). */
o3e_node_t *o3e_codec_compile(const char *json);

/* A bare RawCodec node of `len` bytes, for datapoints the database has no
 * description for. A scan can find DIDs that open3e does not document yet;
 * they answered, so their bytes are real and worth showing as hex rather than
 * dropping. This is what open3e's raw mode produces for the same datapoint. */
o3e_node_t *o3e_codec_raw(uint16_t len, const char *id);
o3e_node_t *o3e_codec_compile_cjson(const struct cJSON *node);
void        o3e_codec_free(o3e_node_t *n);

/* True when open3e is able to encode this node, i.e. writing is supported.
 * Several codecs (text, MAC, dates) raise "not implemented yet" upstream; we
 * report them as read-only rather than inventing an encoding of our own. */
bool o3e_codec_encodable(const o3e_node_t *n);

/* Decode `data` into sink events. Returns false only on malformed input. */
bool o3e_codec_decode(const o3e_node_t *n, const uint8_t *data, size_t len,
                      const o3e_sink_t *sink, void *ctx);

/* Convenience wrapper: decode straight to a JSON string. Caller frees. */
char *o3e_codec_decode_json(const o3e_node_t *n, const uint8_t *data, size_t len);

/* Encode a cJSON value into `out`. Writes exactly n->len bytes on success.
 * On failure, fills `err` with a human-readable reason for the web UI. */
bool o3e_codec_encode(const o3e_node_t *n, const struct cJSON *value,
                      uint8_t *out, size_t out_sz, char *err, size_t err_sz);

#endif /* O3E_CODEC_H */
