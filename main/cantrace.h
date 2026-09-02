/* Capture every frame on the bus, for reverse engineering.
 *
 * The gateway's own request/response traffic is only part of what happens on
 * an E3 bus: the devices command each other, and that is where the answer to
 * "how is this actually triggered" lives -- a write from the HMI to the
 * battery, say. Seeing it requires recording frames the gateway neither sent
 * nor asked for.
 *
 * Frames are captured in the receive interrupt into a lock-free ring in PSRAM;
 * transmitted frames are recorded too, so an exchange reads in order rather
 * than showing only one side of the conversation.
 *
 * Interpretation deliberately happens in the browser: reassembling ISO-TP and
 * naming data identifiers is analysis that wants changing often, and the web
 * interface can be replaced in seconds where firmware cannot.
 */
#ifndef O3E_CANTRACE_H
#define O3E_CANTRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 16 bytes per frame.
 *
 * Sized from a real bus: with the gateway's own polling excluded, an E3
 * installation with seven devices produces about 84 frames a second. The
 * default therefore covers roughly a quarter of an hour, which is what makes
 * the lead-up to a triggered event worth reading -- the previous 16384 frames
 * were barely three minutes. The ring lives in PSRAM, where four megabytes is
 * a small part of what is free. */
#define CANTRACE_DEFAULT_FRAMES 65536
#define CANTRACE_MAX_FRAMES     262144

typedef struct {
    uint32_t us;          /* microseconds since capture started */
    uint16_t id;          /* 11-bit CAN identifier */
    uint8_t  dlc;
    uint8_t  flags;       /* bit 0: transmitted by us rather than received */
    uint8_t  data[8];
} __attribute__((packed)) cantrace_frame_t;

#define CANTRACE_FLAG_TX 0x01

/* What to do when a UDS write request appears on the bus.
 *
 * A ring alone cannot catch an event whose timing is unknown -- and here the
 * trigger comes from Viessmann's backend through one of the gateway ECUs, so
 * it could be hours away. Recording continuously and freezing on the write
 * keeps the lead-up instead of overwriting it. */
typedef enum {
    CANTRACE_TRIG_NONE = 0,
    CANTRACE_TRIG_WRITE,      /* UDS 0x2E seen anywhere on the bus */
    /* Learn what the bus normally does, then fire on anything that deviates.
     *
     * For a command whose identifier, service and even mechanism are unknown,
     * looking for a specific message is the wrong way round: it may not be a
     * UDS write at all, but a changed value inside a periodic broadcast. This
     * mode records, for a learning period, which identifiers occur and which
     * payload bytes ever vary -- then fires on a new identifier, or on a byte
     * that had been constant throughout and suddenly is not. */
    CANTRACE_TRIG_NOVEL,
    /* Fire when one of the datapoints the backend steers the storage with
     * changes value.
     *
     * The two modes above look at raw frames. This one cannot: on a Vitocharge
     * the control traffic is ISO-TP carrying service 0x77, so the value sits
     * several frames deep and a byte comparison on any single frame says
     * nothing. The collect module already reassembles and decodes that stream,
     * so it reports the change here rather than this module parsing it twice.
     * See cantrace_note_control(). */
    CANTRACE_TRIG_CONTROL,
} cantrace_trigger_t;

typedef enum {
    CANTRACE_EV_WRITE = 0,
    CANTRACE_EV_NEW_ID,       /* an identifier not seen during learning */
    CANTRACE_EV_BYTE_CHANGE,  /* a byte that had been constant changed */
    CANTRACE_EV_CONTROL,      /* a watched control datapoint changed value */
} cantrace_event_kind_t;

typedef struct {
    bool     fired;
    cantrace_event_kind_t kind;
    uint32_t us;              /* when, relative to the start of capture */
    uint16_t can_id;
    uint16_t did;             /* for CANTRACE_EV_WRITE and _CONTROL */
    uint8_t  byte_index;      /* only for CANTRACE_EV_BYTE_CHANGE */
    uint8_t  was, now;        /* the value before and after */
    uint8_t  dlc;
    uint8_t  data[8];
} cantrace_event_t;

/* Per-identifier statistics, gathered whenever capture runs.
 *
 * Useful on its own: it says which identifiers exist on this bus, how busy
 * each is, and which payload bytes actually carry information. That is the map
 * you need before you can look for anything specific. */
#define CANTRACE_MAX_IDS 255

typedef struct {
    uint16_t id;
    uint32_t count;
    uint32_t first_us;
    uint32_t last_us;
    uint8_t  first[8];
    uint8_t  last[8];
    uint8_t  varying;         /* bit per payload byte: this byte has changed */
    uint8_t  dlc;
} cantrace_id_stat_t;

typedef struct {
    bool     running;
    uint32_t captured;    /* frames written since start, including overwritten */
    uint32_t stored;      /* frames currently in the ring */
    uint32_t capacity;
    uint32_t dropped;     /* lost to wraparound while the ring was full */
    uint32_t elapsed_ms;
    uint16_t filter_lo;   /* inclusive ID range; 0..0x7FF captures everything */
    uint16_t filter_hi;
    cantrace_trigger_t trigger;
    bool     exclude_own;
    uint32_t skipped_own;   /* frames left out as our own doing */
    bool     triggered;
    uint32_t post_remaining;   /* frames still being recorded after the trigger */
    cantrace_event_t event;
} cantrace_stats_t;

/* Allocate the ring and begin capturing. Restarting clears what was there.
 * With a trigger set, capture continues for `post_frames` after it fires and
 * then stops on its own, so the buffer holds both sides of the event. */
bool cantrace_start(uint32_t frames, uint16_t filter_lo, uint16_t filter_hi,
                    cantrace_trigger_t trigger, uint32_t post_frames,
                    uint32_t learn_s, bool exclude_own);

/* Mark the window in which this gateway is conducting its own exchange.
 *
 * With exclude_own set, frames belonging to that exchange are left out of the
 * trace entirely -- not just the requests this device transmits, but the
 * replies to them, which arrive on `ecu_tx + 0x10` and are just as much "our
 * doing". Polling runs continuously, so without this the recording is mostly
 * the gateway talking to itself, and the learning phase would treat that
 * traffic as part of the bus's normal behaviour. */
void cantrace_own_begin(uint16_t ecu_tx);
void cantrace_own_end(void);

/* Report that a watched control datapoint changed. Trips the trigger when
 * CANTRACE_TRIG_CONTROL is armed and does nothing otherwise, so the caller
 * need not know what the trace is currently doing. Not for interrupt context.
 * `data` is the new payload, at most eight bytes of it are kept. */
void cantrace_note_control(uint16_t can_id, uint16_t did,
                           const uint8_t *data, uint8_t len);

/* Read the per-identifier table, most active first. */
size_t cantrace_ids(cantrace_id_stat_t *out, size_t max);
bool   cantrace_learning(uint32_t *remaining_s);

/* Set a callback invoked once, outside interrupt context, when the trigger
 * fires -- so an unattended capture can announce itself over MQTT rather than
 * waiting to be noticed. */
void cantrace_on_trigger(void (*cb)(const cantrace_event_t *));
void cantrace_stop(void);
void cantrace_free(void);
void cantrace_stats(cantrace_stats_t *out);

/* Called from the receive ISR and from the transmit path. Cheap and lock-free;
 * a no-op while capture is stopped. */
void cantrace_put(uint16_t id, const uint8_t *data, uint8_t dlc, bool tx);

/* Read out frames, oldest first. `from` is an index into the stored range.
 * Returns how many were copied. */
size_t cantrace_read(size_t from, cantrace_frame_t *out, size_t max);

#endif /* O3E_CANTRACE_H */
