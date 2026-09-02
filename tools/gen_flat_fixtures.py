#!/usr/bin/env python3
"""Generate expected MQTT topic/value pairs using open3e's own mqttdump().

Reuses the payloads from test/fixtures.json so both the JSON mode and the
flattened mode are checked against the same decoded values.
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

TZ = "Europe/Berlin"


def flatten(topic, obj, out):
    """Byte-for-byte the recursion in Open3Eclient.py:347."""
    if isinstance(obj, dict):
        for k, itm in obj.items():
            flatten(topic + "/" + str(k), itm, out)
    elif isinstance(obj, list):
        for k in range(len(obj)):
            flatten(topic + "/" + str(k), obj[k], out)
    else:
        out.append([topic, str(obj)])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--open3e", type=Path,
                    default=Path(__file__).resolve().parent.parent / ".cache" / "open3e")
    ap.add_argument("-i", "--input", type=Path, default=Path("test/fixtures.json"))
    ap.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args()

    os.environ["TZ"] = TZ
    time.tzset()

    sys.path.insert(0, str(args.open3e / "src"))
    import open3e.Open3Ecodecs as C
    import open3e.Open3Edatapoints as D
    C.flag_rawmode = False
    C.flag_binary = False

    dids = D.dataIdentifiers["dids"]
    vectors = json.loads(args.input.read_text(encoding="utf-8"))

    out = []
    for v in vectors:
        codec = dids[v["did"]]
        value = codec.decode(bytes.fromhex(v["payload"]))
        pairs = []
        flatten("open3e/" + codec.id, value, pairs)
        out.append({"did": v["did"], "payload": v["payload"], "pairs": pairs})

    args.output.write_text(json.dumps(out, indent=1, ensure_ascii=False), encoding="utf-8")
    total = sum(len(x["pairs"]) for x in out)
    print(f"{len(out)} datapoints, {total} topic/value pairs -> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
