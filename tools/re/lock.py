#!/usr/bin/env python3
"""Named cross-agent lock for a resource only one agent may drive at a time.

    lock.py acquire gui <owner> [--ttl 1200] [--wait 3600]
    lock.py refresh gui <owner>
    lock.py release gui <owner>
    lock.py status  gui

The screen (plugin editor windows, computer use) is such a resource. A lock that was
not refreshed within its ttl is treated as abandoned and taken over.
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["acquire", "refresh", "release", "status"])
    ap.add_argument("name")
    ap.add_argument("owner", nargs="?")
    ap.add_argument("--ttl", type=float, default=1200.0)
    ap.add_argument("--wait", type=float, default=3600.0)
    args = ap.parse_args()

    state = os.environ.get("RE_STATE") or sys.exit("set RE_STATE")
    lock = Path(state) / "locks" / args.name
    lock.parent.mkdir(parents=True, exist_ok=True)
    info = lock / "owner.json"

    def holder():
        try:
            return json.loads(info.read_text())
        except (OSError, ValueError):
            return None

    if args.action == "status":
        print(json.dumps(holder()) if lock.exists() else "free")
        return
    if not args.owner:
        ap.error("owner required")

    if args.action == "release":
        current = holder()
        if current and current["owner"] != args.owner:
            sys.exit(f"held by {current['owner']}, not released")
        info.unlink(missing_ok=True)
        if lock.exists():
            lock.rmdir()
        return

    if args.action == "refresh":
        current = holder()
        if not current or current["owner"] != args.owner:
            sys.exit("not the holder")
        info.write_text(json.dumps(dict(owner=args.owner, at=time.time(), ttl=current["ttl"])))
        return

    deadline = time.time() + args.wait
    while True:
        try:
            lock.mkdir()
            info.write_text(json.dumps(dict(owner=args.owner, at=time.time(), ttl=args.ttl)))
            print("acquired")
            return
        except FileExistsError:
            current = holder()
            if current and current["owner"] == args.owner:
                print("acquired")
                return
            stale = current is None and time.time() - lock.stat().st_mtime > 30
            if current and time.time() - current["at"] > current["ttl"]:
                stale = True
            if stale:
                info.unlink(missing_ok=True)
                try:
                    lock.rmdir()
                except OSError:
                    pass
                continue
        if time.time() > deadline:
            sys.exit(f"timed out waiting for lock {args.name} (held by {current})")
        time.sleep(5)


if __name__ == "__main__":
    main()
