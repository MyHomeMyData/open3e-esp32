/* The hardware half of the two pins: configuration, sampling, driving, MQTT.
 *
 * Sampling rather than interrupts, deliberately. A bouncing contact produces
 * dozens of edges in a few milliseconds and an AC-sensed one produces a
 * hundred a second, for as long as somebody leans on the button; an interrupt
 * per edge would put that storm into the CAN receive path's own core for no
 * gain. Two pin reads every five milliseconds cost nothing measurable and
 * cannot burst.
 *
 * Outputs ride the same loop: whoever wants a relay switched only writes a
 * wish, and the loop drives the pin and publishes the change. One task owns
 * the pins, so an MQTT command, a web click and a settings save cannot
 * interleave on the same GPIO register.
 */
#include "contact.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "knx.h"
#include "mqtt_pub.h"

static const char *TAG = "contact";

static contact_cfg_t s_cfg[CONTACT_COUNT];
/* For an input the debouncer; for an output only `active`, `since_ms` and
 * `edges` are used, with the same meaning, so status reporting is shared. */
static contact_deb_t s_deb[CONTACT_COUNT];
/* What an output should be, as last requested. Written from any task, read
 * by the loop; a bool, so no lock. */
static volatile bool s_want[CONTACT_COUNT];
/* Parsed once at configuration time rather than on every edge: a bad address
 * should be reported when it is entered, not silently on a doorbell press. */
static uint16_t      s_ga[CONTACT_COUNT];
static TaskHandle_t  s_task;
static volatile bool s_reload = true;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool is_output(int i)
{
    return s_cfg[i].mode == CONTACT_MODE_OUTPUT;
}

static void topic_for(int i, char *out, size_t out_sz)
{
    mqtt_cfg_t mq;
    mqtt_cfg_get(&mq);
    char slug[CONTACT_NAME_MAX * 2];
    contact_slug(&s_cfg[i], i, slug, sizeof(slug));
    snprintf(out, out_sz, "%s/%s/%s", mq.base_topic,
             is_output(i) ? "output" : "contact", slug);
}

static void drive(int i, bool on)
{
    gpio_set_level((gpio_num_t)CONTACT_PINS[i],
                   (on != s_cfg[i].active_low) ? 1 : 0);
}

/* Retained, so a subscriber that connects later -- Home Assistant after a
 * restart, say -- learns the current state instead of waiting for the next
 * time somebody rings. The retained value is never stale for long: every
 * change overwrites it within `release_ms`. */
static void publish_one(int i)
{
    if (!s_cfg[i].enabled || !mqtt_pub_connected()) {
        return;
    }
    char topic[CFG_TOPIC_MAX + CONTACT_NAME_MAX * 2 + 16];
    topic_for(i, topic, sizeof(topic));
    mqtt_pub_raw(topic, s_deb[i].active ? "ON" : "OFF", true);
}

void contact_publish(void)
{
    for (int i = 0; i < CONTACT_COUNT; i++) {
        publish_one(i);
    }
}

static void apply_cfg(void)
{
    sys_cfg_t sys;
    sys_cfg_get(&sys);
    uint32_t t = now_ms();
    contact_cfg_t was[CONTACT_COUNT];
    memcpy(was, s_cfg, sizeof(was));

    for (int i = 0; i < CONTACT_COUNT; i++) {
        s_cfg[i] = sys.contact[i];
        if (s_cfg[i].release_ms < CONTACT_RELEASE_MIN) {
            s_cfg[i].release_ms = CONTACT_RELEASE_MS;
        } else if (s_cfg[i].release_ms > CONTACT_RELEASE_MAX) {
            s_cfg[i].release_ms = CONTACT_RELEASE_MAX;
        }

        if (!s_cfg[i].enabled) {
            /* Back to a plain input with no pull, which is what an unused pin
             * on a connector should be: nothing driven, nothing to short. */
            gpio_reset_pin((gpio_num_t)CONTACT_PINS[i]);
            s_want[i] = false;
            continue;
        }
        if (is_output(i)) {
            /* A pin that was not an output a moment ago starts off: either
             * it was unused, or it was an input and nobody has asked for
             * anything yet. One that already was an output keeps its state
             * across a settings save -- renaming a relay must not drop the
             * lock it is holding. */
            if (was[i].mode != CONTACT_MODE_OUTPUT || !was[i].enabled) {
                s_want[i] = false;
                contact_deb_reset(&s_deb[i], t);
            }
            /* The idle level is set before the pin becomes an output, so a
             * low-active relay does not see a brief high-to-low blip. */
            drive(i, s_want[i]);
            gpio_config_t io = {
                .pin_bit_mask = 1ULL << CONTACT_PINS[i],
                .mode         = GPIO_MODE_OUTPUT,
                .pull_up_en   = false,
                .pull_down_en = false,
                .intr_type    = GPIO_INTR_DISABLE,
            };
            esp_err_t e = gpio_config(&io);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "GPIO%d could not be configured: %s",
                         CONTACT_PINS[i], esp_err_to_name(e));
                s_cfg[i].enabled = false;
                continue;
            }
            drive(i, s_want[i]);
            s_deb[i].active = s_want[i];
            s_ga[i] = 0;
            if (s_cfg[i].knx_ga[0] && !knx_ga_parse(s_cfg[i].knx_ga, &s_ga[i])) {
                ESP_LOGW(TAG, "GPIO%d: \"%s\" is not a group address",
                         CONTACT_PINS[i], s_cfg[i].knx_ga);
                s_ga[i] = 0;
            }
            ESP_LOGI(TAG, "GPIO%d: \"%s\", output, on = %s",
                     CONTACT_PINS[i], s_cfg[i].name[0] ? s_cfg[i].name : "(unnamed)",
                     s_cfg[i].active_low ? "low" : "high");
            continue;
        }
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << CONTACT_PINS[i],
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = s_cfg[i].wire == CONTACT_TO_GND,
            .pull_down_en = s_cfg[i].wire == CONTACT_TO_3V3,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        esp_err_t e = gpio_config(&io);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "GPIO%d could not be configured: %s",
                     CONTACT_PINS[i], esp_err_to_name(e));
            s_cfg[i].enabled = false;
            continue;
        }
        contact_deb_reset(&s_deb[i], t);
        s_ga[i] = 0;
        if (s_cfg[i].knx_ga[0] && !knx_ga_parse(s_cfg[i].knx_ga, &s_ga[i])) {
            ESP_LOGW(TAG, "GPIO%d: \"%s\" is not a group address",
                     CONTACT_PINS[i], s_cfg[i].knx_ga);
            s_ga[i] = 0;
        }
        ESP_LOGI(TAG, "GPIO%d: \"%s\", contact to %s, release %u ms",
                 CONTACT_PINS[i], s_cfg[i].name[0] ? s_cfg[i].name : "(unnamed)",
                 s_cfg[i].wire == CONTACT_TO_GND ? "GND" : "3V3",
                 (unsigned)s_cfg[i].release_ms);
    }
}

static void contact_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_reload) {
            s_reload = false;
            apply_cfg();
            contact_publish();
        }

        uint32_t t = now_ms();
        for (int i = 0; i < CONTACT_COUNT; i++) {
            if (!s_cfg[i].enabled) {
                continue;
            }
            if (is_output(i)) {
                bool want = s_want[i];
                if (want == s_deb[i].active) {
                    continue;
                }
                drive(i, want);
                s_deb[i].active = want;
                s_deb[i].since_ms = t;
                if (want) {
                    s_deb[i].edges++;
                }
                ESP_LOGI(TAG, "%s: %s",
                         s_cfg[i].name[0] ? s_cfg[i].name : "output",
                         want ? "on" : "off");
                publish_one(i);
                if (s_ga[i]) {
                    knx_send_bool(s_ga[i], want);
                }
                continue;
            }
            int level = gpio_get_level((gpio_num_t)CONTACT_PINS[i]);
            bool raw = s_cfg[i].wire == CONTACT_TO_GND ? level == 0 : level == 1;
            if (contact_deb_step(&s_deb[i], raw, t, s_cfg[i].release_ms)) {
                ESP_LOGI(TAG, "%s: %s",
                         s_cfg[i].name[0] ? s_cfg[i].name : "input",
                         s_deb[i].active ? "closed" : "open");
                publish_one(i);
                /* Queued, never sent from here: this loop samples every five
                 * milliseconds and must not wait on a gateway that has been
                 * unplugged. */
                if (s_ga[i]) {
                    knx_send_bool(s_ga[i], s_deb[i].active);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CONTACT_POLL_MS));
    }
}

void contact_start(void)
{
    s_reload = true;
    if (s_task) {
        return;
    }
    /* Started even when both inputs are off: the task idles at two comparisons
     * every five milliseconds, and having it already running is what makes
     * enabling an input in the web UI take effect without a reboot. */
    if (xTaskCreate(contact_task, "contact", 3072, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "could not start the input task");
    }
}

bool contact_output_set(int idx, bool on)
{
    if (idx < 0 || idx >= CONTACT_COUNT || !s_cfg[idx].enabled || !is_output(idx)) {
        return false;
    }
    s_want[idx] = on;
    return true;
}

bool contact_output_set_by_slug(const char *slug, bool on)
{
    for (int i = 0; i < CONTACT_COUNT; i++) {
        if (!s_cfg[i].enabled || !is_output(i)) {
            continue;
        }
        char mine[CONTACT_NAME_MAX * 2];
        contact_slug(&s_cfg[i], i, mine, sizeof(mine));
        if (strcmp(mine, slug) == 0) {
            return contact_output_set(i, on);
        }
    }
    return false;
}

void contact_status(int idx, contact_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (idx < 0 || idx >= CONTACT_COUNT) {
        return;
    }
    out->enabled = s_cfg[idx].enabled;
    out->mode    = s_cfg[idx].mode;
    out->active  = s_deb[idx].active;
    out->edges   = s_deb[idx].edges;
    out->since_s = (now_ms() - s_deb[idx].since_ms) / 1000;
}
