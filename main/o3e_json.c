#include "o3e_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* CPython-compatible double formatting                                 */

/* CPython prints a float as the shortest decimal string that reads back as the
 * same double, then chooses fixed or exponential notation purely from the
 * decimal exponent (fixed for -4 <= exp < 16).  Note this is NOT what
 * printf("%g") does: %g switches on the *precision*, so it would render
 * 100000.0 as "1e+05" where Python gives "100000.0".
 */
size_t o3e_json_fmt_double(double v, char *out, size_t out_sz)
{
    if (out_sz == 0) {
        return 0;
    }
    if (isnan(v)) {
        snprintf(out, out_sz, "NaN");
        return strlen(out);
    }
    if (isinf(v)) {
        snprintf(out, out_sz, v < 0 ? "-Infinity" : "Infinity");
        return strlen(out);
    }

    /* Step 1: shortest significant-digit string that round-trips. */
    char sci[64];
    int prec = 0;
    for (; prec < 17; prec++) {
        snprintf(sci, sizeof(sci), "%.*e", prec, v);
        if (strtod(sci, NULL) == v) {
            break;
        }
    }

    /* Step 2: split "-d.dddde+xx" into sign, digit string and exponent. */
    const char *p = sci;
    bool neg = false;
    if (*p == '-') {
        neg = true;
        p++;
    }
    char digits[32];
    size_t nd = 0;
    for (; *p && *p != 'e' && *p != 'E'; p++) {
        if (*p != '.') {
            digits[nd++] = *p;
        }
    }
    digits[nd] = '\0';
    int exp10 = (*p == 'e' || *p == 'E') ? atoi(p + 1) : 0;

    /* Drop trailing zeros; "%.*e" pads to the requested precision. */
    while (nd > 1 && digits[nd - 1] == '0') {
        digits[--nd] = '\0';
    }

    /* Step 3: notation, chosen from the exponent like CPython does. */
    char tmp[O3E_DOUBLE_STR_MAX * 2];
    size_t n = 0;
    if (neg) {
        tmp[n++] = '-';
    }

    if (exp10 < -4 || exp10 >= 16) {
        tmp[n++] = digits[0];
        if (nd > 1) {
            tmp[n++] = '.';
            memcpy(tmp + n, digits + 1, nd - 1);
            n += nd - 1;
        }
        n += (size_t)snprintf(tmp + n, sizeof(tmp) - n, "e%+03d", exp10);
    } else if (exp10 < 0) {
        /* 0.000ddd */
        tmp[n++] = '0';
        tmp[n++] = '.';
        for (int i = 0; i < -exp10 - 1; i++) {
            tmp[n++] = '0';
        }
        memcpy(tmp + n, digits, nd);
        n += nd;
    } else if ((size_t)exp10 + 1 >= nd) {
        /* integral value: digits, zero padding, then the ".0" CPython keeps */
        memcpy(tmp + n, digits, nd);
        n += nd;
        for (size_t i = nd; i <= (size_t)exp10; i++) {
            tmp[n++] = '0';
        }
        tmp[n++] = '.';
        tmp[n++] = '0';
    } else {
        memcpy(tmp + n, digits, (size_t)exp10 + 1);
        n += (size_t)exp10 + 1;
        tmp[n++] = '.';
        memcpy(tmp + n, digits + exp10 + 1, nd - (size_t)exp10 - 1);
        n += nd - (size_t)exp10 - 1;
    }
    tmp[n] = '\0';

    size_t copy = n < out_sz - 1 ? n : out_sz - 1;
    memcpy(out, tmp, copy);
    out[copy] = '\0';
    return copy;
}

/* ------------------------------------------------------------------ */
/* Growable buffer                                                      */

void o3e_buf_init(o3e_buf_t *b)
{
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = false;
}

void o3e_buf_free(o3e_buf_t *b)
{
    free(b->buf);
    o3e_buf_init(b);
}

static bool buf_reserve(o3e_buf_t *b, size_t extra)
{
    if (b->oom) {
        return false;
    }
    if (b->len + extra + 1 <= b->cap) {
        return true;
    }
    size_t want = b->cap ? b->cap : 128;
    while (want < b->len + extra + 1) {
        want *= 2;
    }
    char *nb = realloc(b->buf, want);
    if (!nb) {
        b->oom = true;
        return false;
    }
    b->buf = nb;
    b->cap = want;
    return true;
}

bool o3e_buf_add(o3e_buf_t *b, const char *data, size_t n)
{
    if (!buf_reserve(b, n)) {
        return false;
    }
    memcpy(b->buf + b->len, data, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return true;
}

bool o3e_buf_addc(o3e_buf_t *b, char c)
{
    return o3e_buf_add(b, &c, 1);
}

bool o3e_buf_adds(o3e_buf_t *b, const char *s)
{
    return o3e_buf_add(b, s, strlen(s));
}

bool o3e_buf_add_json_str(o3e_buf_t *b, const char *s)
{
    if (!o3e_buf_addc(b, '"')) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  o3e_buf_adds(b, "\\\""); break;
        case '\\': o3e_buf_adds(b, "\\\\"); break;
        case '\n': o3e_buf_adds(b, "\\n");  break;
        case '\r': o3e_buf_adds(b, "\\r");  break;
        case '\t': o3e_buf_adds(b, "\\t");  break;
        case '\b': o3e_buf_adds(b, "\\b");  break;
        case '\f': o3e_buf_adds(b, "\\f");  break;
        default:
            if (*p < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", *p);
                o3e_buf_adds(b, esc);
            } else {
                o3e_buf_addc(b, (char)*p);
            }
        }
    }
    return o3e_buf_addc(b, '"');
}

/* ------------------------------------------------------------------ */
/* JSON sink                                                            */
/*                                                                      */
/* Separators are ", " and ": " rather than compact, because open3e emits its  */
/* MQTT payloads with a plain json.dumps() and those are its defaults. Keeping */
/* the spacing lets a payload from this firmware be diffed byte for byte       */
/* against one from open3e.                                                    */

void o3e_json_ctx_init(o3e_json_ctx_t *c, o3e_buf_t *out)
{
    c->out = out;
    c->depth = 0;
    memset(c->need_comma, 0, sizeof(c->need_comma));
    memset(c->in_array, 0, sizeof(c->in_array));
}

/* Emit the comma owed at the current depth, then mark this depth as having
 * produced an element. */
static void sep(o3e_json_ctx_t *c)
{
    if (c->depth >= O3E_JSON_MAX_DEPTH) {
        return;
    }
    if (c->need_comma[c->depth]) {
        o3e_buf_add(c->out, ", ", 2);
    }
    c->need_comma[c->depth] = true;
}

/* Inside an object the comma belongs to the key, so a value must not emit one;
 * inside an array there is no key and the value owns it. */
static void pre_value(o3e_json_ctx_t *c)
{
    if (c->depth < O3E_JSON_MAX_DEPTH && c->in_array[c->depth]) {
        sep(c);
    }
}

static void push(o3e_json_ctx_t *c, bool is_array)
{
    c->depth++;
    if (c->depth < O3E_JSON_MAX_DEPTH) {
        c->need_comma[c->depth] = false;
        c->in_array[c->depth] = is_array;
    }
}

static void pop(o3e_json_ctx_t *c)
{
    if (c->depth) {
        c->depth--;
    }
}

static void j_obj_begin(void *ctx)
{
    o3e_json_ctx_t *c = ctx;
    pre_value(c);
    o3e_buf_addc(c->out, '{');
    push(c, false);
}

static void j_obj_end(void *ctx)
{
    o3e_json_ctx_t *c = ctx;
    pop(c);
    o3e_buf_addc(c->out, '}');
}

static void j_arr_begin(void *ctx)
{
    o3e_json_ctx_t *c = ctx;
    pre_value(c);
    o3e_buf_addc(c->out, '[');
    push(c, true);
}

static void j_arr_end(void *ctx)
{
    o3e_json_ctx_t *c = ctx;
    pop(c);
    o3e_buf_addc(c->out, ']');
}

static void j_key(void *ctx, const char *k)
{
    o3e_json_ctx_t *c = ctx;
    sep(c);
    o3e_buf_add_json_str(c->out, k);
    o3e_buf_add(c->out, ": ", 2);
}

static void j_val_double(void *ctx, double v)
{
    o3e_json_ctx_t *c = ctx;
    char num[O3E_DOUBLE_STR_MAX];
    pre_value(c);
    o3e_json_fmt_double(v, num, sizeof(num));
    o3e_buf_adds(c->out, num);
}

static void j_val_i64(void *ctx, int64_t v)
{
    o3e_json_ctx_t *c = ctx;
    char num[24];
    pre_value(c);
    snprintf(num, sizeof(num), "%lld", (long long)v);
    o3e_buf_adds(c->out, num);
}

static void j_val_str(void *ctx, const char *s)
{
    o3e_json_ctx_t *c = ctx;
    pre_value(c);
    o3e_buf_add_json_str(c->out, s);
}

static void j_val_null(void *ctx)
{
    o3e_json_ctx_t *c = ctx;
    pre_value(c);
    o3e_buf_adds(c->out, "null");
}

const o3e_sink_t o3e_json_sink = {
    .obj_begin  = j_obj_begin,
    .obj_end    = j_obj_end,
    .arr_begin  = j_arr_begin,
    .arr_end    = j_arr_end,
    .key        = j_key,
    .val_double = j_val_double,
    .val_i64    = j_val_i64,
    .val_str    = j_val_str,
    .val_null   = j_val_null,
};
