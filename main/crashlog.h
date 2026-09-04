/* What the last reboot was, and where it came from.
 *
 * A device in a boiler room reboots and the only trace is that the uptime
 * started over. That was the position this project kept ending up in: a fault
 * that only appears with real bus traffic, on hardware that is not next to a
 * serial monitor, diagnosed by guessing and reflashing.
 *
 * So the panic handler writes a core dump to its own flash partition, and this
 * reads the useful part of it back: the faulting task, the program counter,
 * the exception cause and address, and a backtrace. Those are addresses, not
 * names -- resolving them needs the ELF of that exact build, which is why the
 * report carries the build's own checksum to compare against.
 *
 *     xtensa-esp32s3-elf-addr2line -pfiaC -e build/open3e-gateway.elf 0x4200...
 */
#ifndef O3E_CRASHLOG_H
#define O3E_CRASHLOG_H

#include <stdbool.h>
#include <stddef.h>

/* Why the chip started: power-on, a deliberate restart, a panic, a watchdog,
 * or a brownout. Available on every boot, with or without a dump. */
const char *crashlog_reset_reason(void);

/* True when the previous run left a core dump behind. */
bool crashlog_present(void);

/* Writes a JSON object with the reset reason and, when there is a dump, the
 * faulting task and its backtrace. Always produces valid JSON. */
void crashlog_json(char *out, size_t out_sz);

/* Erases the stored dump, so the next crash is unmistakably the next one. */
bool crashlog_clear(void);

#endif /* O3E_CRASHLOG_H */
