#!/usr/bin/env python3
"""Generate write-path test vectors using open3e's own encode().

Takes the decoded values from test/fixtures.json, feeds each back through the
Python codec's encode(), and records the bytes it produced.  test/test_encode.c
then checks that the C encoder emits the same bytes for the same JSON input --
the write path gets the same treatment as the read path, which matters more
here because these bytes end up in a heat pump.

Datapoints whose codec open3e cannot encode ("not implemented yet") are
recorded separately: the C port must refuse them too, rather than inventing an
encoding of its own.
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import db_overrides  # noqa: E402

TZ = "Europe/Berlin"


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

    # Same corrections the firmware's database carries, so the C port and

    # open3e are compared on identical definitions.

    n = db_overrides.apply_to_codecs(dids, C)

    print(f"applied {n} sensor error enums", file=sys.stderr)
    vectors = json.loads(args.input.read_text(encoding="utf-8"))

    out, refused = [], []
    seen_refused = set()
    for v in vectors:
        did = v["did"]
        codec = dids[did]
        value = codec.decode(bytes.fromhex(v["payload"]))
        try:
            encoded = codec.encode(value)
        except Exception as exc:  # noqa: BLE001
            if did not in seen_refused:
                seen_refused.add(did)
                refused.append({"did": did, "codec": type(codec).__name__,
                                "value": json.dumps(value, ensure_ascii=False),
                                "reason": str(exc)[:120]})
            continue
        out.append({
            "did": did,
            "codec": type(codec).__name__,
            "value": json.dumps(value, ensure_ascii=False),
            "expected": encoded.hex(),
        })

    args.output.write_text(
        json.dumps({"encodable": out, "refused": refused}, indent=1, ensure_ascii=False),
        encoding="utf-8")
    print(f"{len(out)} encodable vectors, {len(refused)} datapoints open3e refuses "
          f"-> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
