/* Hold the grid connection setpoint against the system's own regulator.
 *
 * The Vitocal's energy manager writes PointOfCommonCouplingSetActivePowerTotal
 * (DID 2188) on the storage unit roughly every ten seconds, telling it to keep
 * the grid exchange at zero. That is ordinary self-consumption regulation, it
 * runs locally, and it is not exposed as a setting anywhere -- switching every
 * management option off in the app changed nothing about it.
 *
 * So the setpoint is not taken over, it is out-written: this module writes the
 * wanted value every two seconds, which beats the regulator's ten by a
 * comfortable margin. Nothing in the installation is modified. Stop, and the
 * manager has the setpoint back within ten seconds by itself.
 *
 * That is also the safety property, and it is the reason for the design: there
 * is no state left behind. A finished deadline, a reboot, a crash, a pulled
 * cable -- every one of them ends the hold, and the system returns to normal
 * operation on its own. Nothing can be left running unattended.
 *
 * Measured on a Vitocharge VX3, twice: writing -1000 produced 1003 W drawn
 * from the grid and 1046 W more charging, and it decayed within five seconds
 * of the manager writing back. The value is plain watts in a signed 16-bit
 * field, negative to draw from the grid.
 */
#ifndef O3E_GRID_HOLD_H
#define O3E_GRID_HOLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DID 2188, six bytes: int16 watts, then int32 = 120. The second field is the
 * same in every message the manager sends and in the datapoint next to it;
 * it looks like a validity period in seconds, so it is repeated rather than
 * invented. */
#define GRID_HOLD_DID       2188
#define GRID_HOLD_VALIDITY  120

/* Beaten against the regulator's ~9.3 s, with room for a missed turn. */
#define GRID_HOLD_PERIOD_MS 2000

/* Caps, deliberately low. A wrong sign or a stray zero should cost minutes and
 * a few hundred watt-hours, not an afternoon. */
#define GRID_HOLD_MAX_W     6000
#define GRID_HOLD_MAX_S     3600

typedef struct {
    bool     active;
    uint16_t ecu;
    int16_t  watts;        /* negative draws from the grid */
    uint32_t remaining_s;
    uint32_t writes;
    uint32_t failures;
    char     last_error[96];
} grid_hold_status_t;

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

void grid_hold_status(grid_hold_status_t *out);

/* Start from the stored settings, or stop. This is what a Home Assistant
 * switch flips: the switch has no room to carry a power and a duration, so it
 * uses the ones in the system settings. */
bool grid_hold_switch(bool on, char *err, size_t err_sz);

/* Publish the state to <base>/grid, retained. Called on every change and
 * regularly while a hold runs, so a subscriber that connects late still learns
 * that something is overriding the installation's own regulation. */
void grid_hold_publish(void);

#endif /* O3E_GRID_HOLD_H */
