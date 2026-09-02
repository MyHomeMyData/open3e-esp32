#!/usr/bin/env python3
"""Assemble the storage partition contents: datapoint database plus the web UI.

The web assets are stored gzip-compressed; httpd_api.c serves the .gz directly
with a Content-Encoding header, which saves both flash and transfer time on a
device whose uplink is a shared 2.4 GHz channel in a basement.

Output goes to storage_image/ at the project root, deliberately *not* into
build/: that directory belongs to CMake, and `idf.py set-target` refuses to
run its fullclean when it finds files there it did not create.
"""
import argparse
import gzip
import shutil
import subprocess
import sys
from pathlib import Path

# Files small enough that the gzip header costs more than it saves.
MIN_GZIP_BYTES = 512
GZIP_SUFFIXES = {".html", ".css", ".js", ".json", ".svg"}


def build_tree(data_dir: Path, web_dir: Path, out_dir: Path) -> None:
    if out_dir.exists():
        shutil.rmtree(out_dir)
    (out_dir / "www").mkdir(parents=True)

    db = data_dir / "o3edb.bin"
    if not db.exists():
        raise SystemExit(f"{db} is missing; run tools/gen_dpdb.py first")
    shutil.copy2(db, out_dir / "o3edb.bin")
    print(f"  o3edb.bin           {db.stat().st_size / 1024:8.0f} KiB")

    for src in sorted(web_dir.rglob("*")):
        if not src.is_file():
            continue
        rel = src.relative_to(web_dir)
        dst = out_dir / "www" / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        raw = src.read_bytes()
        if src.suffix in GZIP_SUFFIXES and len(raw) >= MIN_GZIP_BYTES:
            packed = gzip.compress(raw, 9)
            dst.with_suffix(dst.suffix + ".gz").write_bytes(packed)
            print(f"  www/{rel!s:<16} {len(raw) / 1024:8.1f} KiB -> "
                  f"{len(packed) / 1024:.1f} KiB gzip")
        else:
            dst.write_bytes(raw)
            print(f"  www/{rel!s:<16} {len(raw) / 1024:8.1f} KiB")


def make_image(tree: Path, image: Path, size: int) -> bool:
    """Pack the tree with mklittlefs if it is available.

    Without it the tree is still produced, and `idf.py littlefs-flash` (or the
    littlefs_create_partition_image CMake helper) can pack it instead.
    """
    if not shutil.which("mklittlefs"):
        print("\nmklittlefs not found - the file tree is ready at "
              f"{tree}\nFlash it with: idf.py littlefs-flash")
        return False
    subprocess.run(["mklittlefs", "-c", str(tree), "-b", "4096", "-p", "256",
                    "-s", str(size), str(image)], check=True)
    print(f"\n{image}  ({image.stat().st_size / 1024:.0f} KiB)")
    return True


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", type=Path, default=root / "data")
    ap.add_argument("--web", type=Path, default=root / "web")
    ap.add_argument("--out", type=Path, default=root / "storage_image")
    ap.add_argument("--image", type=Path, default=root / "storage_image.bin")
    # Must match the storage partition in partitions.csv.
    ap.add_argument("--size", type=lambda s: int(s, 0), default=0x400000)
    args = ap.parse_args()

    print("Storage partition contents:")
    build_tree(args.data, args.web, args.out)

    used = sum(f.stat().st_size for f in args.out.rglob("*") if f.is_file())
    print(f"\n  {used / 1024:.0f} KiB of {args.size / 1024:.0f} KiB partition "
          f"({100 * used / args.size:.0f}%)")
    if used > args.size:
        raise SystemExit("contents do not fit in the storage partition")

    args.image.parent.mkdir(parents=True, exist_ok=True)
    make_image(args.out, args.image, args.size)
    return 0


if __name__ == "__main__":
    sys.exit(main())
