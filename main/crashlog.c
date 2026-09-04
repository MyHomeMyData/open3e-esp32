#include "crashlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "crash";

const char *crashlog_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "Einschalten";
    case ESP_RST_EXT:       return "Reset-Pin";
    case ESP_RST_SW:        return "Neustart aus der Software";
    case ESP_RST_PANIC:     return "Absturz";
    case ESP_RST_INT_WDT:   return "Interrupt-Watchdog";
    case ESP_RST_TASK_WDT:  return "Task-Watchdog";
    case ESP_RST_WDT:       return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Tiefschlaf";
    case ESP_RST_BROWNOUT:  return "Unterspannung";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unbekannt";
    }
}

bool crashlog_present(void)
{
    return esp_core_dump_image_check() == ESP_OK;
}

bool crashlog_clear(void)
{
    esp_err_t rc = esp_core_dump_image_erase();
    if (rc == ESP_OK) {
        ESP_LOGI(TAG, "core dump erased");
    }
    return rc == ESP_OK;
}

void crashlog_json(char *out, size_t out_sz)
{
    size_t o = (size_t)snprintf(out, out_sz, "{\"reason\": \"%s\", \"dump\": ",
                                crashlog_reset_reason());

    /* The summary is nearly a kilobyte of registers; it does not belong on the
     * stack of whichever task happens to ask. */
    esp_core_dump_summary_t *s = malloc(sizeof(*s));
    if (!s || esp_core_dump_get_summary(s) != ESP_OK) {
        free(s);
        snprintf(out + o, out_sz - o, "null}");
        return;
    }

    char sha[sizeof(s->app_elf_sha256) + 1];
    snprintf(sha, sizeof(sha), "%s", (const char *)s->app_elf_sha256);

    o += (size_t)snprintf(out + o, out_sz - o,
                          "{\"task\": \"%.16s\", \"pc\": \"0x%08x\", "
                          "\"cause\": %u, \"vaddr\": \"0x%08x\", "
                          "\"elfSha\": \"%.16s\", \"corrupted\": %s, \"bt\": [",
                          s->exc_task, (unsigned)s->exc_pc,
                          (unsigned)s->ex_info.exc_cause,
                          (unsigned)s->ex_info.exc_vaddr, sha,
                          s->exc_bt_info.corrupted ? "true" : "false");

    for (uint32_t i = 0; i < s->exc_bt_info.depth && o + 16 < out_sz; i++) {
        o += (size_t)snprintf(out + o, out_sz - o, "%s\"0x%08x\"",
                              i ? ", " : "", (unsigned)s->exc_bt_info.bt[i]);
    }
    snprintf(out + o, out_sz - o, "]}}");
    free(s);
}
