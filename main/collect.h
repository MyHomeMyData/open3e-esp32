/* Viessmann's "collect" broadcast protocol.
 *
 * E3 devices announce their own datapoints on a fixed CAN identifier without
 * being asked -- the Vitocharge VX3 on 0x451, the Vitocal on 0x693. That is
 * how a device's state becomes visible even when the command that changed it
 * came from somewhere else entirely, such as the manufacturer's backend.
 *
 * The framing *is* ISO-TP, contrary to how E3onCAN describes it. A capture of
 * 21603 frames from a VX3 shows first frames of the form
 *
 *     10 1B 77 00 00 43 01 82        length 0x01B, service 0x77
 *
 * carrying open3e's experimental write service, whose payload is
 *
 *     77 <counter lo> <counter hi> <tag> 01 82 <did lo> <did hi> <type> <data>
 *
 * The six-byte header ends exactly where the first consecutive frame begins,
 * so a parser that hooks the 0x21 frame lands on <did> <type> <data> and gets
 * the right answer without reassembling anything. That coincidence is why the
 * simpler reading works, and it is worth knowing that it is a coincidence.
 *
 * The type byte gives the payload length: 0xB1..0xB4 fit beside the header,
 * 0xB5..0xBF spill into the following frame, and 0xB0 means the length itself
 * follows (one byte later still if that byte is 0xC1). Continuations count
 * 0x22, 0x23, ... wrapping 0x2F to 0x20.
 *
 * The identifier is an ordinary open3e DID, so the datapoint database names
 * and decodes it exactly as it does a polled one.
 *
 * Ported from E3onCAN (github.com/MyHomeMyData/E3onCAN).
 */
#ifndef O3E_COLLECT_H
#define O3E_COLLECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Defaults from E3onCAN's device table. */
#define COLLECT_CANID_VX3   0x451
#define COLLECT_CANID_VCAL  0x693

#define COLLECT_MAX_DIDS 64

/* Several channels can run at once: a Vitocharge announces its datapoints on
 * 0x451 and its control mode on 0x441. Each needs its own reassembly state --
 * the two interleave, and one shared state would splice their messages
 * together. */
#define COLLECT_MAX_IDS 4

typedef struct {
    bool     enabled;
    uint8_t  n_ids;
    uint16_t can_ids[COLLECT_MAX_IDS];
    uint32_t messages;      /* complete messages reassembled */
    uint32_t incomplete;    /* started but never finished */
    uint32_t dropped;       /* reassembled, but the queue was full */
    uint32_t published;
    uint16_t n_dids;        /* distinct identifiers seen */
    uint32_t last_ms;
} collect_stats_t;

typedef struct {
    uint16_t    did;
    uint16_t    len;
    uint32_t    count;
    uint32_t    last_ms;
    const char *name;   /* from the database, or NULL */
    const char *json;   /* decoded value, owned by the module */
} collect_entry_t;

/* True for the datapoints the manufacturer's backend steers the storage with.
 * These are published to <base>/control/<name> and reported to the tracer on
 * every change, independently of what the user selected for MQTT. */
bool collect_is_control_did(uint16_t did);

/* `ids` lists the channels to listen on, e.g. {0x451, 0x441}. */
bool collect_start(const uint16_t *ids, size_t n);

/* Parse a comma-separated list like "0x451,0x441". Returns how many were
 * recognised. */
size_t collect_parse_ids(const char *text, uint16_t *out, size_t max);
void collect_stop(void);
void collect_stats(collect_stats_t *out);
void collect_reload(void);

/* Snapshot of what has been seen, most recent first. */
size_t collect_entries(collect_entry_t *out, size_t max);

/* Releases the strings collect_entries() copied. Copies, because the decode
 * task frees the originals as soon as the next message for that datapoint
 * arrives. */
void collect_entries_free(collect_entry_t *e, size_t n);

#endif /* O3E_COLLECT_H */
