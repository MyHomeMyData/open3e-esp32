/* UDS request/response against a simulated ECU.
 *
 * This exists because of a specific bug: requests used to be handed to a CAN
 * task through a FreeRTOS queue, which copies by value, so the task filled in
 * the result on its own copy while callers read back their own zero-initialised
 * one -- and a zeroed uds_result_t reads as UDS_OK. Every request appeared to
 * succeed, so a bus scan "found" a device at every address it probed and a
 * datapoint behind almost every DID.
 *
 * The property that broke is simple and worth pinning down: a request that is
 * not answered, or answered negatively, must not report success.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../main/uds.h"

#define QMAX 512

typedef struct {
    uint32_t id;
    uint8_t  data[8];
    uint8_t  len;
} frame_t;

static struct {
    frame_t         q[QMAX];
    size_t          n;
    pthread_mutex_t lock;
    pthread_cond_t  cv;
} bus;

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

static bool bus_send(void *ctx, uint32_t id, const uint8_t *data, uint8_t len)
{
    (void)ctx;
    pthread_mutex_lock(&bus.lock);
    if (bus.n < QMAX) {
        frame_t *f = &bus.q[bus.n++];
        f->id = id;
        f->len = len;
        memcpy(f->data, data, len);
    }
    pthread_cond_broadcast(&bus.cv);
    pthread_mutex_unlock(&bus.lock);
    return true;
}

static bool bus_recv(void *ctx, uint32_t *id, uint8_t *data, uint8_t *len, uint32_t timeout_ms)
{
    uint32_t want = *(uint32_t *)ctx;
    uint32_t deadline = now_ms(NULL) + timeout_ms;

    pthread_mutex_lock(&bus.lock);
    for (;;) {
        for (size_t i = 0; i < bus.n; i++) {
            if (bus.q[i].id != want) {
                continue;
            }
            *id = bus.q[i].id;
            *len = bus.q[i].len;
            memcpy(data, bus.q[i].data, bus.q[i].len);
            memmove(&bus.q[i], &bus.q[i + 1], (bus.n - i - 1) * sizeof(frame_t));
            bus.n--;
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

static uint32_t tester_rx = 0x690, ecu_rx = 0x680;

static const isotp_hal_t hal_tester = {
    .send = bus_send, .recv = bus_recv, .delay_us = delay_us,
    .now_ms = now_ms, .ctx = &tester_rx,
};
static const isotp_hal_t hal_ecu = {
    .send = bus_send, .recv = bus_recv, .delay_us = delay_us,
    .now_ms = now_ms, .ctx = &ecu_rx,
};

/* What the simulated ECU should do with the next request. */
typedef enum { ECU_ANSWER, ECU_SILENT, ECU_NEGATIVE, ECU_PENDING_THEN_ANSWER } ecu_mode_t;

static ecu_mode_t ecu_mode;
static uint16_t   ecu_did;
static uint8_t    ecu_nrc;

/* A 36-byte BusIdentification record, as DID 256 would return it. */
static void make_identification(uint8_t *out)
{
    memset(out, 0, 36);
    out[0] = 1;      /* BusAddress */
    out[1] = 2;      /* BusType: CanInternal */
    out[2] = 9;      /* DeviceProperty */
    out[3] = 14;     /* DeviceFunction */
    memcpy(out + 20, "TESTSERIAL123456", 16);
}

static void *ecu_thread(void *arg)
{
    (void)arg;
    isotp_link_t link = { .hal = &hal_ecu, .tx_id = 0x690, .rx_id = 0x680, .stmin_ms = 0 };

    uint8_t req[ISOTP_MAX_PAYLOAD];
    size_t n = 0;
    if (isotp_recv(&link, req, sizeof(req), &n, 2000) != ISOTP_OK || n < 3) {
        return NULL;
    }
    ecu_did = (uint16_t)(req[1] << 8 | req[2]);

    if (ecu_mode == ECU_SILENT) {
        return NULL;   /* the address is empty: nothing comes back at all */
    }
    if (ecu_mode == ECU_NEGATIVE) {
        const uint8_t neg[3] = { UDS_NEGATIVE, UDS_SID_READ, ecu_nrc };
        isotp_send(&link, neg, sizeof(neg));
        return NULL;
    }
    if (ecu_mode == ECU_PENDING_THEN_ANSWER) {
        const uint8_t pend[3] = { UDS_NEGATIVE, UDS_SID_READ, UDS_NRC_PENDING };
        isotp_send(&link, pend, sizeof(pend));
    }

    uint8_t resp[3 + 36];
    resp[0] = UDS_RESP_READ;
    resp[1] = req[1];
    resp[2] = req[2];
    make_identification(resp + 3);
    isotp_send(&link, resp, sizeof(resp));
    return NULL;
}

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

static void scenario(ecu_mode_t mode, uint8_t nrc, uint16_t did,
                     uds_err_t want_err, const char *label)
{
    pthread_mutex_lock(&bus.lock);
    bus.n = 0;
    pthread_mutex_unlock(&bus.lock);
    ecu_mode = mode;
    ecu_nrc = nrc;

    pthread_t th;
    pthread_create(&th, NULL, ecu_thread, NULL);

    isotp_link_t link = { .hal = &hal_tester, .tx_id = 0x680, .rx_id = 0x690, .stmin_ms = 0 };
    uint8_t buf[ISOTP_MAX_PAYLOAD];
    /* Deliberately poisoned: a caller must never be handed stale buffer
     * contents as if they were a response. */
    memset(buf, 0xA5, sizeof(buf));
    size_t n = 12345;

    uds_result_t r = uds_read_did(&link, did, buf, sizeof(buf), &n, 400);
    pthread_join(th, NULL);

    printf("%-44s %s\n", label, uds_strerror(r));
    check(r.err == want_err, "%s: expected err %d, got %d (%s)",
          label, (int)want_err, (int)r.err, uds_strerror(r));

    if (want_err == UDS_OK) {
        check(n == 36, "%s: expected 36 payload bytes, got %zu", label, n);
        check(buf[0] == 1 && buf[2] == 9, "%s: payload was not the record we sent", label);
        check(memcmp(buf + 20, "TESTSERIAL123456", 16) == 0,
              "%s: serial did not survive the round trip", label);
    } else {
        /* The decisive property. `buf` is scratch for the transport and may
         * legitimately hold a negative response, so the length is what decides
         * whether anything in it counts as datapoint content -- and on failure
         * it must be zero rather than whatever the caller passed in. */
        check(n == 0, "%s: reported %zu payload bytes despite failing", label, n);
    }
}

int main(void)
{
    pthread_mutex_init(&bus.lock, NULL);
    pthread_cond_init(&bus.cv, NULL);

    printf("== UDS against a simulated ECU ==\n");
    scenario(ECU_ANSWER, 0, 256, UDS_OK,
             "DID 256 answered");
    scenario(ECU_PENDING_THEN_ANSWER, 0, 256, UDS_OK,
             "responsePending then answered");
    scenario(ECU_SILENT, 0, 999, UDS_ERR_TIMEOUT,
             "empty address stays silent");
    scenario(ECU_NEGATIVE, 0x31, 999, UDS_ERR_NEGATIVE,
             "requestOutOfRange is a failure");
    scenario(ECU_NEGATIVE, 0x22, 300, UDS_ERR_NEGATIVE,
             "conditionsNotCorrect is a failure");

    printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
