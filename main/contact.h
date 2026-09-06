/* Potential-free contacts on the two spare pins.
 *
 * The board brings GND, 3V3, GPIO1 and GPIO2 out on the SH1.0 connector, and
 * nothing on the board uses those two: the CAN transceiver sits on 15 and 16,
 * the RS485 driver on 17, 18 and 21, and neither pin is a strapping pin on the
 * ESP32-S3 (those are 0, 3, 45 and 46). So both can read a switch -- a
 * doorbell, a door contact, a float switch, a fault relay -- and the gateway
 * that is already in the boiler room reports it to the same broker as
 * everything else.
 *
 * The debouncing is the part worth having its own file: it is pure, it is
 * where the mistakes are, and it can be tested on a workstation instead of on
 * a doorstep.
 */
#ifndef O3E_CONTACT_H
#define O3E_CONTACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONTACT_COUNT      2
#define CONTACT_NAME_MAX   32
#define CONTACT_CLASS_MAX  16

/* The pins behind index 0 and 1, in connector order. */
extern const int CONTACT_PINS[CONTACT_COUNT];

/* Sampling period of the input task. */
#define CONTACT_POLL_MS      5
/* Consecutive active samples before an input counts as active: 10 ms. */
#define CONTACT_ATTACK       2
/* Default quiet time before it counts as inactive again. */
#define CONTACT_RELEASE_MS   150
#define CONTACT_RELEASE_MIN  20
#define CONTACT_RELEASE_MAX  5000

typedef enum {
    /* Contact between the pin and GND, held high by the internal pull-up
     * while open. A push button, a reed switch and an optocoupler's output
     * transistor are all wired this way; it is the default because it is the
     * only one that needs no external part at all. */
    CONTACT_TO_GND = 0,
    /* Contact between the pin and 3V3, internal pull-down. */
    CONTACT_TO_3V3,
} contact_wire_t;

typedef struct {
    bool           enabled;
    char           name[CONTACT_NAME_MAX];    /* shown in Home Assistant */
    char           device_class[CONTACT_CLASS_MAX];
    contact_wire_t wire;
    uint16_t       release_ms;
} contact_cfg_t;

/* ---- the debouncer ------------------------------------------------ */
/*
 * Fast attack, slow release, because two very different signals have to come
 * out of the same code.
 *
 * A dry contact bounces for a few milliseconds on each edge and is then
 * steady. A doorbell sensed through an optocoupler is never steady: German
 * bell circuits run on 8-12 V AC, and an optocoupler across the coil conducts
 * on one half wave only, so the pin chops at 50 Hz for as long as the button
 * is held. A symmetric debounce -- N equal samples in a row -- settles on the
 * first and never on the second.
 *
 * Two consecutive active samples are needed to go active, which rejects a
 * single induced spike on what is often many metres of unshielded bell wire.
 * Going inactive needs an uninterrupted quiet period of `release_ms`, which
 * bridges the 10 ms gaps of a chopped signal with room to spare. The cost is
 * that release is reported `release_ms` late -- irrelevant for a doorbell, and
 * adjustable for anything that cares.
 */
typedef struct {
    bool     active;
    uint8_t  run;              /* consecutive active samples, capped */
    uint32_t last_active_ms;
    uint32_t since_ms;         /* when the current state began */
    uint32_t edges;            /* transitions to active, i.e. rings */
} contact_deb_t;

void contact_deb_reset(contact_deb_t *st, uint32_t now_ms);

/* Feed one raw sample; true when the debounced state changed. */
bool contact_deb_step(contact_deb_t *st, bool raw_active, uint32_t now_ms,
                      uint16_t release_ms);

/* Topic-safe name for one input: the configured name folded to lower case
 * with the German umlauts spelled out, or "in1"/"in2" when it has none.
 * Renaming an input therefore moves its topic, which is the intended
 * behaviour -- the topic is meant to read like the thing it reports. */
void contact_slug(const contact_cfg_t *cfg, int idx, char *out, size_t out_sz);

/* ---- the task ----------------------------------------------------- */

typedef struct {
    bool     enabled;
    bool     active;
    uint32_t edges;
    uint32_t since_s;      /* how long the current state has held */
} contact_status_t;

/* Configures the enabled pins and starts sampling. Safe to call again after a
 * settings change: it reconfigures and carries on. */
void contact_start(void);
void contact_status(int idx, contact_status_t *out);

/* Republish the current states, retained. Called after an MQTT (re)connect so
 * a broker that lost its retained set is filled in again. */
void contact_publish(void);

#endif /* O3E_CONTACT_H */
