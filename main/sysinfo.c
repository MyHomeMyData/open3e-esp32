#include "sysinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Previous sample, so CPU shares describe the interval between two reads
 * rather than an average since boot -- an average would hide a task that only
 * misbehaves during a scan. */
typedef struct {
    UBaseType_t number;
    uint32_t    runtime;
} prev_task_t;

static prev_task_t prev[SYSINFO_MAX_TASKS];
static size_t      n_prev;
static uint32_t    prev_total;
static int64_t     prev_us;

static uint32_t previous_runtime_of(UBaseType_t number)
{
    for (size_t i = 0; i < n_prev; i++) {
        if (prev[i].number == number) {
            return prev[i].runtime;
        }
    }
    return 0;
}

static char state_char(eTaskState s)
{
    switch (s) {
    case eRunning:   return 'R';
    case eReady:     return 'r';
    case eBlocked:   return 'B';
    case eSuspended: return 'S';
    case eDeleted:   return 'D';
    default:         return '?';
    }
}

static int by_cpu_desc(const void *a, const void *b)
{
    const sysinfo_task_t *x = a, *y = b;
    if (x->cpu_permille != y->cpu_permille) {
        return y->cpu_permille - x->cpu_permille;
    }
    return (int)y->stack_free - (int)x->stack_free;
}

size_t sysinfo_read(sysinfo_t *out, sysinfo_task_t *tasks, size_t max)
{
    memset(out, 0, sizeof(*out));

    out->heap_int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_int_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->heap_int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out->heap_psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->heap_psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    TaskStatus_t *snap = calloc(SYSINFO_MAX_TASKS, sizeof(*snap));
    if (!snap) {
        return 0;
    }
    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(snap, SYSINFO_MAX_TASKS, &total);
    out->n_tasks = n;

    int64_t now_us = esp_timer_get_time();
    /* The run-time counter wraps; a sample taken after a wrap would produce
     * nonsense, so it is discarded rather than shown. */
    bool have_prev = n_prev > 0 && total >= prev_total;
    uint32_t window = total - prev_total;
    out->cpu_available = have_prev && window > 0;
    out->window_ms = have_prev ? (uint32_t)((now_us - prev_us) / 1000) : 0;

    size_t count = 0;
    for (UBaseType_t i = 0; i < n && count < max; i++) {
        sysinfo_task_t *t = &tasks[count++];
        memset(t, 0, sizeof(*t));
        snprintf(t->name, sizeof(t->name), "%s",
                 snap[i].pcTaskName ? snap[i].pcTaskName : "?");
        t->priority = (uint8_t)snap[i].uxCurrentPriority;
        /* usStackHighWaterMark counts words, not bytes. */
        t->stack_free = (uint32_t)snap[i].usStackHighWaterMark * sizeof(StackType_t);
        t->state = state_char(snap[i].eCurrentState);
#if ( configTASKLIST_INCLUDE_COREID == 1 )
        t->core = (snap[i].xCoreID == tskNO_AFFINITY) ? -1 : (int8_t)snap[i].xCoreID;
#else
        t->core = -1;
#endif
        if (out->cpu_available) {
            uint32_t delta = snap[i].ulRunTimeCounter -
                             previous_runtime_of(snap[i].xTaskNumber);
            t->cpu_permille = (uint16_t)((uint64_t)delta * 1000 / window);
            if (t->cpu_permille > 1000) {
                t->cpu_permille = 1000;   /* two cores can exceed one core's worth */
            }
        }
    }

    n_prev = n < SYSINFO_MAX_TASKS ? n : SYSINFO_MAX_TASKS;
    for (size_t i = 0; i < n_prev; i++) {
        prev[i].number = snap[i].xTaskNumber;
        prev[i].runtime = snap[i].ulRunTimeCounter;
    }
    prev_total = total;
    prev_us = now_us;
    free(snap);

    qsort(tasks, count, sizeof(*tasks), by_cpu_desc);
    return count;
}
