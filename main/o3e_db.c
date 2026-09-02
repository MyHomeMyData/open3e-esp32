#include "o3e_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#define ESP_LOGE(tag, ...) fprintf(stderr, __VA_ARGS__)
#endif

static const char *TAG = "db";

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
/* The resident tables are ~145 KiB and are read far more often than they are
 * written; PSRAM is the right home for them and keeps internal RAM free for
 * the network stack. */
#define DB_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define DB_ALLOC(n) malloc((n))
#endif

#define HEADER_SIZE     64
#define SECTION_NAME    12
#define ENUM_NAME_MAX   40

typedef struct {
    char     name[SECTION_NAME];
    uint32_t off;
    uint32_t len;
} __attribute__((packed)) section_t;

typedef struct {
    char     name[ENUM_NAME_MAX];
    uint32_t ent_off;      /* index into the entry array, not a byte offset */
    uint32_t ent_count;
} __attribute__((packed)) enum_idx_t;

typedef struct {
    int32_t  value;
    uint32_t str_off;
} __attribute__((packed)) enum_ent_t;

static struct {
    FILE           *fp;
    char            version[17];

    o3e_dp_entry_t *dp;
    size_t          n_dp;
    char           *names;
    char           *german;
    uint32_t       *de_off;
    size_t          n_de;

    o3e_var_entry_t *var;
    size_t           n_var;

    o3e_em_entry_t  *em;
    size_t           n_em;
    uint32_t         em_json_off;

    enum_idx_t     *eidx;
    size_t          n_enum;
    enum_ent_t     *eent;
    char           *estr;

    uint32_t        dp_json_off;
    uint32_t        var_json_off;
} db;

static bool read_at(uint32_t off, void *dst, size_t n)
{
    return fseek(db.fp, (long)off, SEEK_SET) == 0 && fread(dst, 1, n, db.fp) == n;
}

/* Read a whole section into freshly allocated memory. */
static void *load_section(const section_t *s, size_t *out_len)
{
    void *p = DB_ALLOC(s->len ? s->len : 1);
    if (!p) {
        return NULL;
    }
    if (s->len && !read_at(s->off, p, s->len)) {
        free(p);
        return NULL;
    }
    if (out_len) {
        *out_len = s->len;
    }
    return p;
}

static const section_t *find_section(const section_t *tab, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++) {
        if (strncmp(tab[i].name, name, SECTION_NAME) == 0) {
            return &tab[i];
        }
    }
    return NULL;
}

/* Every failure path below leaves the database closed rather than half-open:
 * o3e_db_is_open() is what the rest of the firmware trusts before it reads. */
static bool db_open_inner(const char *path);

bool o3e_db_open(const char *path)
{
    o3e_db_close();
    if (!db_open_inner(path)) {
        o3e_db_close();
        return false;
    }
    return true;
}

static bool db_open_inner(const char *path)
{
    db.fp = fopen(path, "rb");
    if (!db.fp) {
        return false;
    }

    uint8_t hdr[HEADER_SIZE];
    if (!read_at(0, hdr, sizeof(hdr)) || memcmp(hdr, "O3EDB", 5) != 0) {
        return false;
    }
    uint32_t container_version, n_sections;
    memcpy(&container_version, hdr + 8, 4);
    memcpy(db.version, hdr + 12, 16);
    db.version[16] = '\0';
    memcpy(&n_sections, hdr + 28, 4);
    if (container_version != 3) {
        /* A firmware paired with a database built for a different container
         * layout is refused rather than misread. Without a clear message this
         * shows up only as every datapoint being "unknown". */
        ESP_LOGE(TAG, "database has container format %u, this firmware needs %u "
                 "- reflash the storage partition",
                 (unsigned)container_version, 3u);
        return false;
    }
    if (n_sections == 0 || n_sections > 32) {
        return false;
    }

    section_t tab[32];
    if (!read_at(HEADER_SIZE, tab, sizeof(section_t) * n_sections)) {
        return false;
    }

    const section_t *s_dp   = find_section(tab, n_sections, "dp_idx");
    const section_t *s_name = find_section(tab, n_sections, "dp_name");
    const section_t *s_json = find_section(tab, n_sections, "dp_json");
    const section_t *s_var  = find_section(tab, n_sections, "var_idx");
    const section_t *s_vjs  = find_section(tab, n_sections, "var_json");
    const section_t *s_eidx = find_section(tab, n_sections, "enum_idx");
    const section_t *s_eent = find_section(tab, n_sections, "enum_ent");
    const section_t *s_estr = find_section(tab, n_sections, "enum_str");
    const section_t *s_emi  = find_section(tab, n_sections, "em_idx");
    const section_t *s_emj  = find_section(tab, n_sections, "em_json");
    const section_t *s_de   = find_section(tab, n_sections, "dp_de");
    const section_t *s_deo  = find_section(tab, n_sections, "dp_de_off");
    if (!s_dp || !s_name || !s_json || !s_var || !s_vjs || !s_eidx || !s_eent ||
        !s_estr || !s_emi || !s_emj || !s_de || !s_deo) {
        return false;
    }

    size_t len;
    db.dp = load_section(s_dp, &len);
    if (!db.dp) {
        return false;
    }
    db.n_dp = len / sizeof(o3e_dp_entry_t);

    db.names = load_section(s_name, NULL);
    db.german = load_section(s_de, NULL);
    db.de_off = load_section(s_deo, &len);
    if (!db.german || !db.de_off) {
        return false;
    }
    db.n_de = len / sizeof(uint32_t);
    db.var = load_section(s_var, &len);
    if (!db.names || !db.var) {
        return false;
    }
    db.n_var = len / sizeof(o3e_var_entry_t);

    db.eidx = load_section(s_eidx, &len);
    if (!db.eidx) {
        return false;
    }
    db.n_enum = len / sizeof(enum_idx_t);

    db.eent = load_section(s_eent, NULL);
    db.estr = load_section(s_estr, NULL);
    if (!db.eent || !db.estr) {
        return false;
    }

    db.em = load_section(s_emi, &len);
    if (!db.em) {
        return false;
    }
    db.n_em = len / sizeof(o3e_em_entry_t);

    db.dp_json_off = s_json->off;
    db.var_json_off = s_vjs->off;
    db.em_json_off = s_emj->off;
    return true;
}

void o3e_db_close(void)
{
    if (db.fp) {
        fclose(db.fp);
    }
    free(db.dp);
    free(db.names);
    free(db.german);
    free(db.de_off);
    free(db.var);
    free(db.em);
    free(db.eidx);
    free(db.eent);
    free(db.estr);
    memset(&db, 0, sizeof(db));
}

bool o3e_db_is_open(void)      { return db.fp != NULL; }
const char *o3e_db_version(void) { return db.version; }
size_t o3e_db_count(void)      { return db.n_dp; }

const o3e_dp_entry_t *o3e_db_at(size_t i)
{
    return i < db.n_dp ? &db.dp[i] : NULL;
}

const char *o3e_db_name(const o3e_dp_entry_t *e)
{
    return (e && db.names) ? db.names + e->name_off : NULL;
}

const char *o3e_db_name_de(size_t i)
{
    if (!db.german || !db.de_off || i >= db.n_de) {
        return "";
    }
    return db.german + db.de_off[i];
}

const o3e_dp_entry_t *o3e_db_find(uint16_t did)
{
    size_t lo = 0, hi = db.n_dp;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (db.dp[mid].did < did) {
            lo = mid + 1;
        } else if (db.dp[mid].did > did) {
            hi = mid;
        } else {
            return &db.dp[mid];
        }
    }
    return NULL;
}

/* Variants are keyed by (did, dlen): the same DID can carry different layouts
 * across device generations, and the response length is what disambiguates. */
static const o3e_var_entry_t *find_variant(uint16_t did, uint16_t dlen)
{
    size_t lo = 0, hi = db.n_var;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const o3e_var_entry_t *v = &db.var[mid];
        if (v->did < did || (v->did == did && v->dlen < dlen)) {
            lo = mid + 1;
        } else if (v->did > did || (v->did == did && v->dlen > dlen)) {
            hi = mid;
        } else {
            return v;
        }
    }
    return NULL;
}

static char *read_json(uint32_t base, uint32_t off, uint32_t len)
{
    char *buf = malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    if (!read_at(base + off, buf, len)) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

char *o3e_db_json(uint16_t did, uint16_t dlen)
{
    if (dlen) {
        const o3e_var_entry_t *v = find_variant(did, dlen);
        if (v) {
            return read_json(db.var_json_off, v->json_off, v->json_len);
        }
    }
    const o3e_dp_entry_t *e = o3e_db_find(did);
    if (!e) {
        return NULL;
    }
    return read_json(db.dp_json_off, e->json_off, e->json_len);
}

size_t o3e_db_em_count(void) { return db.n_em; }

const o3e_em_entry_t *o3e_db_em_at(size_t i)
{
    return i < db.n_em ? &db.em[i] : NULL;
}

const o3e_em_entry_t *o3e_db_em_find(uint16_t can_id)
{
    size_t lo = 0, hi = db.n_em;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (db.em[mid].can_id < can_id) {
            lo = mid + 1;
        } else if (db.em[mid].can_id > can_id) {
            hi = mid;
        } else {
            return &db.em[mid];
        }
    }
    return NULL;
}

char *o3e_db_em_json(uint16_t can_id)
{
    const o3e_em_entry_t *e = o3e_db_em_find(can_id);
    if (!e) {
        return NULL;
    }
    return read_json(db.em_json_off, e->json_off, e->json_len);
}

static const enum_idx_t *find_enum(const char *list_name)
{
    if (!list_name) {
        return NULL;
    }
    for (size_t i = 0; i < db.n_enum; i++) {
        if (strncmp(db.eidx[i].name, list_name, ENUM_NAME_MAX) == 0) {
            return &db.eidx[i];
        }
    }
    return NULL;
}

const char *o3e_db_enum(const char *list_name, int32_t value)
{
    const enum_idx_t *e = find_enum(list_name);
    if (!e) {
        return NULL;
    }
    const enum_ent_t *ent = db.eent + e->ent_off;
    size_t lo = 0, hi = e->ent_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ent[mid].value < value) {
            lo = mid + 1;
        } else if (ent[mid].value > value) {
            hi = mid;
        } else {
            return db.estr + ent[mid].str_off;
        }
    }
    return NULL;
}

size_t o3e_db_enum_count(const char *list_name)
{
    const enum_idx_t *e = find_enum(list_name);
    return e ? e->ent_count : 0;
}

bool o3e_db_enum_at(const char *list_name, size_t i, int32_t *out_value, const char **out_text)
{
    const enum_idx_t *e = find_enum(list_name);
    if (!e || i >= e->ent_count) {
        return false;
    }
    const enum_ent_t *ent = db.eent + e->ent_off + i;
    if (out_value) {
        *out_value = ent->value;
    }
    if (out_text) {
        *out_text = db.estr + ent->str_off;
    }
    return true;
}
