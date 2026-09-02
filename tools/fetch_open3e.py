#!/usr/bin/env python3
"""Fetch the open3e sources this firmware derives its datapoint database from.

The database is *not* vendored into this repo: it is regenerated at build time
from a pinned upstream commit.  Bumping to a newer open3e release is therefore a
one-line change to OPEN3E_REF below.

open3e is Apache-2.0 licensed; see NOTICE.
"""
import subprocess
import sys
from pathlib import Path

OPEN3E_URL = "https://github.com/open3e/open3e.git"
# Pin to a branch or, preferably, an exact commit for reproducible builds.
OPEN3E_REF = "main"

# E3onCAN adds the passively broadcast datapoints of the Viessmann energy
# meters, which open3e does not cover: the E380 answers no UDS request, it only
# announces itself on the bus.
E3ONCAN_URL = "https://github.com/MyHomeMyData/E3onCAN.git"
E3ONCAN_REF = "main"

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / ".cache" / "open3e"
CACHE_E3ONCAN = ROOT / ".cache" / "e3oncan"


def run(*args, **kw):
    return subprocess.run(args, check=True, **kw)


def fetch(url: str, ref: str, dest: Path, name: str) -> None:
    if dest.exists():
        print(f"{name} already present at {dest}")
        if ref != "main":
            run("git", "-C", str(dest), "fetch", "--depth", "1", "origin", ref)
            run("git", "-C", str(dest), "checkout", "--quiet", "FETCH_HEAD")
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"cloning {name}@{ref} -> {dest}")
    run("git", "clone", "--depth", "1", "--branch", ref, url, str(dest))


def main() -> int:
    fetch(OPEN3E_URL, OPEN3E_REF, CACHE, "open3e")
    fetch(E3ONCAN_URL, E3ONCAN_REF, CACHE_E3ONCAN, "E3onCAN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
