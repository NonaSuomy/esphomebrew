#!/usr/bin/env python3
"""Serialize destructive ESP32-P4 development tests per device.

Examples:
  python3 tools/device_test_queue.py status --device 10.20.30.180
  python3 tools/device_test_queue.py run --device 10.20.30.180 \
      --owner quake2 -- python3 tools/run_q2_test.py

The lock is process-owned by flock, so a crashed or interrupted test cannot
leave the device permanently busy. Metadata is only informational and is
removed when the lease exits.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path


LOCK_ROOT = Path(os.environ.get("ESP32_TEST_QUEUE_DIR", "/tmp/esphomebrew-device-queue"))


def lock_paths(device: str) -> tuple[Path, Path]:
    key = hashlib.sha256(device.encode("utf-8")).hexdigest()[:16]
    return LOCK_ROOT / f"{key}.lock", LOCK_ROOT / f"{key}.json"


def read_owner(meta: Path) -> dict:
    try:
        return json.loads(meta.read_text())
    except (OSError, ValueError):
        return {}


def write_owner(meta: Path, device: str, owner: str, command: list[str]) -> None:
    payload = {
        "device": device,
        "owner": owner,
        "pid": os.getpid(),
        "host": socket.gethostname(),
        "started": time.time(),
        "heartbeat": time.time(),
        "phase": "running",
        "last_output": "",
        "command": command,
    }
    tmp = meta.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(payload, indent=2) + "\n")
    tmp.replace(meta)


def update_owner(meta: Path, **changes: object) -> None:
    payload = read_owner(meta)
    payload.update(changes)
    payload["heartbeat"] = time.time()
    tmp = meta.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(payload, indent=2) + "\n")
    tmp.replace(meta)


def acquire(device: str, owner: str, command: list[str] | None, wait: bool) -> tuple[object, Path]:
    LOCK_ROOT.mkdir(parents=True, exist_ok=True)
    lock_path, meta_path = lock_paths(device)
    lock = lock_path.open("a+")
    flags = fcntl.LOCK_EX
    if not wait:
        flags |= fcntl.LOCK_NB
    try:
        fcntl.flock(lock.fileno(), flags)
    except BlockingIOError:
        info = read_owner(meta_path)
        print(json.dumps({"busy": True, **info}, indent=2), file=sys.stderr)
        lock.close()
        raise
    write_owner(meta_path, device, owner, command or [])
    return lock, meta_path


def release(lock: object, meta_path: Path) -> None:
    try:
        meta_path.unlink(missing_ok=True)
    finally:
        fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
        lock.close()


def cmd_status(device: str) -> int:
    LOCK_ROOT.mkdir(parents=True, exist_ok=True)
    lock_path, meta_path = lock_paths(device)
    lock = lock_path.open("a+")
    try:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print(json.dumps({"busy": True, **read_owner(meta_path)}, indent=2))
            return 0
        print(json.dumps({"busy": False, "device": device}, indent=2))
        return 0
    finally:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
        finally:
            lock.close()


def cmd_run(args: argparse.Namespace) -> int:
    if not args.command:
        raise SystemExit("run requires a command after --")
    try:
        lock, meta = acquire(args.device, args.owner, args.command, args.wait)
    except BlockingIOError:
        return 2
    print(f"device lease acquired: {args.device} owner={args.owner}", flush=True)
    try:
        process = subprocess.Popen(
            args.command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1,
        )
        assert process.stdout is not None
        for line in process.stdout:
            update_owner(meta, last_output=line.rstrip()[-500:])
            print(line, end="", flush=True)
        return process.wait()
    finally:
        release(lock, meta)
        print(f"device lease released: {args.device}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)

    status = sub.add_parser("status", help="show the current device lease")
    status.add_argument("--device", required=True)

    run = sub.add_parser("run", help="wait for the device and run one test command")
    run.add_argument("--device", required=True)
    run.add_argument("--owner", required=True, help="chat/app name, e.g. quake2")
    run.add_argument("--no-wait", dest="wait", action="store_false", help="fail if busy")
    run.set_defaults(wait=True)
    run.add_argument("command", nargs=argparse.REMAINDER, help="command after --")

    args = parser.parse_args()
    if args.action == "status":
        return cmd_status(args.device)
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    return cmd_run(args)


if __name__ == "__main__":
    raise SystemExit(main())
