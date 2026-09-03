#include "poller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "can_port.h"
#include "collect.h"
#include "em380.h"
#include "ha_disco.h"
#include "o3e_codec.h"
#include "o3e_db.h"

static const char *TAG = "poll";

typedef struct {
    uint16_t     ecu;
    uint16_t     did;
    uint16_t     dlen;         /* response length seen during the scan */
    uint32_t     interval_s;
    uint32_t     next_due_ms;
    pub_mode_t   mode;
    bool         ha;
    char         topic[192];   /* fully expanded, computed once at load */
    o3e_node_t  *codec;
} point_t;

static point_t          *points;
static uint16_t          n_points;
static SemaphoreHandle_t points_lock;
static poll_stats_t      stats;
static volatile bool     running;
static volatile bool     paused;

static void free_points(void)
{
    for (uint16_t i = 0; i < n_points; i++) {
        o3e_codec_free(points[i].codec);
    }
    free(points);
    points = NULL;
    n_points = 0;
}

/* Resolve {device} the way open3e's dev_of_addr() does: the user's key for
 * that address, falling back to the hex address. */
static void device_name_of(uint16_t ecu, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "0x%03x", ecu);
    char *sys = app_config_read_file(CFG_SYSTEM_PATH);
    if (!sys) {
        return;
    }
    cJSON *root = cJSON_Parse(sys);
    free(sys);
    if (!root) {
        return;
    }
    const cJSON *dev;
    cJSON_ArrayForEach(dev, cJSON_GetObjectItem(root, "devices")) {
        const cJSON *addr = cJSON_GetObjectItem(dev, "addr");
        const cJSON *name = cJSON_GetObjectItem(dev, "name");
        if (cJSON_IsNumber(addr) && (uint16_t)addr->valuedouble == ecu &&
            cJSON_IsString(name)) {
            snprintf(out, out_sz, "%s", name->valuestring);
            break;
        }
    }
    cJSON_Delete(root);
}

bool poller_reload(void)
{
    char *raw = app_config_read_file(CFG_POINTS_PATH);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        xSemaphoreTake(points_lock, portMAX_DELAY);
        free_points();
        stats.active_points = 0;
        xSemaphoreGive(points_lock);
        ESP_LOGI(TAG, "no datapoints selected");
        return true;
    }

    int count = cJSON_GetArraySize(root);
    if (count > POLL_MAX_POINTS) {
        count = POLL_MAX_POINTS;
        ESP_LOGW(TAG, "selection truncated to %d datapoints", POLL_MAX_POINTS);
    }

    point_t *fresh = calloc((size_t)count ? (size_t)count : 1, sizeof(point_t));
    if (!fresh) {
        cJSON_Delete(root);
        return false;
    }

    uint16_t n = 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const cJSON *it;
    cJSON_ArrayForEach(it, root) {
        if (n >= count) {
            break;
        }
        /* The same file also holds the passively received energy-meter frames,
         * which carry a CAN-ID instead of an ECU and DID. Reading those fields
         * from such an entry used to dereference NULL and panic on boot. */
        if (!sel_is_datapoint(it) || !sel_enabled(it)) {
            continue;
        }
        point_t *p = &fresh[n];
        p->ecu = sel_u16(it, "ecu", 0);
        p->did = sel_u16(it, "did", 0);
        p->dlen = sel_u16(it, "len", 0);
        uint32_t iv = sel_u32(it, "interval", 60);
        p->interval_s = iv >= 1 ? iv : 60;
        const char *mode = sel_str(it, "mode");
        p->mode = (mode && strcmp(mode, "flat") == 0) ? PUB_MODE_FLAT : PUB_MODE_JSON;
        p->ha = sel_bool(it, "ha", false);

        /* Compiling here rather than per poll is the whole point of the
         * two-stage codec: the polling loop never parses JSON or reads flash. */
        char *json = o3e_db_json(p->did, p->dlen);
        p->codec = json ? o3e_codec_compile(json) : NULL;
        free(json);
        if (!p->codec) {
            ESP_LOGW(TAG, "DID %u is not in the database, skipping", p->did);
            continue;
        }

        char device[48];
        device_name_of(p->ecu, device, sizeof(device));
        mqtt_pub_topic(p->topic, sizeof(p->topic), p->ecu, p->did,
                       p->codec->id, device, sel_str(it, "topic"));

        /* Stagger the first read so a large selection does not fire every
         * request in the same tick. */
        p->next_due_ms = now + (uint32_t)n * 200;
        n++;
    }
    cJSON_Delete(root);

    xSemaphoreTake(points_lock, portMAX_DELAY);
    free_points();
    points = fresh;
    n_points = n;
    stats.active_points = n;
    xSemaphoreGive(points_lock);

    ESP_LOGI(TAG, "%u datapoints active", n);
    /* The selection file also carries the energy-meter frames. */
    em380_reload();
    collect_reload();
    ha_disco_publish_all();
    return true;
}

/* The 4 KiB ISO-TP receive buffer lives on the heap, not on the polling task's
 * stack.
 *
 * On the stack it left almost nothing for the rest of the call chain -- and
 * that chain ends in o3e_codec_decode_json(), which recurses through nested
 * datapoint types. The result was a stack overflow in this task, which
 * FreeRTOS reports at the next context switch rather than at the call that
 * caused it. The scan task had the same buffer and was fixed earlier; this one
 * was missed. */
static uint8_t *poll_buf;

static void poll_one(point_t *p)
{
    size_t n = 0;
    if (!poll_buf) {
        return;
    }
    uds_result_t r = can_read_did(p->ecu, p->did, poll_buf, ISOTP_MAX_PAYLOAD,
                                  &n, UDS_P2_MS);
    stats.polls++;
    if (r.err != UDS_OK) {
        stats.failures++;
        ESP_LOGD(TAG, "0x%03X.%u: %s", p->ecu, p->did, uds_strerror(r));
        return;
    }
    if (mqtt_pub_value(p->topic, p->codec, poll_buf, n, p->mode)) {
        stats.published++;
    }
}

static void poll_task(void *arg)
{
    (void)arg;
    while (running) {
        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        /* The lock is held only while picking the next due point; the CAN
         * request itself runs unlocked so a web UI save is never blocked
         * behind a bus timeout. */
        point_t due;
        bool have = false;
        xSemaphoreTake(points_lock, portMAX_DELAY);
        for (uint16_t i = 0; i < n_points; i++) {
            if ((int32_t)(now - points[i].next_due_ms) >= 0) {
                points[i].next_due_ms = now + points[i].interval_s * 1000;
                due = points[i];
                have = true;
                break;
            }
        }
        xSemaphoreGive(points_lock);

        if (have) {
            poll_one(&due);
            stats.last_poll_ms = now;
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    vTaskDelete(NULL);
}

bool poller_start(void)
{
    if (!points_lock) {
        points_lock = xSemaphoreCreateMutex();
        if (!points_lock) {
            return false;
        }
    }
    if (!poll_buf) {
        poll_buf = malloc(ISOTP_MAX_PAYLOAD);
        if (!poll_buf) {
            ESP_LOGE(TAG, "no memory for the receive buffer");
            return false;
        }
    }
    poller_reload();
    running = true;
    /* The receive buffer is on the heap now, so this only has to cover the
     * call chain: UDS, ISO-TP, and a recursive codec decode. */
    return xTaskCreate(poll_task, "poll", 6144, NULL, 4, NULL) == pdPASS;
}

void poller_stop(void)
{
    running = false;
}

void poller_pause(bool p)
{
    paused = p;
    ESP_LOGI(TAG, "cyclic reading %s", p ? "paused" : "resumed");
}

bool poller_is_paused(void) { return paused; }

void poller_refresh(void)
{
    if (!points_lock) {
        return;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    xSemaphoreTake(points_lock, portMAX_DELAY);
    for (uint16_t i = 0; i < n_points; i++) {
        /* Staggered, so a large selection does not fire every request in the
         * same tick. */
        points[i].next_due_ms = now + (uint32_t)i * 200;
    }
    xSemaphoreGive(points_lock);
    ESP_LOGI(TAG, "%u datapoints scheduled for an immediate read", n_points);
}

void poller_stats(poll_stats_t *out) { *out = stats; }

char *poller_read_now(uint16_t ecu, uint16_t did, char *err, size_t err_sz)
{
    uint8_t *buf = malloc(ISOTP_MAX_PAYLOAD);
    if (!buf) {
        snprintf(err, err_sz, "out of memory");
        return NULL;
    }
    size_t n = 0;
    uds_result_t r = can_read_did(ecu, did, buf, ISOTP_MAX_PAYLOAD, &n, UDS_P2_MS);
    if (r.err != UDS_OK) {
        snprintf(err, err_sz, "%s", uds_strerror(r));
        free(buf);
        return NULL;
    }

    /* The response length decides which variant of the codec applies. */
    char *json = o3e_db_json(did, (uint16_t)n);
    o3e_node_t *node = json ? o3e_codec_compile(json) : NULL;
    free(json);
    if (!node) {
        /* Undocumented datapoint: show the bytes it actually returned. That is
         * both the proof it answered and the raw material for adding it to the
         * open3e database. */
        node = o3e_codec_raw((uint16_t)n, "Raw");
    }
    if (!node) {
        snprintf(err, err_sz, "out of memory");
        free(buf);
        return NULL;
    }
    char *out = o3e_codec_decode_json(node, buf, n);
    o3e_codec_free(node);
    free(buf);
    if (!out) {
        snprintf(err, err_sz, "could not decode the response");
    }
    return out;
}

/* Add the fields `want` does not mention, taken from the datapoint as it reads
 * right now. Only the top level is merged: a nested object given by the caller
 * is taken as complete, because a half-specified sub-structure is more likely a
 * mistake than an intention. */
static bool merge_current(const o3e_node_t *node, const uint8_t *raw, size_t n,
                          cJSON *want, char *err, size_t err_sz)
{
    if (!cJSON_IsObject(want)) {
        return true;   /* a scalar datapoint has nothing to merge */
    }
    char *now = o3e_codec_decode_json(node, raw, n);
    if (!now) {
        snprintf(err, err_sz, "cannot read the datapoint's current value");
        return false;
    }
    cJSON *cur = cJSON_Parse(now);
    free(now);
    if (!cur) {
        snprintf(err, err_sz, "the datapoint's current value is not readable");
        return false;
    }
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, cur) {
        if (it->string && !cJSON_GetObjectItemCaseSensitive(want, it->string)) {
            cJSON_AddItemToObject(want, it->string, cJSON_Duplicate(it, true));
        }
    }
    cJSON_Delete(cur);
    return true;
}

bool poller_write_now(uint16_t ecu, uint16_t did, const char *value_json,
                      bool force, char *err, size_t err_sz)
{
    sys_cfg_t sys;
    sys_cfg_get(&sys);
    if (!sys.write_enabled) {
        snprintf(err, err_sz, "writing is disabled in the system settings");
        return false;
    }

    /* Read first: the response length selects the codec variant, and it also
     * confirms the datapoint exists on this ECU before anything is written. */
    uint8_t *buf = malloc(ISOTP_MAX_PAYLOAD);
    if (!buf) {
        snprintf(err, err_sz, "out of memory");
        return false;
    }
    size_t n = 0;
    uds_result_t rr = can_read_did(ecu, did, buf, ISOTP_MAX_PAYLOAD, &n, UDS_P2_MS);
    if (rr.err != UDS_OK) {
        free(buf);
        snprintf(err, err_sz, "cannot read the datapoint first: %s", uds_strerror(rr));
        return false;
    }

    char *json = o3e_db_json(did, (uint16_t)n);
    o3e_node_t *node = json ? o3e_codec_compile(json) : NULL;
    free(json);
    if (!node) {
        /* No raw fallback on the write path: sending bytes whose meaning we do
         * not know to a heat pump is exactly the mistake worth preventing. */
        free(buf);
        snprintf(err, err_sz, "DID %u is not in the open3e database, so its "
                 "fields are unknown and it cannot be written", did);
        return false;
    }
    /* The database's own access flag is the second gate: the global switch says
     * writing is allowed at all, this says the datapoint accepts it. */
    if (node->acc != O3E_ACC_RW) {
        if (!force) {
            snprintf(err, err_sz, "%s is read-only in the open3e database",
                     node->id ? node->id : "this datapoint");
            o3e_codec_free(node);
            free(buf);
            return false;
        }
        ESP_LOGW(TAG, "forcing a write to 0x%03X.%u (%s), which the database "
                 "marks read-only", ecu, did, node->id ? node->id : "?");
    }

    cJSON *value = cJSON_Parse(value_json);
    if (!value) {
        snprintf(err, err_sz, "value is not valid JSON");
        o3e_codec_free(node);
        free(buf);
        return false;
    }

    /* Fill in whatever the caller left out from the datapoint's current value.
     *
     * Encoding a complex type needs every field, so without this a caller has
     * to restate fields it does not care about -- including ones the database
     * only knows as "Unknown1". That makes a single-value control impossible:
     * a Home Assistant select can send the ventilation stage and nothing else.
     * The read above already happened to pick the codec variant, so merging
     * costs no extra traffic. */
    if (!merge_current(node, buf, n, value, err, err_sz)) {
        cJSON_Delete(value);
        o3e_codec_free(node);
        free(buf);
        return false;
    }
    free(buf);

    uint8_t payload[UDS_MAX_WRITE];
    bool ok = o3e_codec_encode(node, value, payload, sizeof(payload), err, err_sz);
    cJSON_Delete(value);
    uint16_t len = node->len;
    o3e_codec_free(node);
    if (!ok) {
        return false;
    }

    uds_result_t wr = can_write_did(ecu, did, payload, len, UDS_P2_MS);
    if (wr.err != UDS_OK) {
        snprintf(err, err_sz, "%s", uds_strerror(wr));
        return false;
    }
    ESP_LOGI(TAG, "wrote 0x%03X.%u", ecu, did);
    return true;
}
