/* Hold control datapoints against the installation's own regulator.
 *
 * The Vitocal's energy manager writes the storage unit's control datapoints
 * roughly every ten seconds: the grid setpoint to zero, and the charge and
 * discharge limits to their maxima. That is ordinary self-consumption
 * regulation. It runs locally, it is not exposed as a setting anywhere, and
 * switching every management option off in the app changes nothing about it.
 *
 * So none of it is taken over -- it is out-written. This module rewrites the
 * wanted values every two seconds, which beats the regulator's nine and a half
 * by a comfortable margin. Nothing in the installation is modified. Stop, and
 * the manager has its setpoints back within ten seconds by itself.
 *
 * That is the safety property, and it is the reason for the design: no state
 * is left behind. A finished deadline, a reboot, a crash, a pulled cable --
 * every one of them ends every hold, and the system returns to normal
 * operation on its own. Nothing can be left running unattended.
 *
 * Two things are held, independently, each with its own deadline:
 *
 *   2188  the setpoint at the grid connection point. Plain watts in a signed
 *         16-bit field, negative to draw from the grid. Measured twice on a
 *         Vitocharge VX3: -1000 produced 1003 W drawn and 1046 W more
 *         charging, decaying within five seconds of the manager writing back.
 *
 *   2226  the storage's own charge and discharge limits, two 32-bit fields.
 *         Only the extremes are used here -- zero and whatever the manager
 *         itself writes -- because those need no knowledge of the scale, and
 *         the scale is not known. Zero is zero in any unit.
 */
#ifndef O3E_HOLD_H
#define O3E_HOLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GRID_HOLD_DID       2188
#define STORAGE_HOLD_DID    2226

/* The third field of both datapoints, identical in every message the manager
 * sends. It looks like a validity period in seconds, so it is repeated rather
 * than invented. */
#define HOLD_VALIDITY       120

/* What the manager writes into the limits, and therefore what "no limit"
 * means here. Reads as 100.000 % if the scale is thousandths of a percent,
 * which is the best remaining explanation after the nominal inverter power
 * turned out to be 5880 W rather than the 10 kW the value would need to be
 * tenths of a watt. Nothing here depends on being right about that: only this
 * value and zero are ever written. */
#define HOLD_LIMIT_OPEN     100000

/* Beaten against the regulator's ~9.3 s, with room for a missed turn. */
#define HOLD_PERIOD_MS      2000

/* Caps, deliberately low. A wrong sign or a stray zero should cost minutes and
 * a few hundred watt-hours, not an afternoon. */
#define GRID_HOLD_MAX_W     6000
#define GRID_HOLD_MAX_S     3600

typedef enum {
    /* Not held at all: the manager decides, which is the normal state. */
    STORAGE_MODE_NORMAL = 0,
    STORAGE_MODE_IDLE,             /* neither charge nor discharge */
    STORAGE_MODE_CHARGE_ONLY,      /* charge from surplus, never give back */
    STORAGE_MODE_DISCHARGE_ONLY,   /* discharge, do not take any in */
} storage_mode_t;

typedef struct {
    bool     active;
    uint16_t ecu;
    int16_t  watts;        /* negative draws from the grid */
    uint32_t remaining_s;
    uint32_t writes;
    uint32_t failures;
    char     last_error[96];
} grid_hold_status_t;

typedef struct {
    bool           active;
    storage_mode_t mode;
    uint32_t       remaining_s;
    uint32_t       writes;
    uint32_t       failures;
} storage_hold_status_t;

/* `watts` is negative to draw from the grid, positive to feed in. Fails, with
 * a reason, on a value or duration beyond the caps, or when writing to the bus
 * is switched off in the system settings. Starting again replaces a running
 * hold rather than stacking on it. */
bool grid_hold_start(uint16_t ecu, int16_t watts, uint32_t seconds,
                     char *err, size_t err_sz);

/* Ends the hold and lets the setpoint go. Does not write a neutral value: the
 * manager restores it within ten seconds anyway, and writing one more time
 * would be one more chance to write it wrongly. */
void grid_hold_stop(void);

/* Start from the stored settings, or stop. This is what a Home Assistant
 * switch flips: the switch has no room to carry a power and a duration, so it
 * uses the ones in the system settings. */
bool grid_hold_switch(bool on, char *err, size_t err_sz);

void grid_hold_status(grid_hold_status_t *out);

/* STORAGE_MODE_NORMAL stops the hold; every other mode starts or replaces one.
 * The duration is capped like the grid hold's. */
bool storage_hold_start(uint16_t ecu, storage_mode_t mode, uint32_t seconds,
                        char *err, size_t err_sz);
void storage_hold_status(storage_hold_status_t *out);
const char *storage_mode_name(storage_mode_t mode);
bool storage_mode_parse(const char *name, storage_mode_t *out);

/* Publish both holds to <base>/hold, retained. Called on every change and
 * regularly while one runs, so a subscriber that connects late still learns
 * that something is overriding the installation's own regulation. */
void hold_publish(void);

#endif /* O3E_HOLD_H */
