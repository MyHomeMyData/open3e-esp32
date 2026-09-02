/* Reader for the on-device open3e datapoint database (see tools/gen_dpdb.py).
 *
 * Split of concerns: the small, hot tables (datapoint index, names, enum
 * tables -- around 145 KiB together) are loaded into RAM once at boot, while
 * the ~1.4 MiB of codec descriptions stay on flash and are read one record at
 * a time.  Enum lookups therefore never touch the filesystem, which matters
 * because they sit on the per-poll decode path.
 */
#ifndef O3E_DB_H
#define O3E_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t did;
    uint16_t dlen;        /* payload length the general definition expects */
    uint32_t json_off;
    uint32_t json_len;
    uint32_t name_off;    /* into the name blob */
} __attribute__((packed)) o3e_dp_entry_t;

typedef struct {
    uint16_t did;
    uint16_t dlen;        /* payload length this variant applies to */
    uint32_t json_off;
    uint32_t json_len;
} __attribute__((packed)) o3e_var_entry_t;

/* Open the container and load the resident tables. Returns false on any
 * inconsistency; call o3e_db_close() regardless. */
bool        o3e_db_open(const char *path);
void        o3e_db_close(void);
bool        o3e_db_is_open(void);

/* open3e's datapoints "Version" string, e.g. "20260705". */
const char *o3e_db_version(void);
size_t      o3e_db_count(void);

/* Index access. o3e_db_at() is for enumeration (web UI listings). */
const o3e_dp_entry_t *o3e_db_find(uint16_t did);
const o3e_dp_entry_t *o3e_db_at(size_t i);
const char           *o3e_db_name(const o3e_dp_entry_t *e);
/* German reading aid for the name, generated at build time from a glossary
 * (tools/glossary.py). Empty when nothing could be rendered. The English name
 * stays the identifier; this only helps a reader. */
const char           *o3e_db_name_de(size_t i);

/* Fetch the codec description for a DID as NUL-terminated JSON.
 * `dlen` is the payload length the ECU actually returned: when a
 * length-specific variant exists it wins over the general definition, which is
 * how open3e handles firmware differences between device generations.
 * Caller frees. Returns NULL if the DID is unknown. */
char *o3e_db_json(uint16_t did, uint16_t dlen);

/* Enum text for `value` in list `list_name`, or NULL when not mapped.
 * The returned pointer is owned by the database and stays valid until close. */
const char *o3e_db_enum(const char *list_name, int32_t value);

/* Enumerate an enum list, for building Home Assistant `select` options. */
size_t      o3e_db_enum_count(const char *list_name);
bool        o3e_db_enum_at(const char *list_name, size_t i,
                           int32_t *out_value, const char **out_text);

/* ------------------------------------------------------------------ */
/* Energy meters (E3onCAN)                                              */
/*
 * The E380 answers no request: it broadcasts eight bytes on a fixed CAN-ID and
 * that is the entire protocol. These frames are therefore keyed by CAN-ID
 * rather than by DID, and are looked up on every received frame -- so the
 * index stays resident like the datapoint index does.
 */
typedef struct {
    uint16_t can_id;
    uint16_t dlen;
    uint32_t json_off;
    uint32_t json_len;
} __attribute__((packed)) o3e_em_entry_t;

size_t                o3e_db_em_count(void);
const o3e_em_entry_t *o3e_db_em_at(size_t i);
const o3e_em_entry_t *o3e_db_em_find(uint16_t can_id);
/* Codec description for one broadcast frame. Caller frees. */
char                 *o3e_db_em_json(uint16_t can_id);

#endif /* O3E_DB_H */
