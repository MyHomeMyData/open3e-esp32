#!/usr/bin/env python3
"""Catch link errors in the firmware sources without an ESP-IDF toolchain.

The device-side files cannot be compiled on a workstation, so this checks the
things a linker would otherwise be the first to notice:

  * every local #include resolves to a file in main/
  * every function declared in a main/*.h has a definition in some main/*.c
  * every call to a project function (matched by our module prefixes) has a
    declaration reachable from that translation unit

It is a lint, not a compiler: it will not catch type errors. It does catch the
mistake that actually happens when writing a dozen modules in one sitting --
calling something that was renamed, or never written.
"""
import re
import sys
from pathlib import Path

MAIN = Path(__file__).resolve().parent.parent / "main"

# Functions belonging to this project, by module prefix.
PREFIXES = ("o3e_", "isotp_", "uds_", "can_", "e3_scan_", "ha_disco_",
            "mqtt_pub_", "mqtt_cmnd_", "net_prov_", "poller_", "httpd_api_",
            "app_config_", "wifi_cfg_", "mqtt_cfg_", "sys_cfg_")

DECL = re.compile(
    r"^[A-Za-z_][\w \t\*]*?\b(?P<name>[a-z_][\w]*)\s*\([^;{]*\)\s*;", re.M)
DEFN = re.compile(
    r"^(?P<static>static\s+)?[A-Za-z_][\w \t\*]*?\b(?P<name>[a-z_][\w]*)\s*\([^;{]*\)\s*\{", re.M)
CALL = re.compile(r"\b(?P<name>[a-z_][\w]*)\s*\(")
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def strip_comments(text: str) -> str:
    """Blank out comments without being fooled by string literals.

    A regex cannot do this: the wildcard route `{ "/*", HTTP_GET, ... }`
    contains /* inside a string, and a naive substitution treats it as the
    start of a comment and swallows the rest of the file. That silently blinded
    this checker to everything below it.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in ('"', "'"):
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    out.append("  ")
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            # keep newlines so line-anchored patterns still line up
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:end]))
            i = end
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
            continue
        out.append(c)
        i += 1
    return "".join(out)


def main() -> int:
    headers = {p.name: strip_comments(p.read_text()) for p in sorted(MAIN.glob("*.h"))}
    sources = {p.name: strip_comments(p.read_text()) for p in sorted(MAIN.glob("*.c"))}
    problems = []

    # 1. local includes resolve
    for name, text in {**headers, **sources}.items():
        for inc in INCLUDE.findall(text):
            if "/" in inc:
                continue                      # framework headers
            if inc in headers or inc in ("cJSON.h",):
                continue
            if (MAIN / inc).exists():
                continue
            if inc.startswith(("esp_", "freertos", "driver", "nvs", "mqtt_client",
                               "lwip", "mdns", "sdkconfig")):
                continue
            problems.append(f"{name}: includes \"{inc}\" which does not exist in main/")

    # 2. declared but never defined
    defined = set()
    for text in sources.values():
        for m in DEFN.finditer(text):
            if not m.group("static"):
                defined.add(m.group("name"))

    for hname, text in headers.items():
        for m in DECL.finditer(text):
            fn = m.group("name")
            if not fn.startswith(PREFIXES):
                continue
            if fn not in defined:
                problems.append(f"{hname}: declares {fn}() but no .c file defines it")

    # 3. called but never declared anywhere we can see
    declared = set(defined)
    for text in headers.values():
        declared |= {m.group("name") for m in DECL.finditer(text)}
    for text in sources.values():
        declared |= {m.group("name") for m in DECL.finditer(text)}
        declared |= {m.group("name") for m in DEFN.finditer(text)}

    for sname, text in sources.items():
        for m in CALL.finditer(text):
            fn = m.group("name")
            if not fn.startswith(PREFIXES) or fn in declared:
                continue
            problems.append(f"{sname}: calls {fn}() which is never declared")

    for p in sorted(set(problems)):
        print(f"  {p}")
    print(f"\n{len(headers)} headers, {len(sources)} sources, "
          f"{len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
