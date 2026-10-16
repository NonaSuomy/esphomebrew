"""The SPI compatibility fix must survive a managed-component refresh."""
from pathlib import Path
import runpy
import tempfile

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "idf61_compat.py"
RELATIVE_SOURCE = "managed_components/espressif__esp_hosted/host/port/esp/freertos/src/port_esp_hosted_host_spi.c"
ORIGINAL = "void transfer() {\n    t.flags |= SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL;\n}\n"


class BuildEnvironment:
    def __init__(self, project):
        self.project = project
        self.actions = []
        self.dependencies = []

    def subst(self, value):
        return value.replace("$PROJECT_DIR", str(self.project))

    def File(self, path):
        return path

    def AddPreAction(self, target, callback):
        self.actions.append((target, callback))

    def GetExtraScripts(self, phase):
        assert phase == "pre"
        return [str(SCRIPT)]

    def Depends(self, target, source):
        self.dependencies.append((target, source))


class SourceNode:
    def __init__(self, path):
        self.path = path

    def get_abspath(self):
        return str(self.path)


with tempfile.TemporaryDirectory() as folder:
    project = Path(folder)
    source = project / RELATIVE_SOURCE
    source.parent.mkdir(parents=True)
    source.write_text(ORIGINAL)
    env = BuildEnvironment(project)
    runpy.run_path(str(SCRIPT), init_globals={"env": env, "Import": lambda name: None})
    assert "t.flags |= SPI_TRANS_DMA_USE_PSRAM;" in source.read_text()
    assert len(env.actions) == 1
    target, callback = env.actions[0]
    assert target == "$BUILD_DIR/" + RELATIVE_SOURCE + ".o"
    assert env.dependencies == [(target, str(SCRIPT.resolve()))]
    # CMake refreshes the component after the pre-script has already run.
    source.write_text(ORIGINAL)
    callback([target], [SourceNode(source)], env)
    assert "t.flags |= SPI_TRANS_DMA_USE_PSRAM;" in source.read_text()
    unchanged = source.stat().st_mtime_ns
    callback([target], [SourceNode(source)], env)
    assert source.stat().st_mtime_ns == unchanged
print("PASS hosted SPI build hook: refreshed source repaired before compile; idempotent and script-dependent")
