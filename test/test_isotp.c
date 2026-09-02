/* Exercise the ISO-TP implementation against itself over a simulated bus.
 *
 * Sender and receiver run in separate threads sharing one frame queue with
 * swapped COB-IDs, so a real FF / FC / CF handshake takes place -- the same
 * exchange that runs on the wire, minus the TWAI peripheral.  Running them
 * concurrently is the point: isotp_send() blocks on flow control that only
 * isotp_recv() produces, so a single-threaded harness would prove nothing
 * about segmented messages.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../main/isotp.h"

#define QMAX 2048

typedef struct {
    uint32_t id;
    uint8_t  data[8];
    uint8_t  len;
} frame_t;

/* Two endpoints, each with its own inbox.
 *
 * An earlier version had one shared queue and filtered by CAN-ID inside the
 * fake receive call. That is not what the hardware does: the TWAI driver hands
 * up every frame on the wire and isotp_recv() does the filtering itself. The
 * difference matters -- it is exactly why a receive loop spun forever on a
 * busy bus in the field while these tests stayed green. */
typedef struct {
    frame_t q[QMAX];
    size_t  n;
} inbox_t;

static struct {
    inbox_t         ep[2];        /* 0 = tester, 1 = ECU */
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    int             drop_cf_nth;  /* 0 = lossless */
    int             cf_count;
} bus;

static void bus_reset(int drop_cf_nth)
{
    pthread_mutex_lock(&bus.lock);
    bus.ep[0].n = 0;
    bus.ep[1].n = 0;
    bus.cf_count = 0;
    bus.drop_cf_nth = drop_cf_nth;
    pthread_mutex_unlock(&bus.lock);
}

static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    struct timespec ts = { .tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

static void deliver_locked(int to, uint32_t id, const uint8_t *data, uint8_t len)
{
    inbox_t *b = &bus.ep[to];
    if (b->n >= QMAX) {
        return;
    }
    frame_t *f = &b->q[b->n++];
    f->id = id;
    f->len = len;
    memcpy(f->data, data, len);
}

/* ctx is the sending endpoint; the frame lands in the other one's inbox, as a
 * node hears its peer but not itself. A NULL ctx is a third node on the wire
 * and reaches both. */
static bool bus_send(void *ctx, uint32_t id, const uint8_t *data, uint8_t len)
{
    int from = ctx ? *(int *)ctx : -1;

    pthread_mutex_lock(&bus.lock);
    if ((data[0] & 0xF0) == 0x20) {
        bus.cf_count++;
        if (bus.drop_cf_nth && bus.cf_count % bus.drop_cf_nth == 0) {
            pthread_mutex_unlock(&bus.lock);
            return true;   /* transmitted but never arrived */
        }
    }
    for (int to = 0; to < 2; to++) {
        if (to != from) {
            deliver_locked(to, id, data, len);
        }
    }
    pthread_cond_broadcast(&bus.cv);
    pthread_mutex_unlock(&bus.lock);
    return true;
}

/* Returns the next frame in the caller's inbox whatever its CAN-ID, exactly as
 * the TWAI driver does. Filtering is isotp_recv()'s job. */
static bool bus_recv(void *ctx, uint32_t *id, uint8_t *data, uint8_t *len, uint32_t timeout_ms)
{
    int me = *(int *)ctx;
    uint32_t deadline = now_ms(NULL) + timeout_ms;

    pthread_mutex_lock(&bus.lock);
    for (;;) {
        inbox_t *b = &bus.ep[me];
        if (b->n) {
            *id = b->q[0].id;
            *len = b->q[0].len;
            memcpy(data, b->q[0].data, b->q[0].len);
            memmove(&b->q[0], &b->q[1], (b->n - 1) * sizeof(frame_t));
            b->n--;
            pthread_mutex_unlock(&bus.lock);
            return true;
        }
        uint32_t now = now_ms(NULL);
        if (now >= deadline) {
            pthread_mutex_unlock(&bus.lock);
            return false;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint32_t wait = deadline - now;
        ts.tv_nsec += (long)(wait % 1000) * 1000000;
        ts.tv_sec += wait / 1000 + ts.tv_nsec / 1000000000;
        ts.tv_nsec %= 1000000000;
        pthread_cond_timedwait(&bus.cv, &bus.lock, &ts);
    }
}

static int ep_tester = 0, ep_ecu = 1;

static const isotp_hal_t hal_tester = {
    .send = bus_send, .recv = bus_recv, .delay_us = delay_us,
    .now_ms = now_ms, .ctx = &ep_tester,
};
static const isotp_hal_t hal_ecu = {
    .send = bus_send, .recv = bus_recv, .delay_us = delay_us,
    .now_ms = now_ms, .ctx = &ep_ecu,
};

static int failures;

static void check(bool cond, const char *fmt, ...)
{
    if (cond) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    printf("  FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    failures++;
}

struct send_job {
    isotp_link_t  link;
    const uint8_t *msg;
    size_t        len;
    isotp_err_t   err;
};

static void *send_thread(void *arg)
{
    struct send_job *j = arg;
    j->err = isotp_send(&j->link, j->msg, j->len);
    return NULL;
}

static void roundtrip(size_t len, int drop_cf_nth, isotp_err_t want_recv, const char *label)
{
    bus_reset(drop_cf_nth);

    uint8_t *msg = malloc(len);
    for (size_t i = 0; i < len; i++) {
        msg[i] = (uint8_t)(i * 31 + 7);
    }

    /* stmin 0 keeps the test fast; the wire value is ISOTP_STMIN_MS and is
     * covered by the STmin encoding checks below. */
    struct send_job job = {
        .link = { .hal = &hal_tester, .tx_id = 0x680, .rx_id = 0x690, .stmin_ms = 0 },
        .msg = msg, .len = len,
    };
    isotp_link_t ecu = { .hal = &hal_ecu, .tx_id = 0x690, .rx_id = 0x680, .stmin_ms = 0 };

    pthread_t th;
    pthread_create(&th, NULL, send_thread, &job);

    uint8_t out[ISOTP_MAX_PAYLOAD];
    size_t out_len = 0;
    isotp_err_t re = isotp_recv(&ecu, out, sizeof(out), &out_len, 2000);
    pthread_join(th, NULL);

    printf("%-38s send=%-12s recv=%s\n", label, isotp_strerror(job.err), isotp_strerror(re));
    check(re == want_recv, "%s: recv expected %s, got %s",
          label, isotp_strerror(want_recv), isotp_strerror(re));
    if (want_recv == ISOTP_OK) {
        check(job.err == ISOTP_OK, "%s: send failed: %s", label, isotp_strerror(job.err));
        check(out_len == len, "%s: length %zu != %zu", label, out_len, len);
        if (out_len == len) {
            check(memcmp(msg, out, len) == 0, "%s: payload mismatch", label);
        }
    }
    free(msg);
}

/* Injects unrelated traffic on a third CAN-ID while a receive is in progress.
 *
 * This is the shape of a real E3 bus: the devices talk to each other
 * constantly. A receive loop whose only limit is a per-frame timeout never
 * expires under that traffic, because a frame always arrives -- just never the
 * one it is waiting for. That held the bus for hours in the field. */
static volatile bool noise_running;

static void *noise_thread(void *arg)
{
    (void)arg;
    uint8_t junk[8] = { 0x21, 1, 2, 3, 4, 5, 6, 7 };
    while (noise_running) {
        bus_send(NULL, 0x123, junk, sizeof(junk));
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* A first frame followed by silence, while the bus is busy with other nodes.
 * The receive must give up on its own rather than wait forever. */
static void stalled_receive(void)
{
    bus_reset(0);
    isotp_link_t ecu = { .hal = &hal_ecu, .tx_id = 0x690, .rx_id = 0x680, .stmin_ms = 0 };

    /* A first frame announcing 64 bytes, of which no consecutive frame ever
     * follows. */
    uint8_t ff[8] = { 0x10, 64, 1, 2, 3, 4, 5, 6 };
    bus_send(NULL, 0x680, ff, sizeof(ff));

    noise_running = true;
    pthread_t th;
    pthread_create(&th, NULL, noise_thread, NULL);

    uint8_t out[ISOTP_MAX_PAYLOAD];
    size_t out_len = 0;
    uint32_t t0 = now_ms(NULL);
    isotp_err_t e = isotp_recv(&ecu, out, sizeof(out), &out_len, 1000);
    uint32_t took = now_ms(NULL) - t0;

    noise_running = false;
    pthread_join(th, NULL);

    printf("%-38s %s after %u ms\n", "stalled receive under bus noise",
           isotp_strerror(e), took);
    check(e != ISOTP_OK, "stalled receive must not report success");
    check(took < ISOTP_MSG_MS + 1500,
          "stalled receive took %u ms; it must be bounded by ISOTP_MSG_MS (%u)",
          took, ISOTP_MSG_MS);
}

/* A peer that answers every flow control with "wait" must not be granted an
 * unbounded extension. */
static void endless_wait_fc(void)
{
    bus_reset(0);
    isotp_link_t tester = { .hal = &hal_tester, .tx_id = 0x680, .rx_id = 0x690,
                            .stmin_ms = 0 };

    uint8_t msg[64];
    memset(msg, 0xAB, sizeof(msg));

    noise_running = true;
    pthread_t th;
    pthread_create(&th, NULL, noise_thread, NULL);

    /* Queue a stream of WAIT flow controls for the sender to chew on. */
    for (int i = 0; i < 200; i++) {
        uint8_t fc[8] = { 0x31, 0, 0, 0, 0, 0, 0, 0 };   /* FS = wait */
        bus_send(NULL, 0x690, fc, sizeof(fc));
    }

    uint32_t t0 = now_ms(NULL);
    isotp_err_t e = isotp_send(&tester, msg, sizeof(msg));
    uint32_t took = now_ms(NULL) - t0;

    noise_running = false;
    pthread_join(th, NULL);

    printf("%-38s %s after %u ms\n", "endless WAIT flow control",
           isotp_strerror(e), took);
    check(e != ISOTP_OK, "a peer stuck on WAIT must not look like success");
    check(took < ISOTP_MSG_MS + 1500,
          "endless WAIT took %u ms; N_WFTmax must bound it", took);
}

int main(void)
{
    pthread_mutex_init(&bus.lock, NULL);
    pthread_cond_init(&bus.cv, NULL);

    printf("== ISO-TP over a simulated CAN bus ==\n");

    roundtrip(1,    0, ISOTP_OK, "single frame, 1 byte");
    roundtrip(7,    0, ISOTP_OK, "single frame, 7 bytes (max SF)");
    roundtrip(8,    0, ISOTP_OK, "segmented, 8 bytes (min FF)");
    roundtrip(13,   0, ISOTP_OK, "segmented, 13 bytes (exact CF fill)");
    roundtrip(199,  0, ISOTP_OK, "segmented, 199 bytes (largest DID)");
    roundtrip(255,  0, ISOTP_OK, "segmented, 255 bytes");
    /* >15 consecutive frames forces the 4-bit sequence number to wrap. */
    roundtrip(1000, 0, ISOTP_OK, "segmented, 1000 bytes (SN wraps)");
    roundtrip(ISOTP_MAX_PAYLOAD, 0, ISOTP_OK, "segmented, 4095 bytes (max)");

    /* A lost consecutive frame must surface as an error, never as a short or
     * silently corrupted payload. */
    roundtrip(200, 3, ISOTP_ERR_PROTOCOL, "dropped CF is detected");

    printf("\n== bounded under a busy bus ==\n");
    stalled_receive();
    endless_wait_fc();

    printf("\n== STmin encoding ==\n");
    /* Values above 0x7F are the microsecond range; anything reserved must fall
     * back to the 127 ms maximum rather than being taken literally. */
    struct { uint8_t raw; uint32_t us; const char *what; } cases[] = {
        { 0x00, 0,      "0 ms" },
        { 0x0A, 10000,  "10 ms (open3e default)" },
        { 0x7F, 127000, "127 ms (max ms)" },
        { 0xF1, 100,    "100 us" },
        { 0xF9, 900,    "900 us" },
        { 0x80, 127000, "reserved -> 127 ms" },
        { 0xFA, 127000, "reserved -> 127 ms" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t got = isotp_stmin_to_us(cases[i].raw);
        printf("  0x%02X -> %6u us  %s\n", cases[i].raw, got, cases[i].what);
        check(got == cases[i].us, "STmin 0x%02X: expected %u us, got %u",
              cases[i].raw, cases[i].us, got);
    }

    printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
