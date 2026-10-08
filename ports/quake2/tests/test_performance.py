"""Host regression for preserving incomplete device-test measurements."""
import json
from pathlib import Path
import tempfile
from device_smoke import write_performance

with tempfile.TemporaryDirectory(prefix="quake2-perf-") as directory:
    output = Path(directory)
    samples = [{"frames": 40, "elapsed_us": 5000000, "fps": 8.0, "present_ms": 39.0},
               {"frames": 20, "elapsed_us": 5000000, "fps": 4.0, "present_ms": 40.0}]
    checks = {"capture_completed": False, "clean_exit_observed": False}
    write_performance(output, {"layout": "sram-depth"}, samples, 1, checks)
    data = json.loads((output / "performance.json").read_text())
    assert data["fps"] == 8.0 and len(data["samples"]) == 1
    assert data["checks"] == checks
    write_performance(output, None, [], 0, checks)
    assert json.loads((output / "performance.json").read_text())["fps"] is None
print("PASS: fixed measurement windows and honest partial results after capture failure")
