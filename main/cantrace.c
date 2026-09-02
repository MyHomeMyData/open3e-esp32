#include "cantrace.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "trace";

/* Single producer contexts (the receive ISR and the one task that owns the
 * bus) and a reader that tolerates a torn newest entry, so no lock is needed.
 * A trace that occasionally shows one incomplete frame at the head is a fair
 * trade for not adding a critical section to the receive interrupt. */
static cantrace_frame_t *ring;
static uint32_t          capacity;
static volatile uint32_t write_idx;
static volatile uint32_t total;
static volatile bool     running;
static int64_t           started_us;
static uint16_t          filt_lo, filt_hi;

/* Direct-mapped index over the 11-bit identifier space, so the per-frame
 * lookup in the receive interrupt is a single array read rather than a scan.
 * 2 KiB for the index, 8 KiB for the table. */
static uint8_t            id_index[2048];   /* 0 = unused, else slot + 1 */
static cantrace_id_stat_t id_stats[CANTRACE_MAX_IDS];
static uint8_t            id_count;
static int64_t            learn_until_us;

static bool               exclude_own;
static volatile uint16_t  own_tx;        /* 0 = no exchange in progress */
static volatile uint32_t  skipped_own;

static cantrace_trigger_t trigger_mode;
static volatile bool      triggered;
static volatile uint32_t  post_left;
static cantrace_event_t   event;
static void             (*trigger_cb)(const cantrace_event_t *);
static SemaphoreHandle_t  trigger_sem;
static TaskHandle_t       notify_task_h;

/* Recognise a UDS write request in a single frame.
 *
 * Two services write: the standard 0x2E, and 0x77, which open3e documents as
 * experimental. On a real E3 bus the interesting writes turned out to be 0x77
 * -- watching only for 0x2E would have missed them entirely.
 *
 * Only the first frame of an exchange carries the service byte, so this sees
 * both the short form and the start of a segmented one. Reassembly is left to
 * the browser; here it only has to be cheap enough for an interrupt. */
static inline bool IRAM_ATTR is_write_service(uint8_t sid)
{
    return sid == 0x2E || sid == 0x77;
}

static inline bool IRAM_ATTR is_write_request(const uint8_t *d, uint8_t dlc)
{
    if (dlc < 2) {
        return false;
    }
    uint8_t pci = d[0] & 0xF0;
    if (pci == 0x00) {
        return (d[0] & 0x0F) >= 3 && is_write_service(d[1]);   /* single frame */
    }
    if (pci == 0x10) {
        return dlc >= 3 && is_write_service(d[2]);             /* first frame */
    }
    return false;
}

static void notify_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(trigger_sem, portMAX_DELAY) == pdTRUE && trigger_cb) {
            trigger_cb(&event);
        }
    }
}

void cantrace_on_trigger(void (*cb)(const cantrace_event_t *))
{
    trigger_cb = cb;
}

void cantrace_note_control(uint16_t can_id, uint16_t did,
                           const uint8_t *data, uint8_t len)
{
    if (!running || triggered || trigger_mode != CANTRACE_TRIG_CONTROL) {
        return;
    }
    triggered = true;
    event.fired = true;
    event.kind = CANTRACE_EV_CONTROL;
    event.us = (uint32_t)(esp_timer_get_time() - started_us);
    event.can_id = can_id;
    event.did = did;
    event.dlc = (len > 8) ? 8 : len;
    memset(event.data, 0, sizeof(event.data));
    if (data && event.dlc) {
        memcpy(event.data, data, event.dlc);
    }
    if (trigger_sem) {
        xSemaphoreGive(trigger_sem);
    }
    ESP_LOGW(TAG, "control datapoint %u changed on 0x%03X -- trace frozen",
             did, can_id);
}

void cantrace_own_begin(uint16_t ecu_tx)
{
    own_tx = ecu_tx;
}

void cantrace_own_end(void)
{
    own_tx = 0;
}

bool cantrace_start(uint32_t frames, uint16_t lo, uint16_t hi,
                    cantrace_trigger_t trigger, uint32_t post_frames,
                    uint32_t learn_s, bool skip_own)
{
    cantrace_stop();

    if (frames == 0) {
        frames = CANTRACE_DEFAULT_FRAMES;
    }
    if (frames > CANTRACE_MAX_FRAMES) {
        frames = CANTRACE_MAX_FRAMES;
    }

    if (capacity != frames) {
        cantrace_free();
        /* PSRAM: a full ring is a megabyte, and nothing here is latency
         * critical enough to justify taking it from internal RAM. */
        ring = heap_caps_malloc((size_t)frames * sizeof(*ring),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ring) {
            ring = malloc((size_t)frames * sizeof(*ring));
        }
        if (!ring) {
            ESP_LOGE(TAG, "no memory for %u frames", (unsigned)frames);
            return false;
        }
        capacity = frames;
    }

    if (!trigger_sem) {
        trigger_sem = xSemaphoreCreateBinary();
        if (trigger_sem) {
            xTaskCreate(notify_task, "tracenote", 4096, NULL, 3, &notify_task_h);
        }
    }

    memset(id_index, 0, sizeof(id_index));
    memset(id_stats, 0, sizeof(id_stats));
    id_count = 0;
    learn_until_us = esp_timer_get_time() + (int64_t)learn_s * 1000000;

    exclude_own = skip_own;
    skipped_own = 0;

    write_idx = 0;
    total = 0;
    triggered = false;
    trigger_mode = trigger;
    post_left = post_frames ? post_frames : frames / 2;
    memset(&event, 0, sizeof(event));
    filt_lo = lo;
    filt_hi = hi >= lo ? hi : 0x7FF;
    started_us = esp_timer_get_time();
    running = true;
    ESP_LOGI(TAG, "capturing 0x%03X..0x%03X into %u frames%s",
             filt_lo, filt_hi, (unsigned)capacity,
             trigger == CANTRACE_TRIG_WRITE ? ", stopping after a write"
             : trigger == CANTRACE_TRIG_NOVEL ? ", learning first" : "");
    return true;
}

void cantrace_stop(void)
{
    if (running) {
        running = false;
        ESP_LOGI(TAG, "capture stopped: %u frames", (unsigned)total);
    }
}

void cantrace_free(void)
{
    running = false;
    free(ring);
    ring = NULL;
    capacity = 0;
    write_idx = 0;
    total = 0;
}

void IRAM_ATTR cantrace_put(uint16_t id, const uint8_t *data, uint8_t dlc, bool tx)
{
    if (!running || !ring || id < filt_lo || id > filt_hi) {
        return;
    }
    /* Our own exchange: the request we sent and the reply it drew. Dropping
     * both leaves the bus's own conversation, which is what is being looked
     * for -- and keeps the learning phase from being taught our poll cycle. */
    if (exclude_own) {
        uint16_t mine = own_tx;
        if (mine && (id == mine || id == (uint16_t)(mine + 0x10))) {
            skipped_own++;
            return;
        }
    }
    uint32_t i = write_idx;
    cantrace_frame_t *f = &ring[i];
    f->us = (uint32_t)(esp_timer_get_time() - started_us);
    f->id = id;
    f->dlc = dlc > 8 ? 8 : dlc;
    f->flags = tx ? CANTRACE_FLAG_TX : 0;
    memcpy(f->data, data, f->dlc);
    if (f->dlc < 8) {
        memset(f->data + f->dlc, 0, 8 - f->dlc);
    }

    write_idx = (i + 1) % capacity;
    total++;

    /* Per-identifier bookkeeping. Cheap enough for the interrupt: one indexed
     * lookup and at most eight byte comparisons. */
    bool learning = esp_timer_get_time() < learn_until_us;
    uint8_t slot = id_index[id & 0x7FF];
    if (!slot) {
        if (id_count < CANTRACE_MAX_IDS) {
            slot = ++id_count;
            id_index[id & 0x7FF] = slot;
            cantrace_id_stat_t *st = &id_stats[slot - 1];
            st->id = id;
            st->first_us = f->us;
            st->dlc = f->dlc;
            memcpy(st->first, f->data, 8);
            memcpy(st->last, f->data, 8);

            /* An identifier that never appeared while learning is itself the
             * anomaly -- no payload analysis needed. */
            if (trigger_mode == CANTRACE_TRIG_NOVEL && !learning && !triggered) {
                triggered = true;
                event.fired = true;
                event.kind = CANTRACE_EV_NEW_ID;
                event.us = f->us;
                event.can_id = id;
                event.dlc = f->dlc;
                memcpy(event.data, f->data, 8);
                if (trigger_sem) {
                    BaseType_t w = pdFALSE;
                    xSemaphoreGiveFromISR(trigger_sem, &w);
                    if (w == pdTRUE) {
                        portYIELD_FROM_ISR();
                    }
                }
            }
        }
    } else {
        cantrace_id_stat_t *st = &id_stats[slot - 1];
        st->count++;
        st->last_us = f->us;
        for (uint8_t k = 0; k < 8; k++) {
            if (st->last[k] == f->data[k]) {
                continue;
            }
            /* While learning, a difference only means the byte is not
             * constant. Afterwards, a byte that stayed constant throughout and
             * now moves is exactly what we are watching for. */
            if (learning) {
                st->varying |= (uint8_t)(1u << k);
            } else if (trigger_mode == CANTRACE_TRIG_NOVEL && !triggered &&
                       !(st->varying & (1u << k))) {
                triggered = true;
                event.fired = true;
                event.kind = CANTRACE_EV_BYTE_CHANGE;
                event.us = f->us;
                event.can_id = id;
                event.byte_index = k;
                event.was = st->last[k];
                event.now = f->data[k];
                event.dlc = f->dlc;
                memcpy(event.data, f->data, 8);
                if (trigger_sem) {
                    BaseType_t w = pdFALSE;
                    xSemaphoreGiveFromISR(trigger_sem, &w);
                    if (w == pdTRUE) {
                        portYIELD_FROM_ISR();
                    }
                }
            }
            st->last[k] = f->data[k];
        }
    }

    if ((trigger_mode == CANTRACE_TRIG_NOVEL ||
         trigger_mode == CANTRACE_TRIG_CONTROL) && triggered) {
        if (post_left == 0) {
            running = false;
        } else {
            post_left--;
        }
    }

    if (trigger_mode == CANTRACE_TRIG_WRITE) {
        if (!triggered && is_write_request(data, dlc)) {
            triggered = true;
            event.fired = true;
            event.us = f->us;
            event.kind = CANTRACE_EV_WRITE;
            event.can_id = id;
            /* The identifier sits after the service byte in both frame forms. */
            event.did = ((data[0] & 0xF0) == 0x00)
                      ? (uint16_t)((data[2] << 8) | data[3])
                      : (uint16_t)((data[3] << 8) | data[4]);
            event.dlc = f->dlc;
            memcpy(event.data, f->data, 8);
            if (trigger_sem) {
                BaseType_t woken = pdFALSE;
                xSemaphoreGiveFromISR(trigger_sem, &woken);
                if (woken == pdTRUE) {
                    portYIELD_FROM_ISR();
                }
            }
        } else if (triggered) {
            /* Keep recording a while so the response is captured too, then
             * stop rather than overwrite the event. */
            if (post_left == 0) {
                running = false;
            } else {
                post_left--;
            }
        }
    }
}

void cantrace_stats(cantrace_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->running = running;
    out->capacity = capacity;
    out->captured = total;
    out->stored = total < capacity ? total : capacity;
    out->dropped = total > capacity ? total - capacity : 0;
    out->elapsed_ms = started_us ? (uint32_t)((esp_timer_get_time() - started_us) / 1000) : 0;
    out->filter_lo = filt_lo;
    out->filter_hi = filt_hi;
    out->trigger = trigger_mode;
    out->exclude_own = exclude_own;
    out->skipped_own = skipped_own;
    out->triggered = triggered;
    out->post_remaining = post_left;
    out->event = event;
}

bool cantrace_learning(uint32_t *remaining_s)
{
    int64_t left = learn_until_us - esp_timer_get_time();
    if (remaining_s) {
        *remaining_s = left > 0 ? (uint32_t)(left / 1000000) : 0;
    }
    return left > 0;
}

static int by_count_desc(const void *a, const void *b)
{
    const cantrace_id_stat_t *x = a, *y = b;
    return (x->count < y->count) - (x->count > y->count);
}

size_t cantrace_ids(cantrace_id_stat_t *out, size_t max)
{
    size_t n = id_count < max ? id_count : max;
    memcpy(out, id_stats, n * sizeof(*out));
    qsort(out, n, sizeof(*out), by_count_desc);
    return n;
}

size_t cantrace_read(size_t from, cantrace_frame_t *out, size_t max)
{
    if (!ring || capacity == 0) {
        return 0;
    }
    uint32_t stored = total < capacity ? total : capacity;
    if (from >= stored) {
        return 0;
    }
    /* Oldest first: once the ring has wrapped, the oldest entry sits just
     * after the write cursor. */
    uint32_t oldest = total < capacity ? 0 : write_idx;
    size_t n = stored - from;
    if (n > max) {
        n = max;
    }
    for (size_t k = 0; k < n; k++) {
        out[k] = ring[(oldest + from + k) % capacity];
    }
    return n;
}
