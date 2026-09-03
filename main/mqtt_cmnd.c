#include "mqtt_cmnd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_config.h"
#include "mqtt_pub.h"
#include "o3e_db.h"
#include "poller.h"

static const char *TAG = "cmnd";

static void reply_error(const char *fmt, ...)
{
    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);
    char topic[CFG_TOPIC_MAX + 8];
    snprintf(topic, sizeof(topic), "%s/ERR", cfg.base_topic);

    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    ESP_LOGW(TAG, "%s", msg);
    mqtt_pub_raw(topic, msg, false);
}

/* open3e accepts the ECU either as "0x680" or as a plain number, and omits it
 * entirely for single-ECU systems. */
static uint16_t parse_addr(const cJSON *addr, uint16_t dflt)
{
    if (cJSON_IsNumber(addr)) {
        return (uint16_t)addr->valuedouble;
    }
    if (cJSON_IsString(addr) && addr->valuestring) {
        return (uint16_t)strtol(addr->valuestring, NULL, 0);
    }
    return dflt;
}

/* Falls back to the first ECU the scan found, which is what open3e does when
 * no address is given. */
static uint16_t default_ecu(void)
{
    uint16_t addr = 0x680;
    char *sys = app_config_read_file(CFG_SYSTEM_PATH);
    if (!sys) {
        return addr;
    }
    cJSON *root = cJSON_Parse(sys);
    free(sys);
    if (root) {
        const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "devices"), 0);
        const cJSON *a = first ? cJSON_GetObjectItem(first, "addr") : NULL;
        if (cJSON_IsNumber(a)) {
            addr = (uint16_t)a->valuedouble;
        }
        cJSON_Delete(root);
    }
    return addr;
}

static void do_read(uint16_t ecu, const cJSON *data)
{
    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);

    const cJSON *it;
    cJSON_ArrayForEach(it, data) {
        if (!cJSON_IsNumber(it)) {
            continue;
        }
        uint16_t did = (uint16_t)it->valuedouble;
        char err[128] = "";
        char *json = poller_read_now(ecu, did, err, sizeof(err));
        if (!json) {
            reply_error("read 0x%03X.%u failed: %s", ecu, did, err);
            continue;
        }
        /* Replies land on the same topic a scheduled poll would use, so a
         * subscriber does not need to know which triggered it. */
        const o3e_dp_entry_t *e = o3e_db_find(did);
        char topic[256];
        mqtt_pub_topic(topic, sizeof(topic), ecu, did,
                       e ? o3e_db_name(e) : "", "", NULL);
        mqtt_pub_raw(topic, json, false);
        free(json);
    }
}

static void do_write(uint16_t ecu, const cJSON *data)
{
    if (!cJSON_IsObject(data)) {
        reply_error("write expects an object of did -> value");
        return;
    }
    const cJSON *it;
    cJSON_ArrayForEach(it, data) {
        if (!it->string) {
            continue;
        }
        uint16_t did = (uint16_t)strtol(it->string, NULL, 0);
        char *value = cJSON_PrintUnformatted(it);
        char err[192] = "";
        if (!value) {
            continue;
        }
        /* Never forced over MQTT: the command topic is where automations
           live, and one of those repeating a mistake is worse than a person
           making it once. */
        if (!poller_write_now(ecu, did, value, false, err, sizeof(err))) {
            reply_error("write 0x%03X.%u failed: %s", ecu, did, err);
        } else {
            ESP_LOGI(TAG, "wrote 0x%03X.%u via MQTT", ecu, did);
        }
        free(value);
    }
}

/* Commands run on their own task.
 *
 * A read or write can wait seconds for the CAN bus; doing that in the MQTT
 * event callback would stall the client's own task, stop its keepalive and
 * drop the connection under exactly the load it exists to handle. */
#define CMND_MAX_PAYLOAD 1024

typedef struct {
    char payload[CMND_MAX_PAYLOAD];
} cmnd_job_t;

static QueueHandle_t cmnd_q;

static void execute(const char *payload);

static void cmnd_task(void *arg)
{
    (void)arg;
    cmnd_job_t job;
    for (;;) {
        if (xQueueReceive(cmnd_q, &job, portMAX_DELAY) == pdTRUE) {
            execute(job.payload);
        }
    }
}

void mqtt_cmnd_dispatch(const char *topic, int topic_len,
                        const char *data, int data_len)
{
    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);
    if ((int)strlen(cfg.cmnd_topic) != topic_len ||
        strncmp(cfg.cmnd_topic, topic, (size_t)topic_len) != 0) {
        return;
    }
    if (data_len <= 0 || data_len >= CMND_MAX_PAYLOAD) {
        reply_error("command payload must be 1..%d bytes", CMND_MAX_PAYLOAD - 1);
        return;
    }

    if (!cmnd_q) {
        cmnd_q = xQueueCreate(4, sizeof(cmnd_job_t));
        if (!cmnd_q) {
            return;
        }
        /* 6 KiB: decoding recurses through the codec tree and parses JSON. */
        if (xTaskCreate(cmnd_task, "mqttcmd", 6144, NULL, 4, NULL) != pdPASS) {
            vQueueDelete(cmnd_q);
            cmnd_q = NULL;
            return;
        }
    }

    cmnd_job_t job;
    memcpy(job.payload, data, (size_t)data_len);
    job.payload[data_len] = '\0';
    if (xQueueSend(cmnd_q, &job, 0) != pdTRUE) {
        reply_error("busy: a previous command is still running");
    }
}

static void execute(const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        reply_error("bad payload: not valid JSON");
        return;
    }

    const cJSON *mode = cJSON_GetObjectItem(root, "mode");
    const cJSON *jdata = cJSON_GetObjectItem(root, "data");
    uint16_t ecu = parse_addr(cJSON_GetObjectItem(root, "addr"), default_ecu());

    if (!cJSON_IsString(mode)) {
        reply_error("bad payload: \"mode\" is required (read or write)");
    } else if (strcmp(mode->valuestring, "read") == 0) {
        do_read(ecu, jdata);
    } else if (strcmp(mode->valuestring, "write") == 0) {
        do_write(ecu, jdata);
    } else if (strcmp(mode->valuestring, "read-raw") == 0 ||
               strcmp(mode->valuestring, "write-raw") == 0) {
        /* open3e's raw modes exchange undecoded hex. Not implemented here:
         * the point of this gateway is the decoded view, and a raw write is
         * the easiest way to put a heat pump into a state nobody intended. */
        reply_error("mode '%s' is not supported by this gateway", mode->valuestring);
    } else {
        reply_error("bad mode '%s'; supported: read, write", mode->valuestring);
    }
    cJSON_Delete(root);
}
