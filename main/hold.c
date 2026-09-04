#include "hold.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "mqtt_pub.h"
#include "poller.h"

static const char *TAG = "hold";

/* Stop rather than hammer: if the storage has begun refusing the write, the
 * next fifty attempts will not go better, and a hold that cannot take effect
 * should end loudly instead of running as a lie on the status page. */
#define HOLD_MAX_FAILS 5

/* One per datapoint held, each with its own deadline, because the two are
 * independent: charging from the grid while the storage is otherwise idle is a
 * sensible combination, and so is neither. */
enum { SLOT_GRID = 0, SLOT_STORAGE, N_SLOTS };

typedef struct {
    bool     active;
    uint16_t ecu;
    uint16_t did;
    uint8_t  bytes[12];
    uint8_t  len;
    int64_t  deadline_us;
    uint32_t writes;
    uint32_t fails;
    char     last_error[96];
} slot_t;

static SemaphoreHandle_t lock;
static TaskHandle_t      task_h;
static slot_t            slots[N_SLOTS];
static int16_t           grid_watts;
static storage_mode_t    storage_mode;

static void put_i32(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* The datapoints are RawCodecs, so the value goes in as a hex string -- quotes
 * and all, because that is a JSON string. */
static void as_json(char *out, size_t out_sz, const uint8_t *b, uint8_t len)
{
    size_t o = 0;
    out[o++] = '"';
    for (uint8_t i = 0; i < len && o + 3 < out_sz; i++) {
        o += (size_t)snprintf(out + o, out_sz - o, "%02x", b[i]);
    }
    snprintf(out + o, out_sz - o, "\"");
}

static bool ensure_task(char *err, size_t err_sz);

static bool slot_start(int idx, uint16_t ecu, uint16_t did,
                       const uint8_t *bytes, uint8_t len, uint32_t seconds,
                       char *err, size_t err_sz)
{
    if (seconds == 0 || seconds > GRID_HOLD_MAX_S) {
        snprintf(err, err_sz, "the duration must be between 1 and %d seconds",
                 GRID_HOLD_MAX_S);
        return false;
    }
    sys_cfg_t sys;
    sys_cfg_get(&sys);
    if (!sys.write_enabled) {
        snprintf(err, err_sz, "writing is disabled in the system settings");
        return false;
    }
    if (!ensure_task(err, err_sz)) {
        return false;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    slot_t *s = &slots[idx];
    s->ecu = ecu;
    s->did = did;
    s->len = len;
    memcpy(s->bytes, bytes, len);
    s->deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    s->writes = s->fails = 0;
    s->last_error[0] = '\0';
    s->active = true;
    xSemaphoreGive(lock);

    ESP_LOGW(TAG, "holding 0x%03X.%u for %u s", ecu, did, (unsigned)seconds);
    hold_publish();
    return true;
}

static void slot_stop(int idx)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool was = slots[idx].active;
    slots[idx].active = false;
    xSemaphoreGive(lock);
    if (was) {
        ESP_LOGI(TAG, "hold on %u stopped after %u writes",
                 slots[idx].did, (unsigned)slots[idx].writes);
        hold_publish();
    }
}

static void hold_task(void *arg)
{
    (void)arg;
    uint32_t turns = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HOLD_PERIOD_MS));
        bool any = false;

        for (int i = 0; i < N_SLOTS; i++) {
            xSemaphoreTake(lock, portMAX_DELAY);
            slot_t s = slots[i];
            bool run = s.active && esp_timer_get_time() < s.deadline_us;
            bool expired = s.active && !run;
            if (expired) {
                slots[i].active = false;
            }
            xSemaphoreGive(lock);

            if (expired) {
                ESP_LOGI(TAG, "hold on %u finished after %u writes",
                         s.did, (unsigned)s.writes);
                hold_publish();
                continue;
            }
            if (!run) {
                continue;
            }
            any = true;

            char json[40];
            as_json(json, sizeof(json), s.bytes, s.len);
            char err[96] = "";
            /* Forced: the database marks both datapoints read-only, which the
             * device does not -- it acknowledges the write and acts on it. */
            if (poller_write_now(s.ecu, s.did, json, true, err, sizeof(err))) {
                xSemaphoreTake(lock, portMAX_DELAY);
                slots[i].writes++;
                xSemaphoreGive(lock);
            } else {
                xSemaphoreTake(lock, portMAX_DELAY);
                slots[i].fails++;
                snprintf(slots[i].last_error, sizeof(slots[i].last_error),
                         "%s", err);
                bool give_up = slots[i].fails >= HOLD_MAX_FAILS;
                if (give_up) {
                    slots[i].active = false;
                }
                xSemaphoreGive(lock);
                ESP_LOGW(TAG, "write to %u failed: %s", s.did, err);
                if (give_up) {
                    ESP_LOGE(TAG, "giving up on %u", s.did);
                }
                hold_publish();
            }
        }

        /* Every fifth turn, so a countdown on a dashboard moves without a
         * message every two seconds. */
        if (any && ++turns % 5 == 0) {
            hold_publish();
        }
    }
}

static bool ensure_task(char *err, size_t err_sz)
{
    if (!lock) {
        lock = xSemaphoreCreateMutex();
        if (!lock) {
            snprintf(err, err_sz, "out of memory");
            return false;
        }
    }
    if (!task_h &&
        xTaskCreate(hold_task, "hold", 4096, NULL, 4, &task_h) != pdPASS) {
        snprintf(err, err_sz, "cannot start the hold task");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* the grid setpoint                                                    */

bool grid_hold_start(uint16_t ecu, int16_t w, uint32_t seconds,
                     char *err, size_t err_sz)
{
    if (w > GRID_HOLD_MAX_W || w < -GRID_HOLD_MAX_W) {
        snprintf(err, err_sz, "%d W is beyond the %d W limit", w, GRID_HOLD_MAX_W);
        return false;
    }
    uint8_t b[6];
    b[0] = (uint8_t)((uint16_t)w & 0xFF);
    b[1] = (uint8_t)(((uint16_t)w >> 8) & 0xFF);
    put_i32(b + 2, HOLD_VALIDITY);
    grid_watts = w;
    return slot_start(SLOT_GRID, ecu, GRID_HOLD_DID, b, sizeof(b), seconds,
                      err, err_sz);
}

void grid_hold_stop(void) { slot_stop(SLOT_GRID); }

bool grid_hold_switch(bool on, char *err, size_t err_sz)
{
    if (!on) {
        grid_hold_stop();
        return true;
    }
    sys_cfg_t sys;
    sys_cfg_get(&sys);
    if (!sys.grid_ecu) {
        snprintf(err, err_sz, "no storage ECU configured for grid charging");
        return false;
    }
    /* The setting is positive and reads as "draw this much"; the datapoint
     * wants a negative number for the same thing. */
    return grid_hold_start(sys.grid_ecu, -(int16_t)sys.grid_watts,
                           (uint32_t)sys.grid_minutes * 60, err, err_sz);
}

void grid_hold_status(grid_hold_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!lock) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    const slot_t *s = &slots[SLOT_GRID];
    out->active = s->active;
    out->ecu = s->ecu;
    out->watts = grid_watts;
    int64_t left = s->deadline_us - esp_timer_get_time();
    out->remaining_s = (s->active && left > 0) ? (uint32_t)(left / 1000000) : 0;
    out->writes = s->writes;
    out->failures = s->fails;
    snprintf(out->last_error, sizeof(out->last_error), "%s", s->last_error);
    xSemaphoreGive(lock);
}

/* ------------------------------------------------------------------ */
/* the storage's own limits                                             */

static const struct { storage_mode_t mode; const char *name; } MODES[] = {
    { STORAGE_MODE_NORMAL,          "normal" },
    { STORAGE_MODE_IDLE,            "steht still" },
    { STORAGE_MODE_CHARGE_ONLY,     "nur laden" },
    { STORAGE_MODE_DISCHARGE_ONLY,  "nur entladen" },
};

const char *storage_mode_name(storage_mode_t mode)
{
    for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
        if (MODES[i].mode == mode) {
            return MODES[i].name;
        }
    }
    return "normal";
}

bool storage_mode_parse(const char *name, storage_mode_t *out)
{
    for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
        if (name && strcmp(MODES[i].name, name) == 0) {
            *out = MODES[i].mode;
            return true;
        }
    }
    return false;
}

bool storage_hold_start(uint16_t ecu, storage_mode_t mode, uint32_t seconds,
                        char *err, size_t err_sz)
{
    if (mode == STORAGE_MODE_NORMAL) {
        storage_mode = mode;
        slot_stop(SLOT_STORAGE);
        return true;
    }
    /* Only the extremes, so nothing here depends on knowing the scale. */
    int32_t charge = (mode == STORAGE_MODE_DISCHARGE_ONLY) ? 0 : HOLD_LIMIT_OPEN;
    int32_t discharge = (mode == STORAGE_MODE_CHARGE_ONLY) ? 0 : HOLD_LIMIT_OPEN;
    if (mode == STORAGE_MODE_IDLE) {
        charge = discharge = 0;
    }

    uint8_t b[12];
    put_i32(b + 0, charge);
    put_i32(b + 4, discharge);
    put_i32(b + 8, HOLD_VALIDITY);
    storage_mode = mode;
    return slot_start(SLOT_STORAGE, ecu, STORAGE_HOLD_DID, b, sizeof(b),
                      seconds, err, err_sz);
}

void storage_hold_status(storage_hold_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!lock) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    const slot_t *s = &slots[SLOT_STORAGE];
    out->active = s->active;
    out->mode = s->active ? storage_mode : STORAGE_MODE_NORMAL;
    int64_t left = s->deadline_us - esp_timer_get_time();
    out->remaining_s = (s->active && left > 0) ? (uint32_t)(left / 1000000) : 0;
    out->writes = s->writes;
    out->failures = s->fails;
    xSemaphoreGive(lock);
}

/* ------------------------------------------------------------------ */

void hold_publish(void)
{
    mqtt_cfg_t mq;
    mqtt_cfg_get(&mq);
    if (!mq.enabled || !mqtt_pub_connected()) {
        return;
    }
    sys_cfg_t sys;
    sys_cfg_get(&sys);

    grid_hold_status_t g;
    storage_hold_status_t s;
    grid_hold_status(&g);
    storage_hold_status(&s);

    char topic[CFG_TOPIC_MAX + 8];
    snprintf(topic, sizeof(topic), "%s/hold", mq.base_topic);

    char payload[288];
    snprintf(payload, sizeof(payload),
             "{\"active\": %s, \"power\": %u, \"minutes\": %u, "
             "\"remainingS\": %u, \"writes\": %u, \"failures\": %u, "
             "\"storage\": \"%s\", \"storageRemainingS\": %u}",
             g.active ? "true" : "false",
             (unsigned)sys.grid_watts, (unsigned)sys.grid_minutes,
             (unsigned)g.remaining_s, (unsigned)g.writes, (unsigned)g.failures,
             storage_mode_name(s.mode), (unsigned)s.remaining_s);
    mqtt_pub_raw(topic, payload, true);
}
