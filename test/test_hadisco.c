/* The Home Assistant discovery payloads, built on a workstation.
 *
 * This is the part of the firmware whose failures are silent: an entity that
 * never appears looks exactly like a datapoint that has none. Three separate
 * faults reached a real installation before this test existed -- an enum leaf
 * pointed at a topic that never carries a value, a control that was never
 * published because a setting was blank, and a stack overflow that stopped
 * publishing halfway. All three are cheap to catch here and expensive to
 * diagnose on a device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "../main/app_config.h"
#include "../main/ha_disco.h"
#include "../main/mqtt_pub.h"
#include "../main/o3e_db.h"

/* ---- the world the discovery code talks to ---- */

static mqtt_cfg_t g_cfg;
static char      *g_points;

typedef struct { char *topic; char *payload; } msg_t;
static msg_t  g_msgs[512];
static size_t g_n;

int esp_read_mac(uint8_t *out, int t)
{
    (void)t;
    static const uint8_t mac[6] = { 0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33 };
    memcpy(out, mac, 6);
    return 0;
}

void mqtt_cfg_get(mqtt_cfg_t *out) { *out = g_cfg; }
bool mqtt_pub_connected(void) { return true; }

bool mqtt_pub_raw(const char *topic, const char *payload, bool retain)
{
    (void)retain;
    if (g_n < sizeof(g_msgs) / sizeof(g_msgs[0])) {
        g_msgs[g_n].topic = strdup(topic);
        g_msgs[g_n].payload = strdup(payload);
        g_n++;
    }
    return true;
}

/* Same expansion the firmware does; only {didName} is exercised here. */
void mqtt_pub_topic(char *out, size_t out_sz, uint16_t ecu, uint16_t did,
                    const char *did_name, const char *device,
                    const char *suffix_override)
{
    (void)ecu; (void)did; (void)device;
    snprintf(out, out_sz, "%s/%s", g_cfg.base_topic,
             (suffix_override && suffix_override[0]) ? suffix_override
                                                     : (did_name ? did_name : ""));
}

char *app_config_read_file(const char *path) { (void)path; return strdup(g_points); }

/* The selection helpers live in app_config.c next to NVS and LittleFS; only
 * these few lines of them matter here. */
bool sel_is_datapoint(const struct cJSON *e)
{
    const cJSON *t = cJSON_GetObjectItemCaseSensitive((const cJSON *)e, "type");
    if (cJSON_IsString(t) && t->valuestring && strcmp(t->valuestring, "em380") == 0) {
        return false;
    }
    return cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive((const cJSON *)e, "ecu"))
        && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive((const cJSON *)e, "did"));
}
bool sel_enabled(const struct cJSON *e)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((const cJSON *)e, "enabled");
    return !v || cJSON_IsTrue(v);
}
bool sel_bool(const struct cJSON *e, const char *k, bool dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((const cJSON *)e, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : dflt;
}
uint16_t sel_u16(const struct cJSON *e, const char *k, uint16_t dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((const cJSON *)e, k);
    return cJSON_IsNumber(v) ? (uint16_t)v->valuedouble : dflt;
}
const char *sel_str(const struct cJSON *e, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((const cJSON *)e, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* ---- checks ---- */

static int fail;
static void ok(const char *what, int cond, const char *detail)
{
    if (!cond) {
        fail++;
        printf("  FAIL %-46s %s\n", what, detail ? detail : "");
    }
}

static const msg_t *find(const char *topic)
{
    for (size_t i = 0; i < g_n; i++) {
        if (strcmp(g_msgs[i].topic, topic) == 0) {
            return &g_msgs[i];
        }
    }
    return NULL;
}

static void reset(const char *points, const char *cmnd, const char *mode)
{
    for (size_t i = 0; i < g_n; i++) { free(g_msgs[i].topic); free(g_msgs[i].payload); }
    g_n = 0;
    free(g_points);
    g_points = strdup(points);
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.enabled = true;
    g_cfg.ha_discovery = true;
    snprintf(g_cfg.base_topic, sizeof(g_cfg.base_topic), "open3e-vent");
    snprintf(g_cfg.ha_prefix, sizeof(g_cfg.ha_prefix), "homeassistant");
    snprintf(g_cfg.format, sizeof(g_cfg.format), "{didName}");
    snprintf(g_cfg.cmnd_topic, sizeof(g_cfg.cmnd_topic), "%s", cmnd);
    (void)mode;
}

/* 533 is writable and its Acutual field is a byte value: the exact datapoint a
 * ventilation unit is operated by. */
#define POINTS_533 "[{\"ecu\":1664,\"did\":533,\"len\":2,\"enabled\":true," \
                   "\"interval\":60,\"mode\":\"%s\",\"topic\":\"\",\"ha\":true}]"
#define POINTS_327 "[{\"ecu\":1664,\"did\":327,\"len\":9,\"enabled\":true," \
                   "\"interval\":60,\"mode\":\"%s\",\"topic\":\"\",\"ha\":true}]"

int main(int argc, char **argv)
{
    if (!o3e_db_open(argc > 1 ? argv[1] : "data/o3edb.bin")) {
        fprintf(stderr, "cannot open the datapoint database\n");
        return 1;
    }
    char points[512];
    const msg_t *m;

    /* --- a writable datapoint gets a control, in both publishing modes --- */
    for (const char *mode = "flat";; mode = "json") {
        snprintf(points, sizeof(points), POINTS_533, mode);
        reset(points, "open3e-vent/cmnd", mode);
        ha_disco_publish_all();

        int sensors = 0, controls = 0;
        ha_disco_counts(&sensors, &controls);
        char d[96];
        snprintf(d, sizeof(d), "mode=%s: %d sensors, %d controls", mode, sensors, controls);
        ok("writable datapoint yields a control", controls >= 1, d);

        m = find("homeassistant/number/open3e_112233/680_533_Acutual_set/config");
        ok("control config on the expected topic", m != NULL, d);
        if (m) {
            cJSON *j = cJSON_Parse(m->payload);
            ok("control payload is valid JSON", j != NULL, m->payload);
            if (j) {
                const cJSON *ct = cJSON_GetObjectItem(j, "command_topic");
                ok("command_topic is the configured one",
                   cJSON_IsString(ct) && strcmp(ct->valuestring, "open3e-vent/cmnd") == 0,
                   m->payload);
                const cJSON *tp = cJSON_GetObjectItem(j, "command_template");
                ok("command_template present", cJSON_IsString(tp), m->payload);
                if (cJSON_IsString(tp)) {
                    /* Substitute the way Home Assistant does, then insist the
                     * result is the command this gateway actually accepts. */
                    char filled[512], *w = filled;
                    for (const char *p = tp->valuestring; *p && w < filled + sizeof(filled) - 8; ) {
                        if (strncmp(p, "{{ value }}", 11) == 0) { *w++ = '4'; p += 11; }
                        else { *w++ = *p++; }
                    }
                    *w = '\0';
                    cJSON *c = cJSON_Parse(filled);
                    ok("rendered command is valid JSON", c != NULL, filled);
                    if (c) {
                        const cJSON *data = cJSON_GetObjectItem(c, "data");
                        const cJSON *did = data ? cJSON_GetObjectItem(data, "533") : NULL;
                        const cJSON *fld = did ? cJSON_GetObjectItem(did, "Acutual") : NULL;
                        ok("rendered command sets the right field",
                           cJSON_IsNumber(fld) && fld->valuedouble == 4, filled);
                        cJSON_Delete(c);
                    }
                }
                const cJSON *mx = cJSON_GetObjectItem(j, "max");
                ok("range comes from the field width, not HA's 0..100",
                   cJSON_IsNumber(mx) && mx->valuedouble == 255, m->payload);
                cJSON_Delete(j);
            }
        }
        if (strcmp(mode, "json") == 0) {
            break;
        }
    }

    /* --- an empty command topic must not silently swallow every control --- */
    snprintf(points, sizeof(points), POINTS_533, "flat");
    reset(points, "", "flat");
    ha_disco_publish_all();
    {
        int sensors = 0, controls = 0;
        ha_disco_counts(&sensors, &controls);
        char d[96];
        snprintf(d, sizeof(d), "%d sensors, %d controls", sensors, controls);
        ok("without a command topic: sensors still published", sensors > 0, d);
        ok("without a command topic: no controls, and it is countable",
           controls == 0, d);
    }

    /* --- an enum leaf is an object on the wire, not a scalar --- */
    snprintf(points, sizeof(points), POINTS_327, "flat");
    reset(points, "open3e-vent/cmnd", "flat");
    ha_disco_publish_all();
    m = find("homeassistant/sensor/open3e_112233/680_327_Error/config");
    ok("enum leaf is announced", m != NULL, NULL);
    if (m) {
        cJSON *j = cJSON_Parse(m->payload);
        const cJSON *st = j ? cJSON_GetObjectItem(j, "state_topic") : NULL;
        ok("enum sensor points at the Text sub-topic, which is what flat mode fills",
           cJSON_IsString(st)
             && strcmp(st->valuestring,
                       "open3e-vent/OutdoorAirTemperatureSensor/Error/Text") == 0,
           cJSON_IsString(st) ? st->valuestring : "(none)");
        cJSON_Delete(j);
    }
    m = find("homeassistant/sensor/open3e_112233/680_327_Actual/config");
    if (m) {
        cJSON *j = cJSON_Parse(m->payload);
        const cJSON *u = j ? cJSON_GetObjectItem(j, "unit_of_measurement") : NULL;
        ok("temperature carries the unit the overrides fill in",
           cJSON_IsString(u) && strcmp(u->valuestring, "°C") == 0,
           cJSON_IsString(u) ? u->valuestring : "(none)");
        const cJSON *sc = j ? cJSON_GetObjectItem(j, "state_class") : NULL;
        ok("a measurement is a measurement",
           cJSON_IsString(sc) && strcmp(sc->valuestring, "measurement") == 0, NULL);
        cJSON_Delete(j);
    } else {
        ok("temperature leaf is announced", 0, NULL);
    }

    printf("%s: %zu discovery messages inspected\n", fail ? "FAILED" : "ha_disco", g_n);
    return fail ? 1 : 0;
}
