#!/usr/bin/env python3
"""Generate E380 test vectors using E3onCAN's own codecs.

The energy-meter frames go through the same C decoder as the open3e
datapoints, but two codec details differ upstream: E3onCAN's O3EFloat32
divides by a scale where open3e's does not, and O3EcosPhi has no open3e
counterpart at all. Both are easy to get subtly wrong, so they are measured
against the reference rather than eyeballed.
"""
import argparse
import json
import random
import sys
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--e3oncan", type=Path, default=root / ".cache" / "e3oncan")
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("-n", "--per-frame", type=int, default=40)
    ap.add_argument("--seed", type=int, default=380)
    args = ap.parse_args()

    sys.path.insert(0, str(args.e3oncan))
    import E3onCANcodecs as C
    import E3onCANdatapointsE380 as e380
    C.flag_rawmode = False

    rng = random.Random(args.seed)
    vectors = []
    for can_id, codec in sorted(e380.dataIdentifiersE380.items()):
        for i in range(args.per_frame):
            # The cosPhi sign byte is only ever 0x00 or 0x04 on the wire; steer
            # towards those so both branches are actually exercised.
            payload = bytearray(rng.randrange(256) for _ in range(codec.string_len))
            if can_id in (0x254, 0x255) and i % 2 == 0:
                payload[6] = rng.choice([0x00, 0x04])
            payload = bytes(payload)
            vectors.append({
                "canId": can_id,
                "name": codec.id,
                "payload": payload.hex(),
                "expected": json.dumps(codec.decode(payload), ensure_ascii=False),
            })

    args.output.write_text(json.dumps(vectors, indent=1, ensure_ascii=False),
                           encoding="utf-8")
    print(f"{len(vectors)} E380 vectors -> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
