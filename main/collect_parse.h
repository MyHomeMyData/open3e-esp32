/* The frame-level state machine of the "collect" stream, on its own.
 *
 * Separated from collect.c so it can be exercised on a workstation against
 * real captures. It is the piece that decides which datapoints the gateway
 * ever sees, and a gap in it is silent: a type byte with no branch simply
 * makes those messages disappear, with no error anywhere. One such gap cost
 * four messages in ten on a Vitocharge bus, including two of the three
 * datapoints the manufacturer's backend steers the storage with.
 *
 * No FreeRTOS, no allocation, no logging -- feed it frames, get messages.
 */
#ifndef O3E_COLLECT_PARSE_H
#define O3E_COLLECT_PARSE_H

#include <stdbool.h>
#include <stdint.h>

/* The largest datapoint in the database is 199 bytes; anything longer is a
 * framing error, not a datapoint. */
#define COLLECT_MAX_LEN 200

typedef struct {
    bool     active;
    uint8_t  expect;      /* next sequence byte */
    uint16_t did;
    uint16_t need;
    uint16_t have;
    uint8_t  buf[COLLECT_MAX_LEN];
} collect_asm_t;

typedef struct {
    uint16_t did;
    uint16_t len;
    uint8_t  data[COLLECT_MAX_LEN];
} collect_msg_t;

/* Feed one CAN frame belonging to this channel.
 *
 * Returns true when a message completed, with `out` filled. `abandoned`, when
 * given, is incremented for each message that was started and then interrupted
 * -- the count worth watching, because it rises when the framing assumption is
 * wrong rather than when the bus is merely quiet. */
bool collect_feed(collect_asm_t *st, const uint8_t *d, uint8_t dlc,
                  collect_msg_t *out, uint32_t *abandoned);

#endif /* O3E_COLLECT_PARSE_H */
