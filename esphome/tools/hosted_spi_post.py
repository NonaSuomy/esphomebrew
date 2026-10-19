"""Apply hosted DMA compatibility after the framework's CMake refresh."""
from pathlib import Path
from runpy import run_path
Import("env")
compat_directory = next(Path(path).parent for path in env.GetExtraScripts("post") if Path(path).name == "hosted_spi_post.py")
patch = run_path(str(compat_directory / "hosted_spi_dma.py"))["patch_hosted_spi_source"]
project = Path(env.subst("$PROJECT_DIR"))
for root in (project / "managed_components", project / ".esphome/pio_components/idf"):
    patch(root / "espressif__esp_hosted/host/port/esp/freertos/src/port_esp_hosted_host_spi.c")
