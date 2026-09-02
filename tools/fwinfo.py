#!/usr/bin/env python3
"""Print the identity of a built firmware image.

Answers "is the build I just made the one running on the device?" -- compare
the build identity here with the one on the status page. The version string is
only as trustworthy as somebody's discipline in bumping it; the ELF checksum
changes with every changed byte of code.

The application descriptor sits at a fixed offset in the image, right after the
image and first segment headers.
"""
import argparse
import struct
import sys
from pathlib import Path

APP_DESC_OFFSET = 32          # esp_image_header_t (24) + esp_image_segment_header_t (8)
APP_DESC_MAGIC = 0xABCD5432


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", nargs="?", type=Path,
                    default=root / "build" / "open3e-gateway.bin")
    args = ap.parse_args()

    if not args.image.exists():
        print(f"{args.image} not found - run idf.py build first", file=sys.stderr)
        return 1

    raw = args.image.read_bytes()[APP_DESC_OFFSET:APP_DESC_OFFSET + 256]
    magic, secure, _r1, _r2 = struct.unpack_from("<IIII", raw, 0)
    if magic != APP_DESC_MAGIC:
        print("no application descriptor found; is this an ESP32 firmware image?",
              file=sys.stderr)
        return 1

    def text(off, size):
        return raw[off:off + size].split(b"\0")[0].decode("utf-8", "replace")

    version = text(16, 32)
    project = text(48, 32)
    btime = text(80, 16)
    bdate = text(96, 16)
    idf = text(112, 32)
    sha = raw[144:176].hex()

    print(f"{args.image}  ({len(args.image.read_bytes()) / 1024:.0f} KiB)")
    print(f"  project        {project}")
    print(f"  version        {version}")
    print(f"  built          {bdate} {btime}")
    print(f"  ESP-IDF        {idf}")
    print(f"  build identity {sha[:16]}")
    print()
    print(f"  Status page shows: {sha[:16]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
