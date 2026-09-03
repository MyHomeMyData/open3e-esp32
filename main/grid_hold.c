#include "grid_hold.h"

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

static const char *TAG = "grid";

/* Stop rather than hammer: if the storage has begun refusing the write, the
 * next fifty attempts will not go better, and a hold that cannot take effect
 * should end loudly instead of running as a lie on the status page. */
#define GRID_HOLD_MAX_FAILS 5

static SemaphoreHandle_t lock;
static TaskHandle_t      task_h;

static volatile bool     active;
static uint16_t          ecu;
static int16_t           watts;
static int64_t           deadline_us;
static uint32_t          n_writes, n_fails;
static char              last_error[96];

/* DID 2188 is a RawCodec, so the value goes in as a hex string -- quotes and
 * all, because that is a JSON string. */
static void payload(char *out, size_t out_sz, int16_t w)
{
    uint16_t u = (uint16_t)w;
    snprintf(out, out_sz, "\"%02x%02x%02x%02x%02x%02x\"",
             u & 0xFF, (u >> 8) & 0xFF,
             GRID_HOLD_VALIDITY & 0xFF, (GRID_HOLD_VALIDITY >> 8) & 0xFF,
             (GRID_HOLD_VALIDITY >> 16) & 0xFF, (GRID_HOLD_VALIDITY >> 24) & 0xFF);
}

static void grid_hold_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(GRID_HOLD_PERIOD_MS));
        if (!active) {
            continue;
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        bool run = active && esp_timer_get_time() < deadline_us;
        uint16_t e = ecu;
        int16_t w = watts;
        xSemaphoreGive(lock);

        if (!run) {
            if (active) {
                ESP_LOGI(TAG, "hold finished after %u writes", (unsigned)n_writes);
                active = false;
                grid_hold_publish();
            }
            continue;
        }

        char json[32];
        payload(json, sizeof(json), w);
        char err[96] = "";
        /* Forced: the database marks 2188 read-only, which the device does
         * not -- it acknowledges the write and acts on it. */
        if (poller_write_now(e, GRID_HOLD_DID, json, true, err, sizeof(err))) {
            n_writes++;
            /* Every fifth write, so the remaining time on a dashboard moves
               without a message every two seconds. */
            if (n_writes % 5 == 0) {
                grid_hold_publish();
            }
        } else {
            n_fails++;
            snprintf(last_error, sizeof(last_error), "%s", err);
            ESP_LOGW(TAG, "write failed (%u): %s", (unsigned)n_fails, err);
            if (n_fails >= GRID_HOLD_MAX_FAILS) {
                ESP_LOGE(TAG, "giving up after %u failures", (unsigned)n_fails);
                active = false;
            }
            grid_hold_publish();
        }
    }
}

bool grid_hold_start(uint16_t e, int16_t w, uint32_t seconds,
                     char *err, size_t err_sz)
{
    if (w > GRID_HOLD_MAX_W || w < -GRID_HOLD_MAX_W) {
        snprintf(err, err_sz, "%d W is beyond the %d W limit", w, GRID_HOLD_MAX_W);
        return false;
    }
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
    if (!lock) {
        lock = xSemaphoreCreateMutex();
        if (!lock) {
            snprintf(err, err_sz, "out of memory");
            return false;
        }
    }
    if (!task_h &&
        xTaskCreate(grid_hold_task, "gridhold", 4096, NULL, 4, &task_h) != pdPASS) {
        snprintf(err, err_sz, "cannot start the hold task");
        return false;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    ecu = e;
    watts = w;
    deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    n_writes = n_fails = 0;
    last_error[0] = '\0';
    active = true;
    xSemaphoreGive(lock);

    ESP_LOGW(TAG, "holding 0x%03X.%u at %d W for %u s", e, GRID_HOLD_DID, w,
             (unsigned)seconds);
    grid_hold_publish();
    return true;
}

void grid_hold_stop(void)
{
    if (active) {
        ESP_LOGI(TAG, "hold stopped after %u writes", (unsigned)n_writes);
    }
    active = false;
    grid_hold_publish();
}

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

void grid_hold_publish(void)
{
    mqtt_cfg_t mq;
    mqtt_cfg_get(&mq);
    if (!mq.enabled || !mqtt_pub_connected()) {
        return;
    }
    sys_cfg_t sys;
    sys_cfg_get(&sys);

    grid_hold_status_t st;
    grid_hold_status(&st);

    char topic[CFG_TOPIC_MAX + 8];
    snprintf(topic, sizeof(topic), "%s/grid", mq.base_topic);

    char payload[224];
    snprintf(payload, sizeof(payload),
             "{\"active\": %s, \"power\": %u, \"minutes\": %u, "
             "\"remainingS\": %u, \"writes\": %u, \"failures\": %u}",
             st.active ? "true" : "false",
             (unsigned)sys.grid_watts, (unsigned)sys.grid_minutes,
             (unsigned)st.remaining_s, (unsigned)st.writes,
             (unsigned)st.failures);
    mqtt_pub_raw(topic, payload, true);
}

void grid_hold_status(grid_hold_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!lock) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    out->active = active;
    out->ecu = ecu;
    out->watts = watts;
    int64_t left = deadline_us - esp_timer_get_time();
    out->remaining_s = (active && left > 0) ? (uint32_t)(left / 1000000) : 0;
    out->writes = n_writes;
    out->failures = n_fails;
    snprintf(out->last_error, sizeof(out->last_error), "%s", last_error);
    xSemaphoreGive(lock);
}
