/* Discovery of ECUs and datapoints on the E3 bus.
 *
 * Mirrors open3e's Open3E_depictSystem.py: sweep the request COB-IDs
 * 0x680..0x6EF probing DID 256 (BusIdentification), then enumerate the DIDs of
 * every ECU that answered.  Runs in its own task and streams progress to the
 * web UI, because a full sweep takes minutes.
 */
#ifndef O3E_E3_SCAN_H
#define O3E_E3_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* open3e's defaults. The range is configurable because it turned out to be too
 * narrow: control traffic was found on 0x441, well outside it. The full 11-bit
 * identifier space is allowed, at the cost of a much longer sweep. */
#define SCAN_COB_FIRST  0x680
#define SCAN_COB_LAST   0x6EF
#define SCAN_COB_MIN    0x000
#define SCAN_COB_MAX    0x7FF
#define SCAN_DID_FIRST  256
#define SCAN_DID_LAST   4000
/* open3e sweeps 112 addresses. A real system has a handful of devices, but the
 * limit must not be a silent ceiling: scan_status_t reports when it is hit. */
#define SCAN_MAX_ECUS   32

typedef enum {
    /* Probe only the DIDs the bundled database knows about: about a minute per
     * ECU instead of the ten to twenty minutes upstream reports for a full
     * sweep, and enough for everything the UI can decode anyway. */
    SCAN_MODE_KNOWN = 0,
    /* Every DID from 256 to 4000, including ones absent from the database.
     * Slow, but finds datapoints a firmware update added. */
    SCAN_MODE_FULL,
} scan_mode_t;

typedef enum {
    SCAN_IDLE = 0,
    SCAN_ECUS,
    SCAN_DIDS,
    SCAN_DONE,
    SCAN_FAILED,
} scan_phase_t;

typedef struct {
    uint16_t addr;             /* request COB-ID */
    /* Free-form key used for the {device} placeholder in MQTT topics. Defaults
     * to "0x680" and is editable, which is exactly how open3e's devices.json
     * works -- its dev_of_addr() returns the user's key and falls back to the
     * hex address. Keeping the same default means a format string containing
     * {device} produces the same topics here as it did on the Pi. */
    char     name[32];
    /* Everything below is read from the bus, not configured: DID 256
     * (BusIdentification) decoded with its real codec, plus DID 377. */
    char     prop[40];         /* DeviceProperty, e.g. HPMUMASTER */
    char     function[40];     /* DeviceFunction */
    char     bus_type[24];
    char     sw[40];
    char     hw[40];
    char     vin[24];
    char     vid[20];          /* ViessmannIdentificationNumber (DID 377) */
    uint16_t n_dids;
} scan_ecu_t;

/* Progress only. The device records themselves are not in here on purpose:
 * this struct is copied on every status poll, and the web UI reads devices
 * from the scan result file instead. */
typedef struct {
    scan_phase_t phase;
    scan_mode_t  mode;
    uint8_t      n_ecus;
    bool         ecu_limit_hit;   /* more devices answered than we can record */
    uint8_t      cur_ecu;
    uint16_t     cur_did;
    uint32_t     probed;       /* requests issued so far */
    uint32_t     total;        /* requests planned, for a progress bar */
    uint16_t     cob_first;
    uint16_t     cob_last;
    uint32_t     started_ms;
    /* When the probe counter last moved. A frozen counter with a busy bus is
     * the signature of one stuck exchange, and the UI can say so outright
     * instead of leaving a number standing still. */
    uint32_t     last_progress_ms;
    char         message[96];
} scan_status_t;

/* Start a scan over the given request COB-ID range. Passing 0 for both uses
 * open3e's 0x680..0x6EF. Returns false if one is already running. */
bool e3_scan_start(scan_mode_t mode, uint16_t cob_first, uint16_t cob_last);
void e3_scan_abort(void);
void e3_scan_status(scan_status_t *out);
bool e3_scan_running(void);

/* Last completed result, as the JSON the web UI and the poller consume.
 * Shape mirrors open3e's devices.json plus the discovered DID list. */
char *e3_scan_result_json(void);

/* Rename one ECU. The name feeds the {device} topic placeholder, so callers
 * should reload the poller afterwards. */
bool e3_scan_rename(uint16_t addr, const char *name);

#endif /* O3E_E3_SCAN_H */
