/* The two spare pins: potential-free contacts in, or relays out.
 *
 * The board brings GND, 3V3, GPIO1 and GPIO2 out on the SH1.0 connector, and
 * nothing on the board uses those two: the CAN transceiver sits on 15 and 16,
 * the RS485 driver on 17, 18 and 21, and neither pin is a strapping pin on the
 * ESP32-S3 (those are 0, 3, 45 and 46). So both can read a switch -- a
 * doorbell, a door contact, a float switch, a fault relay -- and the gateway
 * that is already in the boiler room reports it to the same broker as
 * everything else.
 *
 * Or, per pin, the other direction: an output driving a relay module. The
 * heat pump's SG-Ready terminals are the reason -- the controller only reads
 * them as contacts, there is no datapoint to write, so blocking the unit from
 * an automation means a relay, and the gateway sitting next to the terminals
 * is the obvious thing to switch it. An output is off after every boot: a
 * lock that survives a reboot nobody remembers is worse than one that drops.
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

typedef enum {
    CONTACT_MODE_INPUT = 0,
    CONTACT_MODE_OUTPUT,
} contact_mode_t;

#define CONTACT_GA_MAX     16

typedef struct {
    bool           enabled;
    contact_mode_t mode;
    char           name[CONTACT_NAME_MAX];    /* shown in Home Assistant */
    char           device_class[CONTACT_CLASS_MAX];
    /* Input only. */
    contact_wire_t wire;
    uint16_t       release_ms;
    /* Output only: "on" drives the pin low instead of high. Most cheap relay
     * modules switch on a low input, with an LED that would otherwise light
     * up whenever the gateway is off. */
    bool           active_low;
    /* KNX group address, empty to send nothing. One bit, DPT 1.001: closed
     * writes 1, open writes 0. Both are sent, because a group address that
     * only ever receives a 1 stays at 1 -- the next reader of that address
     * would see a doorbell permanently ringing. */
    char           knx_ga[CONTACT_GA_MAX];
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

/* What a pin is called when nobody has named it: deliberately generic,
 * because the pins are generic. "Eingang 1", "Eingang 2" -- or "Ausgang". */
const char *contact_default_name(const contact_cfg_t *cfg, int idx);

/* Whether `payload` means on, off, or neither: ON/OFF, 1/0, true/false, in
 * any case. Shared by the MQTT set topic and the command listener so the two
 * never disagree about what counts as a switch-on. */
bool contact_parse_onoff(const char *payload, size_t len, bool *on);

/* Topic-safe name for one input: the configured name folded to lower case
 * with the German umlauts spelled out, or the default name when it has none.
 * Renaming an input therefore moves its topic, which is the intended
 * behaviour -- the topic is meant to read like the thing it reports. */
void contact_slug(const contact_cfg_t *cfg, int idx, char *out, size_t out_sz);

/* ---- the task ----------------------------------------------------- */

typedef struct {
    bool           enabled;
    contact_mode_t mode;
    bool           active;     /* input closed, or output switched on */
    uint32_t       edges;      /* activations: rings, or switch-ons */
    uint32_t       since_s;    /* how long the current state has held */
} contact_status_t;

/* Configures the enabled pins and starts sampling. Safe to call again after a
 * settings change: it reconfigures and carries on. */
void contact_start(void);
void contact_status(int idx, contact_status_t *out);

/* Republish the current states, retained. Called after an MQTT (re)connect so
 * a broker that lost its retained set is filled in again. */
void contact_publish(void);

/* Switch an output. False when the pin is not an enabled output. Only records
 * the wish: the sampling task drives the pin and publishes the change, so
 * this is safe from the MQTT client's task, the web server and a KNX
 * telegram alike. */
bool contact_output_set(int idx, bool on);
/* The same, addressed by topic name, for <base>/output/<slug>/set. */
bool contact_output_set_by_slug(const char *slug, bool on);

#endif /* O3E_CONTACT_H */
