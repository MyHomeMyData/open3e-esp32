/* Viessmann E380 energy meter, read passively.
 *
 * Ported from E3onCAN (github.com/MyHomeMyData/E3onCAN). The E380 answers no
 * UDS request -- it broadcasts eight bytes on CAN-IDs 0x250..0x25D and that is
 * the entire protocol, so open3e cannot see it at all. Even IDs belong to the
 * meter at CAN address 97, odd IDs to the one at 98.
 *
 * Because it is broadcast, there is nothing to poll and nothing to schedule:
 * frames arrive at the meter's own rate and are published as they come, rate
 * limited per frame so a chatty meter cannot flood the broker.
 */
#ifndef O3E_EM380_H
#define O3E_EM380_H

#include <stdbool.h>
#include <stdint.h>

#define EM380_CAN_FIRST 0x250
#define EM380_CAN_LAST  0x25D

typedef struct {
    bool     enabled;
    bool     seen;            /* at least one frame has arrived */
    uint32_t frames;          /* frames received since boot */
    uint32_t published;
    uint32_t dropped;         /* the receive queue was full */
    uint32_t last_seen_ms;
    uint16_t ids_seen;        /* bitmask over EM380_CAN_FIRST..LAST */
} em380_stats_t;

/* Start or stop listening. Enabling is cheap: it installs a receive filter,
 * it does not touch the bus. */
bool em380_start(void);
void em380_stop(void);
void em380_stats(em380_stats_t *out);

/* Re-read /data/points.json for the frames the user selected. */
void em380_reload(void);

/* Last decoded value for one CAN-ID, as JSON. Caller frees; NULL if that
 * frame has not been seen yet. Used by the web UI's live view. */
char *em380_last_json(uint16_t can_id);

#endif /* O3E_EM380_H */
