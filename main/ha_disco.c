#include "ha_disco.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "app_config.h"
#include "mqtt_pub.h"
#include "o3e_codec.h"
#include "o3e_db.h"
#include "o3e_json.h"

static const char *TAG = "ha";

/* Map the open3e unit string onto a Home Assistant device class. Only units
 * that actually occur in the database are listed; anything else is published
 * without a class, which HA renders fine. */
static const char *device_class_for(const char *unit)
{
    if (!unit || !unit[0]) {
        return NULL;
    }
    if (strcmp(unit, "°C") == 0)  return "temperature";
    if (strcmp(unit, "hPa") == 0)      return "pressure";
    if (strcmp(unit, "bar") == 0)      return "pressure";
    if (strcmp(unit, "kWh") == 0)      return "energy";
    if (strcmp(unit, "Wh") == 0)       return "energy";
    if (strcmp(unit, "W") == 0)        return "power";
    if (strcmp(unit, "kW") == 0)       return "power";
    if (strcmp(unit, "V") == 0)        return "voltage";
    if (strcmp(unit, "A") == 0)        return "current";
    if (strcmp(unit, "Hz") == 0)       return "frequency";
    if (strcmp(unit, "h") == 0)        return "duration";
    if (strcmp(unit, "l/h") == 0)      return "volume_flow_rate";
    return NULL;
}

static void device_id(char *out, size_t out_sz)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_sz, "open3e_%02x%02x%02x", mac[3], mac[4], mac[5]);
}

/* One config topic per published scalar. In JSON mode a value_template picks
 * the field out of the payload; in flat mode the field already has its own
 * topic and no template is needed. */
/* What the last walk actually put on the broker. Kept apart because
 "published for 19 datapoints" says nothing about whether any of them
 became operable, and a control that never appears looks exactly like a
 datapoint that is read-only. */
static int n_sensors, n_controls;

/* Home Assistant keeps long-term statistics, and admits a sensor to the energy
 * dashboard, only when the state class suits the device class. An energy
 * reading marked `measurement` is rejected outright -- that is what an entity
 * page saying "no statistics found" is reporting.
 *
 * The rolling windows are the exception. Past7Days, PastMonth and PastYear
 * fall as well as rise, and `total_increasing` reads every fall as a counter
 * reset and books the entire new value as fresh consumption. Those get no
 * state class rather than a wrong one: no statistics is a gap, invented
 * kilowatt-hours are a lie. */
static const char *state_class_for(const char *device_class, const char *field)
{
    bool counter = strcmp(device_class, "energy") == 0
                || strcmp(device_class, "duration") == 0;
    if (!counter) {
        return "measurement";
    }
    if (field && strncmp(field, "Past", 4) == 0) {
        return NULL;
    }
    return "total_increasing";
}

static void publish_entity(const mqtt_cfg_t *cfg, const char *dev_id,
                           const char *state_topic, const char *object_id,
                           const char *name, const o3e_node_t *leaf,
                           const char *value_template, bool clear)
{
    char topic[320];
    snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config",
             cfg->ha_prefix, dev_id, object_id);
    if (clear) {
        /* An empty retained payload is how Home Assistant is told an entity is
         * gone. It has to target the exact same topic the entity was published
         * on, which is why removal walks the codec tree too. */
        mqtt_pub_raw(topic, "", true);
        n_sensors++;
        return;
    }

    o3e_buf_t b;
    o3e_buf_init(&b);

    o3e_buf_adds(&b, "{\"name\": ");
    o3e_buf_add_json_str(&b, name);
    o3e_buf_adds(&b, ", \"state_topic\": ");
    o3e_buf_add_json_str(&b, state_topic);
    o3e_buf_adds(&b, ", \"unique_id\": ");
    char uid[160];
    snprintf(uid, sizeof(uid), "%s_%s", dev_id, object_id);
    o3e_buf_add_json_str(&b, uid);

    if (value_template) {
        o3e_buf_adds(&b, ", \"value_template\": ");
        o3e_buf_add_json_str(&b, value_template);
    }
    if (leaf && leaf->unit && leaf->unit[0]) {
        o3e_buf_adds(&b, ", \"unit_of_measurement\": ");
        o3e_buf_add_json_str(&b, leaf->unit);
        const char *dc = device_class_for(leaf->unit);
        if (dc) {
            o3e_buf_adds(&b, ", \"device_class\": ");
            o3e_buf_add_json_str(&b, dc);
            const char *sc = state_class_for(dc, leaf->id);
            if (sc) {
                o3e_buf_adds(&b, ", \"state_class\": ");
                o3e_buf_add_json_str(&b, sc);
            }
        }
    }

    char lwt[CFG_TOPIC_MAX + 8];
    snprintf(lwt, sizeof(lwt), "%s/LWT", cfg->base_topic);
    o3e_buf_adds(&b, ", \"availability_topic\": ");
    o3e_buf_add_json_str(&b, lwt);
    o3e_buf_adds(&b, ", \"payload_available\": \"online\""
                     ", \"payload_not_available\": \"offline\"");

    o3e_buf_adds(&b, ", \"device\": {\"identifiers\": [");
    o3e_buf_add_json_str(&b, dev_id);
    o3e_buf_adds(&b, "], \"manufacturer\": \"Viessmann\", \"model\": \"E3 via open3e\""
                     ", \"name\": ");
    /* The base topic, because one household can run more than one of these --
     * one on the storage bus, one on the ventilation unit -- and two devices
     * both called "open3e Gateway" are told apart only by a MAC address. */
    o3e_buf_add_json_str(&b, cfg->base_topic[0] ? cfg->base_topic : "open3e Gateway");
    o3e_buf_adds(&b, ", \"sw_version\": ");
    o3e_buf_add_json_str(&b, o3e_db_version());
    o3e_buf_adds(&b, "}}");

    if (!b.oom && b.buf) {
        mqtt_pub_raw(topic, b.buf, true);
        n_sensors++;
    }
    o3e_buf_free(&b);
}

/* A writable datapoint also gets something to operate it.
 *
 * The command goes to the gateway's open3e-compatible command topic, which
 * takes an object of DID to value. Only one field of that value is set here --
 * a select can send a ventilation stage and nothing else -- and the firmware
 * fills the rest in from the datapoint's current reading before encoding.
 * Without that merge, encoding would fail on the first field the control does
 * not mention, including ones the database only knows as "Unknown1".
 *
 * Limited to the first level: a control for a field nested two deep would have
 * to send a nested object, and a half-specified sub-structure is more likely a
 * mistake than an intention.
 */
static void publish_control(const mqtt_cfg_t *cfg, const char *dev_id,
                            const char *state_topic, const char *object_id,
                            const char *name, const o3e_node_t *leaf,
                            const char *value_template,
                            uint16_t ecu, uint16_t did, const char *path,
                            bool writable, bool clear)
{
    if (!writable || !cfg->cmnd_topic[0]) {
        return;
    }
    /* path is "" for a scalar datapoint and "_Field" one level down. Anything
     * with a second separator is nested deeper than a control can address. */
    const char *field = path[0] ? path + 1 : NULL;
    if (field && strchr(field, '_')) {
        return;
    }

    const char *component = NULL;
    if (leaf->kind == O3E_K_ENUM && leaf->list_name) {
        component = "select";
    } else if (leaf->kind == O3E_K_INT || leaf->kind == O3E_K_BYTEVAL) {
        /* Only these two: their byte width bounds the value, so the control
         * can offer the range the datapoint actually accepts. A float has no
         * such bound, and text, raw bytes and timestamps have no sensible
         * control at all. */
        component = "number";
    } else {
        return;
    }

    char topic[352];
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s_set/config",
             cfg->ha_prefix, component, dev_id, object_id);
    if (clear) {
        mqtt_pub_raw(topic, "", true);
        return;
    }

    /* The value the command carries: a name for a select, a number otherwise.
     * The encoder resolves an enum by its text, which is exactly the option
     * Home Assistant sends back. */
    char cmd[320];
    if (field) {
        snprintf(cmd, sizeof(cmd),
                 "{\"mode\": \"write\", \"addr\": \"0x%03X\", \"data\": "
                 "{\"%u\": {\"%s\": %s}}}",
                 ecu, did, field,
                 component[0] == 's' ? "\"{{ value }}\"" : "{{ value }}");
    } else {
        snprintf(cmd, sizeof(cmd),
                 "{\"mode\": \"write\", \"addr\": \"0x%03X\", \"data\": "
                 "{\"%u\": %s}}",
                 ecu, did,
                 component[0] == 's' ? "\"{{ value }}\"" : "{{ value }}");
    }

    o3e_buf_t b;
    o3e_buf_init(&b);
    o3e_buf_adds(&b, "{\"name\": ");
    o3e_buf_add_json_str(&b, name);
    o3e_buf_adds(&b, ", \"state_topic\": ");
    o3e_buf_add_json_str(&b, state_topic);
    if (value_template) {
        o3e_buf_adds(&b, ", \"value_template\": ");
        o3e_buf_add_json_str(&b, value_template);
    }
    o3e_buf_adds(&b, ", \"command_topic\": ");
    o3e_buf_add_json_str(&b, cfg->cmnd_topic);
    o3e_buf_adds(&b, ", \"command_template\": ");
    o3e_buf_add_json_str(&b, cmd);

    char uid[176];
    snprintf(uid, sizeof(uid), "%s_%s_set", dev_id, object_id);
    o3e_buf_adds(&b, ", \"unique_id\": ");
    o3e_buf_add_json_str(&b, uid);

    if (component[0] == 's') {
        o3e_buf_adds(&b, ", \"options\": [");
        size_t count = o3e_db_enum_count(leaf->list_name);
        for (size_t i = 0, written = 0; i < count; i++) {
            int32_t val;
            const char *text = NULL;
            if (!o3e_db_enum_at(leaf->list_name, i, &val, &text) || !text) {
                continue;
            }
            if (written++) {
                o3e_buf_adds(&b, ", ");
            }
            o3e_buf_add_json_str(&b, text);
        }
        o3e_buf_adds(&b, "]");
    } else {
        /* Bounds from the field's own width and sign. Home Assistant defaults
         * a number to 0..100, which would silently forbid most of the range a
         * datapoint actually accepts. */
        double lo, hi;
        double span = 1.0;
        for (uint16_t i = 0; i < leaf->len && i < 4; i++) {
            span *= 256.0;
        }
        if (leaf->signd) {
            hi = span / 2.0 - 1.0;
            lo = -span / 2.0;
        } else {
            hi = span - 1.0;
            lo = 0.0;
        }
        double scale = leaf->scale > 0.0 ? leaf->scale : 1.0;
        char nums[128];
        snprintf(nums, sizeof(nums),
                 ", \"min\": %.4g, \"max\": %.4g, \"step\": %.4g, \"mode\": \"box\"",
                 lo / scale, hi / scale, 1.0 / scale);
        o3e_buf_adds(&b, nums);
        if (leaf->unit && leaf->unit[0]) {
            o3e_buf_adds(&b, ", \"unit_of_measurement\": ");
            o3e_buf_add_json_str(&b, leaf->unit);
        }
    }

    char lwt[CFG_TOPIC_MAX + 8];
    snprintf(lwt, sizeof(lwt), "%s/LWT", cfg->base_topic);
    o3e_buf_adds(&b, ", \"availability_topic\": ");
    o3e_buf_add_json_str(&b, lwt);
    o3e_buf_adds(&b, ", \"payload_available\": \"online\""
                     ", \"payload_not_available\": \"offline\"");
    o3e_buf_adds(&b, ", \"device\": {\"identifiers\": [");
    o3e_buf_add_json_str(&b, dev_id);
    o3e_buf_adds(&b, "]}}");

    if (!b.oom && b.buf) {
        mqtt_pub_raw(topic, b.buf, true);
        n_controls++;
    }
    o3e_buf_free(&b);
}

/* Announce one scalar leaf: a sensor, and a control if it can be written. */
static void publish_leaf(const mqtt_cfg_t *cfg, const char *dev_id,
                         const char *state_topic, const o3e_node_t *n,
                         const char *did_name, uint16_t did, uint16_t ecu,
                         const char *path, const char *tmpl,
                         bool flat, bool writable, bool clear)
{
        char object_id[192];
        snprintf(object_id, sizeof(object_id), "%03x_%u%s", ecu, did, path);
        char name[192];
        snprintf(name, sizeof(name), "%s%s", did_name, path[0] ? path : "");
        for (char *p = name; *p; p++) {
            if (*p == '_') {
                *p = ' ';
            }
        }

        /* An enum leaf decodes to {"ID": n, "Text": "..."}, so it is not a
           scalar on the wire: flat mode gives it two topics and JSON mode a
           nested object. The Text is the half a person wants, and it is also
           what the encoder accepts back, so both the sensor and the control
           point at it. */
        bool is_enum = (n->kind == O3E_K_ENUM);

        if (flat) {
            /* Flat mode gives each leaf its own topic; path segments are
               separated by '/' there rather than '_'. */
            char leaf_topic[384];
            char sub[192];
            snprintf(sub, sizeof(sub), "%s", path);
            for (char *p = sub; *p; p++) {
                if (*p == '_') {
                    *p = '/';
                }
            }
            snprintf(leaf_topic, sizeof(leaf_topic), "%s%s%s", state_topic, sub,
                     is_enum ? "/Text" : "");
            publish_entity(cfg, dev_id, leaf_topic, object_id, name, n, NULL, clear);
            publish_control(cfg, dev_id, leaf_topic, object_id, name, n, NULL,
                            ecu, did, path, writable, clear);
        } else {
            char t[256];
            snprintf(t, sizeof(t), "{{ value_json%s%s }}", tmpl,
                     is_enum ? ".Text" : "");
            const char *vt = (tmpl[0] || is_enum) ? t : "{{ value }}";
            publish_entity(cfg, dev_id, state_topic, object_id, name, n, vt, clear);
            publish_control(cfg, dev_id, state_topic, object_id, name, n, vt,
                            ecu, did, path, writable, clear);
        }
}

/* Walk the codec tree and announce every scalar leaf, since that is what
 * actually reaches a topic. A complex datapoint like FlowTemperatureSensor
 * therefore becomes Actual/Minimum/Maximum/Average entities rather than one
 * opaque one. */
static void walk_leaves(const mqtt_cfg_t *cfg, const char *dev_id,
                        const char *state_topic, const o3e_node_t *n,
                        const char *did_name, uint16_t did, uint16_t ecu,
                        char *path, size_t path_sz, char *tmpl, size_t tmpl_sz,
                        bool flat, bool writable, bool clear, int depth)
{
    if (depth > 4) {
        return;   /* deeply nested lists are not useful as entities */
    }

    switch (n->kind) {
    case O3E_K_COMPLEX:
    case O3E_K_SWITCH:
        for (uint16_t i = 0; i < n->n_kids; i++) {
            const o3e_node_t *k = n->kids[i];
            if (k->kind == O3E_K_LIST || k->kind == O3E_K_ARRAY) {
                continue;   /* variable-length; not a sensible fixed entity */
            }
            size_t plen = strlen(path);
            size_t tlen = strlen(tmpl);
            snprintf(path + plen, path_sz - plen, "_%s", k->id ? k->id : "");
            snprintf(tmpl + tlen, tmpl_sz - tlen, ".%s", k->id ? k->id : "");
            walk_leaves(cfg, dev_id, state_topic, k, did_name, did, ecu,
                        path, path_sz, tmpl, tmpl_sz, flat, writable,
                        clear, depth + 1);
            path[plen] = '\0';
            tmpl[tlen] = '\0';
        }
        return;

    case O3E_K_LIST:
    case O3E_K_ARRAY:
    case O3E_K_UNKNOWN:
        return;

    default:
        /* Not inlined: walk_leaves recurses, so every byte of frame it needs is
         * paid once per level. The publishing buffers are over a kilobyte and
         * are wanted only at the bottom, where nothing recurses further.
         * Keeping them in their own frame is what stops this from overflowing
         * the MQTT control task's stack. */
        publish_leaf(cfg, dev_id, state_topic, n, did_name, did, ecu,
                     path, tmpl, flat, writable, clear);
        return;
    }
}

/* Both publishing and removal walk the same selection and the same codec
 * trees; only the payload differs. Keeping them in one function is what stops
 * removal from targeting topics publishing never created. */
static void ha_disco_walk_selection(bool clear)
{
    n_sensors = n_controls = 0;
    mqtt_cfg_t cfg;
    mqtt_cfg_get(&cfg);
    if (!cfg.enabled || !mqtt_pub_connected()) {
        return;
    }
    if (!clear && !cfg.ha_discovery) {
        return;
    }

    char *raw = app_config_read_file(CFG_POINTS_PATH);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return;
    }

    char dev_id[32];
    device_id(dev_id, sizeof(dev_id));

    int count = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, root) {
        /* Energy-meter frames live in the same file but have no ECU or DID. */
        if (!sel_is_datapoint(it)) {
            continue;
        }
        /* When clearing, ignore the flags: the entity may exist precisely
         * because one of them was just switched off. */
        if (!clear && (!sel_enabled(it) || !sel_bool(it, "ha", false))) {
            continue;
        }
        uint16_t ecu = sel_u16(it, "ecu", 0);
        uint16_t did = sel_u16(it, "did", 0);
        uint16_t dlen = sel_u16(it, "len", 0);
        const char *mode = sel_str(it, "mode");
        bool flat = mode && strcmp(mode, "flat") == 0;

        char *json = o3e_db_json(did, dlen);
        o3e_node_t *node = json ? o3e_codec_compile(json) : NULL;
        free(json);
        if (!node) {
            continue;
        }

        char state_topic[256];
        mqtt_pub_topic(state_topic, sizeof(state_topic), ecu, did, node->id, "",
                       sel_str(it, "topic"));

        char path[192] = "";
        char tmpl[192] = "";
        /* The datapoint's own access flag decides whether its leaves get a
           control. Sub-fields rarely carry one of their own, so taking it from
           the root is both what the database means and what poller_write_now
           checks before it writes. */
        bool writable = (node->acc == O3E_ACC_RW);
        walk_leaves(&cfg, dev_id, state_topic, node,
                    node->id ? node->id : "datapoint", did, ecu,
                    path, sizeof(path), tmpl, sizeof(tmpl), flat, writable,
                    clear, 0);
        o3e_codec_free(node);
        count++;
    }
    cJSON_Delete(root);
    /* Counted separately: "published for 19 datapoints" says nothing about
     * whether any of them became operable, and a control that never appears
     * looks exactly like a datapoint that is read-only. */
    ESP_LOGI(TAG, "discovery %s: %d datapoints, %d sensors, %d controls%s",
             clear ? "cleared" : "published", count, n_sensors, n_controls,
             (!clear && n_controls == 0 && !cfg.cmnd_topic[0])
                 ? " (no command topic configured)" : "");
}

void ha_disco_counts(int *sensors, int *controls)
{
    if (sensors) {
        *sensors = n_sensors;
    }
    if (controls) {
        *controls = n_controls;
    }
}

void ha_disco_publish_all(void) { ha_disco_walk_selection(false); }
void ha_disco_clear_all(void)   { ha_disco_walk_selection(true); }
