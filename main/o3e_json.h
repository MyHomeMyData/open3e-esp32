/* Streaming JSON output plus CPython-compatible number formatting.
 *
 * open3e produces its MQTT payloads with Python's json.dumps(), so matching it
 * means matching repr() of a float exactly -- "3772.8", not "3772.800000".
 * o3e_json_fmt_double() implements the same shortest-round-trip rule CPython
 * uses, which is why the C port can be diffed against open3e's own output.
 */
#ifndef O3E_JSON_H
#define O3E_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Longest possible output: sign + 17 digits + point + exponent + NUL. */
#define O3E_DOUBLE_STR_MAX 32

/* Format `v` the way CPython's repr()/json.dumps() would.
 * Returns the number of characters written (excluding the NUL). */
size_t o3e_json_fmt_double(double v, char *out, size_t out_sz);

/* ------------------------------------------------------------------ */
/* Growable output buffer                                              */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    bool   oom;   /* sticky: set once an allocation failed */
} o3e_buf_t;

void  o3e_buf_init(o3e_buf_t *b);
void  o3e_buf_free(o3e_buf_t *b);
bool  o3e_buf_add(o3e_buf_t *b, const char *data, size_t n);
bool  o3e_buf_addc(o3e_buf_t *b, char c);
bool  o3e_buf_adds(o3e_buf_t *b, const char *s);
/* Append `s` as a JSON string literal, including the surrounding quotes. */
bool  o3e_buf_add_json_str(o3e_buf_t *b, const char *s);

/* ------------------------------------------------------------------ */
/* Decode sink                                                          */
/*                                                                      */
/* Decoding emits events rather than building a tree, so the same walk   */
/* serves both MQTT modes: the JSON sink concatenates one payload, while */
/* the flattening sink in mqtt_pub.c publishes one topic per leaf. No    */
/* intermediate cJSON tree is allocated on the polling path.             */

typedef struct o3e_sink {
    void (*obj_begin)(void *ctx);
    void (*obj_end)(void *ctx);
    void (*arr_begin)(void *ctx);
    void (*arr_end)(void *ctx);
    void (*key)(void *ctx, const char *k);
    void (*val_double)(void *ctx, double v);
    void (*val_i64)(void *ctx, int64_t v);
    void (*val_str)(void *ctx, const char *s);
    void (*val_null)(void *ctx);
} o3e_sink_t;

/* A sink that renders events into an o3e_buf_t as compact JSON. */
#define O3E_JSON_MAX_DEPTH 16

typedef struct {
    o3e_buf_t *out;
    uint8_t    depth;
    bool       need_comma[O3E_JSON_MAX_DEPTH];
    bool       in_array[O3E_JSON_MAX_DEPTH];
} o3e_json_ctx_t;

extern const o3e_sink_t o3e_json_sink;
void o3e_json_ctx_init(o3e_json_ctx_t *c, o3e_buf_t *out);

#endif /* O3E_JSON_H */
