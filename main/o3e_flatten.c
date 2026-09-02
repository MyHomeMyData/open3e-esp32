#include "o3e_flatten.h"

#include <stdio.h>
#include <string.h>

#include "o3e_json.h"

/* Mirrors open3e's mqttdump(): dict keys and list indices become path
 * segments, and every scalar leaf is published as its plain string form --
 * "27.2", not "\"27.2\"".
 *
 * Path bookkeeping is one mark per level: whatever appended this value's
 * segment (key() for an object member, the index for an array element) records
 * where it started, and leaving the value truncates back to it. */
#define FLAT_DEPTH 12

typedef struct {
    const char *base;
    char        path[256];
    uint8_t     depth;
    size_t      mark[FLAT_DEPTH];      /* path length before this value's segment */
    uint32_t    index[FLAT_DEPTH];     /* next array index at this level */
    bool        in_array[FLAT_DEPTH];
    uint32_t    published;
    void      (*emit)(void *user, const char *topic, const char *value);
    void       *user;
} flat_ctx_t;

static void flat_append(flat_ctx_t *c, const char *seg)
{
    size_t o = strlen(c->path);
    snprintf(c->path + o, sizeof(c->path) - o, "/%s", seg);
}

/* Appends the segment an array element needs; object members already got
 * theirs from key(). */
static void flat_enter(flat_ctx_t *c)
{
    if (c->depth < FLAT_DEPTH && c->in_array[c->depth]) {
        char idx[12];
        snprintf(idx, sizeof(idx), "%u", (unsigned)c->index[c->depth]++);
        c->mark[c->depth] = strlen(c->path);
        flat_append(c, idx);
    }
}

static void flat_leave(flat_ctx_t *c)
{
    if (c->depth < FLAT_DEPTH) {
        c->path[c->mark[c->depth]] = '\0';
    }
}

static void flat_emit(flat_ctx_t *c, const char *value)
{
    char topic[384];
    snprintf(topic, sizeof(topic), "%s%s", c->base, c->path);
    c->emit(c->user, topic, value);
    c->published++;
}

static void flat_open(flat_ctx_t *c, bool is_array)
{
    flat_enter(c);
    if (c->depth + 1 < FLAT_DEPTH) {
        c->in_array[c->depth + 1] = is_array;
        c->index[c->depth + 1] = 0;
        c->mark[c->depth + 1] = strlen(c->path);
    }
    c->depth++;
}

static void flat_close(flat_ctx_t *c)
{
    if (c->depth) {
        c->depth--;
    }
    flat_leave(c);
}

static void f_obj_begin(void *ctx) { flat_open(ctx, false); }
static void f_obj_end(void *ctx)   { flat_close(ctx); }
static void f_arr_begin(void *ctx) { flat_open(ctx, true); }
static void f_arr_end(void *ctx)   { flat_close(ctx); }

static void f_key(void *ctx, const char *k)
{
    flat_ctx_t *c = ctx;
    if (c->depth < FLAT_DEPTH) {
        c->mark[c->depth] = strlen(c->path);
    }
    flat_append(c, k);
}

static void f_double(void *ctx, double v)
{
    flat_ctx_t *c = ctx;
    char num[O3E_DOUBLE_STR_MAX];
    flat_enter(c);
    o3e_json_fmt_double(v, num, sizeof(num));
    flat_emit(c, num);
    flat_leave(c);
}

static void f_i64(void *ctx, int64_t v)
{
    flat_ctx_t *c = ctx;
    char num[24];
    flat_enter(c);
    snprintf(num, sizeof(num), "%lld", (long long)v);
    flat_emit(c, num);
    flat_leave(c);
}

static void f_str(void *ctx, const char *s)
{
    flat_ctx_t *c = ctx;
    flat_enter(c);
    flat_emit(c, s);
    flat_leave(c);
}

/* open3e publishes str(None) for a missing value. */
static void f_null(void *ctx) { f_str(ctx, "None"); }

static const o3e_sink_t flat_sink = {
    .obj_begin = f_obj_begin, .obj_end = f_obj_end,
    .arr_begin = f_arr_begin, .arr_end = f_arr_end,
    .key = f_key,
    .val_double = f_double, .val_i64 = f_i64, .val_str = f_str, .val_null = f_null,
};

uint32_t o3e_flatten(const o3e_node_t *node, const uint8_t *payload, size_t len,
                     const char *base,
                     void (*emit)(void *user, const char *topic, const char *value),
                     void *user)
{
    flat_ctx_t c;
    memset(&c, 0, sizeof(c));
    c.base = base;
    c.emit = emit;
    c.user = user;
    o3e_codec_decode(node, payload, len, &flat_sink, &c);
    return c.published;
}

