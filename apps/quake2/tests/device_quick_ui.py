#!/usr/bin/env python3
"""Verify quick-launcher pad navigation using its shared input injection path.

Run under the device queue after uploading the three-button quick firmware.
The last A press opens the NETWORK launcher; no GitHub download is started.
"""
import argparse
import asyncio
from pathlib import Path
import re
import sys
import time
from aioesphomeapi import APIClient, LogLevel


async def main(args):
    client = APIClient(args.device, 6053, "", keepalive=10)
    for attempt in range(45):
        try:
            await client.connect()
            break
        except Exception:
            if attempt == 44:
                raise
            await asyncio.sleep(1)
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    seen = []
    try:
        info = await client.device_info()
        assert info.name == "esp32-p4-elecrow-pax-quick", info.name
        print(f"Quick firmware: {info.compilation_time}", flush=True)
        entities, _ = await client.list_entities_services()
        control = next(e for e in entities if e.object_id == "remote_launcher_ui_input")
        close = next(e for e in entities if e.object_id == "remote_close_pax")
        with (output / "ui.log").open("w") as log:
            def on_log(message):
                clean = re.sub(r"\x1b\[[0-9;]*m", "", message.message.decode("utf-8", "replace"))
                seen.append(clean)
                log.write(clean + "\n")
                log.flush()
                if "Quick launcher" in clean or "ERROR" in clean:
                    print(clean, flush=True)

            unsubscribe = client.subscribe_logs(on_log, log_level=LogLevel.LOG_LEVEL_INFO)
            await asyncio.sleep(2)

            async def press(button, expected):
                first = len(seen)
                client.text_command(control.key, f"@{time.monotonic_ns()} press {button}")
                for _ in range(50):
                    await asyncio.sleep(0.1)
                    if any(expected in line for line in seen[first:]):
                        await asyncio.sleep(0.3)  # Release before the next edge.
                        return
                raise RuntimeError(f"No {expected!r} after {button!r}")

            await press("right", "Quick launcher focus: NETWORK")
            await press("right", "Quick launcher focus: GITHUB STORE")
            await press("left", "Quick launcher focus: NETWORK")
            await press("left", "Quick launcher focus: STORAGE")
            await press("right", "Quick launcher focus: NETWORK")
            capture = await asyncio.create_subprocess_exec(
                sys.executable,
                "/home/nonasuomy/code/ESP32-GameEmulation/RetroESP32-P4/tools/record_screen.py",
                args.device, "--screenshot", str(output / "three-buttons.png"))
            result = await asyncio.wait_for(capture.wait(), 30)
            assert result == 0, f"screenshot exited {result}"
            await press("a", "Quick launcher controller activate")
            # request_launch deliberately refuses a second app while one runs.
            # Finish validating the NETWORK launcher, then leave the device
            # idle for the following Quake smoke test.
            for _ in range(200):
                if any("launcher_ui: LVGL LAN screen created" in line for line in seen):
                    break
                await asyncio.sleep(0.1)
            else:
                raise RuntimeError("NETWORK launcher did not start")
            first = len(seen)
            client.button_command(close.key)
            for _ in range(200):
                if any("Heap after the app ended" in line for line in seen[first:]):
                    break
                await asyncio.sleep(0.1)
            else:
                raise RuntimeError("NETWORK launcher did not release its heap")
            unsubscribe()
            print("PASS: quick firmware, three-button focus navigation, screenshot and A activation", flush=True)
    finally:
        await client.disconnect()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="10.20.30.180")
    parser.add_argument("--output", default="/tmp/quake2-controller-ui")
    asyncio.run(main(parser.parse_args()))
