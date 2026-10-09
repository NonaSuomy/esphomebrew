#!/usr/bin/env python3
"""Run inside device_test_queue.py with a QUAKE2_TEST_NEWGAME build served.

The test sends Enter inside the game task. Optional L1 injection uses the
quick firmware's shared gamepad path. Capture LAN logs and clean exit.
"""
import argparse
import asyncio
import os
from pathlib import Path
import re
import json
import selectors
import subprocess
import sys
import time
from aioesphomeapi import APIClient, LogLevel


def write_performance(output, video_layout, perf_samples, windows, checks):
    """Keep measurements even if the optional network capture fails."""
    samples = perf_samples[:windows] if windows else perf_samples
    (output / "performance.json").write_text(json.dumps({
        "video": video_layout, "samples": samples, "checks": checks,
        "fps": sum(s["frames"] for s in samples) * 1000000 /
               sum(s["elapsed_us"] for s in samples) if samples else None,
    }, indent=2) + "\n")


async def ensure_idle(device):
    """Close the previous app and await the loader's actual heap release."""
    client = APIClient(device, 6053, "", keepalive=10)
    for attempt in range(30):
        try:
            await client.connect()
            break
        except Exception:
            if attempt == 29:
                raise
            await asyncio.sleep(1)
    ended = asyncio.Event()
    idle = asyncio.Event()

    def on_log(message):
        line = message.message.decode("utf-8", "replace")
        if "no app is running" in line:
            idle.set()
            ended.set()
        elif "Heap after the app ended" in line:
            ended.set()

    try:
        entities, _ = await client.list_entities_services()
        close = next(e for e in entities if e.object_id == "remote_close_pax")
        unsubscribe = client.subscribe_logs(on_log, log_level=LogLevel.LOG_LEVEL_INFO, dump_config=False)
        await asyncio.sleep(0.3)
        # Exiting a game may return to its parent launcher. Heap release is
        # not sufficient: keep closing the chain until the loader confirms
        # there is no app running, with time for each hand-off to settle.
        for _ in range(10):
            ended.clear()
            client.button_command(close.key)
            await asyncio.wait_for(ended.wait(), 35)
            await asyncio.sleep(1)
            if idle.is_set():
                break
        else:
            raise RuntimeError("Previous PAX chain did not return to idle")
        unsubscribe()
        print("phase=previous PAX closed; loader idle", flush=True)
    finally:
        await client.disconnect()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="10.20.30.180")
    parser.add_argument("--url", default="http://10.20.30.158:8000/quake2-smoke.pax")
    parser.add_argument("--output", default="/tmp/quake2-newgame")
    parser.add_argument("--timeout", type=int, default=360)
    parser.add_argument("--audio-check", action="store_true",
                        help="Require three consecutive healthy 5 s audio windows in the playable map")
    parser.add_argument("--skip-capture", action="store_true",
                        help="Verify rendered-frame logs and audio without the optional screen-stream connection")
    parser.add_argument("--music-check", action="store_true",
                        help="Require decoded music and two healthy decoder/ring windows in the playable map")
    parser.add_argument("--input-check", action="store_true",
                        help="Inject two L1 presses through the shared pad path and verify F1 press/release events")
    parser.add_argument("--death-check", action="store_true",
                        help="Require death -> load menu -> B/back -> reopen -> A/autosave reload")
    parser.add_argument("--perf-windows", type=int, default=0,
                        help="Require this many 5 s playable-map FPS samples (exclude screen capture)")
    parser.add_argument("--expect-layout", choices=("baseline", "sram-depth"),
                        help="Verify actual buffer placement, not just the build flag")
    args = parser.parse_args()
    if args.perf_windows < 0:
        parser.error("--perf-windows must not be negative")
    asyncio.run(ensure_idle(args.device))
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    tools = Path("/home/nonasuomy/code/ESP32-GameEmulation/RetroESP32-P4/tools")
    control = [sys.executable, "-u", str(tools / "remote_pax.py"), args.device]
    process = subprocess.Popen(control + [args.url], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, bufsize=0)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    pending = b""
    map_seen = False
    map_frames = 0
    captured = False
    closing = False
    stopped = False
    heap_released = False
    third_enter = False
    healthy_audio = 0
    heard_audio = False
    healthy_music = 0
    decoded_music = False
    input_sent = 0
    input_next = 0
    input_events = []
    death_step = 0
    death_pending = None
    video_layout = None
    perf_samples = []
    deadline = time.monotonic() + args.timeout
    try:
        with (output / "device.log").open("w") as log:
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"LAN monitor exited {process.returncode}")
                for key, _ in selector.select(1):
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        raise RuntimeError("LAN log connection closed")
                    pending += chunk
                    lines = pending.split(b"\n")
                    pending = lines.pop()
                    for raw in lines:
                        line = raw.decode("utf-8", "replace")
                        log.write(line + "\n")
                        log.flush()
                        clean = re.sub(r"\x1b\[[0-9;]*m", "", line)
                        if any(s in clean for s in ("QUAKE2", "Heap ", "Map:", "PAX stopped", "Server Initialization", "heap: no memory", "FS_BIG", "App hand-off", "worker returned", "Released launcher", "Network PAX")):
                            print(clean, flush=True)
                        if any(s in clean for s in ("QUAKE2 ERROR", "Guru Meditation", "assert failed:", "out of memory for", "Network PAX load failed", "Network PAX data download failed")):
                            raise RuntimeError(clean)
                        if "QUAKE2 TEST ENTER 3 down=1" in clean:
                            third_enter = True
                        if third_enter and "Map: base1" in clean:
                            map_seen = True
                            print("phase=loading first playable map", flush=True)
                        layout = re.search(r"QUAKE2 VIDEO layout=(\S+) color=(\S+) z=(\S+) rgb=(\S+) extra_psram=(\d+)", clean)
                        if layout:
                            video_layout = dict(zip(("layout", "color", "z", "rgb", "extra_psram"), layout.groups()))
                            if args.expect_layout and video_layout["layout"] != args.expect_layout:
                                raise RuntimeError(f"unexpected video buffer layout: {video_layout}")
                            if args.expect_layout == "sram-depth" and not video_layout["z"].endswith("/SRAM"):
                                raise RuntimeError(f"depth buffer did not land in SRAM: {video_layout}")
                        perf = re.search(r"QUAKE2 PERF frames=(\d+) elapsed_us=(\d+) fps=([\d.]+) present_ms=([\d.]+)", clean)
                        if map_seen and perf:
                            frames, elapsed, fps, present = perf.groups()
                            perf_samples.append({"frames": int(frames), "elapsed_us": int(elapsed),
                                                 "fps": float(fps), "present_ms": float(present)})
                        if map_seen and "QUAKE2 FRAME" in clean:
                            map_frames += 1
                        stream = re.search(r"AUDIO STREAM blocks=\d+ rate=(\d+) Hz underrun=(\d+)/(\d+) peak=(\d+)", clean)
                        if map_seen and stream:
                            rate, underrun, total, peak = map(int, stream.groups())
                            heard_audio |= peak > 0
                            healthy_audio = healthy_audio + 1 if 21600 <= rate <= 22500 and underrun == 0 else 0
                            print(f"phase=audio verification healthy_windows={healthy_audio} rate={rate} underrun={underrun}/{total}", flush=True)
                        if map_seen and "QUAKE2 MUSIC decoded" in clean:
                            decoded_music = True
                        if map_seen and args.music_check and "QUAKE2 MUSIC missing" in clean:
                            raise RuntimeError("playable-map music file is missing: " + clean)
                        if args.death_check:
                            if "QUAKE2 TEST DEATH ready" in clean and death_step == 0:
                                death_step = 1
                                death_pending = "a"
                            menu = re.search(r"TEST MENU menu=(\d) load=(\d) health=(-?\d+)", clean)
                            if menu:
                                active, load, health = map(int, menu.groups())
                                if death_step == 1 and active and load:
                                    death_step = 2
                                    death_pending = "b"
                                    print("phase=death load menu; testing B/back", flush=True)
                                elif death_step == 2 and not active:
                                    death_step = 3
                                    death_pending = "a"
                                    print("phase=B escaped the death load menu without quitting", flush=True)
                                elif death_step == 3 and active and load:
                                    death_step = 4
                                    death_pending = "a"
                                    print("phase=load menu reopened; selecting autosave with A", flush=True)
                            if death_step == 4 and "QUAKE2 TEST DEATH recovered health=" in clean:
                                death_step = 5
                                map_frames = healthy_audio = healthy_music = 0
                                print("phase=autosave restored living player; verifying recovered gameplay/audio", flush=True)
                        shoulder = re.search(r"INPUT L1 -> F1 down=([01])", clean)
                        if shoulder:
                            input_events.append(int(shoulder.group(1)))
                        music = re.search(r"MUSIC STREAM track=\d+ decode_cpu=([\d.]+)% mixed=(\d+) underrun=(\d+) lead=(\d+)", clean)
                        if map_seen and music:
                            cpu, mixed, underrun, lead = music.groups()
                            healthy_music = healthy_music + 1 if int(mixed) > 0 and int(underrun) == 0 else 0
                            print(f"phase=music verification healthy_windows={healthy_music} decode_cpu={cpu}% lead={lead}", flush=True)
                        if "=== Quake 2 PAX stopped ===" in clean:
                            stopped = True
                        if "Heap after the app ended" in clean:
                            heap_released = True
                        if stopped and heap_released:
                            if not captured:
                                raise RuntimeError("game exited before playable-map capture")
                            if args.expect_layout and not video_layout:
                                raise RuntimeError("missing video buffer placement diagnostics")
                            print("PASS: New Game -> gameplay/audio" +
                                  (" -> death/load menu -> B/back -> A/autosave reload -> recovered gameplay/audio" if args.death_check else "") +
                                  " -> clean exit", flush=True)
                            return 0
                if (args.input_check and map_frames >= 1 and len(perf_samples) >= args.perf_windows
                        and input_sent < 2 and time.monotonic() >= input_next):
                    subprocess.run(control + ["ui-press", "l", "--no-monitor"], check=True, timeout=20)
                    input_sent += 1
                    input_next = time.monotonic() + 2
                if death_pending:
                    subprocess.run(control + ["ui-press", death_pending, "--no-monitor"], check=True, timeout=20)
                    death_pending = None
                if (map_frames >= 3 and (not args.audio_check or healthy_audio >= 3 and heard_audio)
                        and (not args.music_check or decoded_music and healthy_music >= 2)
                        and (not args.death_check or death_step == 5)
                        and len(perf_samples) >= args.perf_windows
                        and (not args.input_check or input_events[:4] == [1, 0, 1, 0]) and not captured):
                    print("phase=verified playable-map frames and audio", flush=True)
                    if not args.skip_capture:
                        subprocess.run([sys.executable, str(tools / "record_screen.py"),
                                        args.device, "--screenshot", str(output / "base1.png")],
                                       check=True, timeout=40)
                        subprocess.run([sys.executable, str(tools / "record_screen.py"),
                                        args.device, "--duration", "12" if args.audio_check else "6", "--output", str(output / "base1.mp4")],
                                       check=True, timeout=45)
                    captured = True
                if captured and not closing:
                    subprocess.run(control + ["close", "--no-monitor"], check=True, timeout=20)
                    closing = True
            raise RuntimeError("timed out waiting for New Game and clean exit")
    finally:
        selector.close()
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        # Fixed pre-input windows keep comparisons independent of capture
        # latency. Save after stopping the monitor so a disk/write failure
        # cannot leak a logging process beyond the queue lease. A partial
        # result is diagnostic data, not a passing test.
        write_performance(output, video_layout, perf_samples, args.perf_windows, {
            "playable_map": map_seen, "healthy_audio_windows": healthy_audio,
            "heard_audio": heard_audio, "healthy_music_windows": healthy_music,
            "decoded_music": decoded_music, "input_events": input_events,
            "capture_requested": not args.skip_capture,
            "capture_completed": captured and not args.skip_capture,
            "clean_exit_observed": stopped and heap_released,
        })


if __name__ == "__main__":
    raise SystemExit(main())
