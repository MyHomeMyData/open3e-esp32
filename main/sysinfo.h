/* Runtime load: what the chip is actually spending its time and memory on.
 *
 * Worth having on a device that has already been taken down twice by a task
 * doing blocking work in the wrong context and once by a stack that was sized
 * by guesswork. The stack headroom column is the one that would have caught
 * those before they became a panic.
 */
#ifndef O3E_SYSINFO_H
#define O3E_SYSINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SYSINFO_MAX_TASKS 24
#define SYSINFO_NAME_MAX  20

typedef struct {
    char     name[SYSINFO_NAME_MAX];
    uint16_t cpu_permille;   /* share of the interval since the last call */
    uint32_t stack_free;     /* bytes still unused at the deepest point so far */
    uint8_t  priority;
    int8_t   core;           /* 0, 1, or -1 when not pinned */
    char     state;          /* R)unning B)locked S)uspended D)eleted r)eady */
} sysinfo_task_t;

typedef struct {
    /* Internal RAM is the scarce one; the 8 MB of PSRAM makes a combined
     * figure look reassuring while the part that matters runs out. */
    uint32_t heap_int_free;
    uint32_t heap_int_min;
    uint32_t heap_int_largest;
    uint32_t heap_psram_free;
    uint32_t heap_psram_min;
    uint32_t n_tasks;
    /* Time covered by the CPU shares below, so the UI can say what they mean. */
    uint32_t window_ms;
    bool     cpu_available;  /* false until a second sample exists */
} sysinfo_t;

/* Fills `out` and up to `max` task entries, sorted by CPU share.
 * CPU shares are measured against the previous call, so polling it at a steady
 * interval gives load over that interval rather than an average since boot. */
size_t sysinfo_read(sysinfo_t *out, sysinfo_task_t *tasks, size_t max);

#endif /* O3E_SYSINFO_H */
