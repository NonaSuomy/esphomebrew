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
        self.middleware = []

    def subst(self, value):
        return value.replace("$PROJECT_DIR", str(self.project)).replace("$BUILD_DIR", str(self.project / ".pioenvs/test"))

    def File(self, path):
        return path

    def AddPreAction(self, target, callback):
        self.actions.append((target, callback))

    def AddBuildMiddleware(self, callback, pattern):
        self.middleware.append((callback, pattern))

    def GetExtraScripts(self, phase):
        assert phase in ("pre", "post")
        return [str(SCRIPT if phase == "pre" else SCRIPT.with_name("hosted_spi_post.py"))]

    def Depends(self, target, source):
        self.dependencies.append((target, source))


class SourceNode:
    def __init__(self, path, source_path=None):
        self.path = path
        self.source_path = source_path

    def srcnode(self):
        return SourceNode(self.source_path) if self.source_path else self

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
    assert len(env.actions) == 2
    target, callback = env.actions[0]
    assert target == str(project/".pioenvs/test"/RELATIVE_SOURCE)+".o"
    assert (target, str(SCRIPT.resolve())) in env.dependencies
    # CMake refreshes the component after the pre-script has already run.
    source.write_text(ORIGINAL)
    callback([target], [SourceNode(source)], env)
    assert "t.flags |= SPI_TRANS_DMA_USE_PSRAM;" in source.read_text()
    assert "t.flags &= ~SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL;" in source.read_text()
    assert len(env.middleware)==2
    middleware,pattern=env.middleware[0]
    assert pattern=="*port_esp_hosted_host_spi.c"
    source.write_text(ORIGINAL)
    node=SourceNode(project/".pioenvs/variant/port_esp_hosted_host_spi.c",source)
    assert middleware(env,node) is node
    assert "t.flags &= ~SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL;" in source.read_text()
    assert "t.flags |= SPI_TRANS_DMA_USE_PSRAM;" in source.read_text()
    # The framework may bypass project-source middleware; the post script
    # must repair freshly downloaded sources before dependency signatures.
    source.write_text(ORIGINAL)
    runpy.run_path(str(SCRIPT.with_name("hosted_spi_post.py")), init_globals={"env":env,"Import":lambda name:None})
    assert "t.flags &= ~SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL;" in source.read_text()
    assert "t.flags |= SPI_TRANS_DMA_USE_PSRAM;" in source.read_text()
    unchanged = source.stat().st_mtime_ns
    callback([target], [SourceNode(source)], env)
    assert source.stat().st_mtime_ns == unchanged
    rpc_source=project/"managed_components/espressif__esp_hosted/host/drivers/rpc/wrap/rpc_wrap.c"
    rpc_source.parent.mkdir(parents=True,exist_ok=True)
    original_rpc="int rpc_wifi_sta_get_ap_info(void *ap_info) {\n if (resp && resp->resp_event_status == SUCCESS) { copy(); }\n}\nint next() {}\n"
    rpc_target,rpc_callback=env.actions[1]
    assert rpc_target==str(project/".pioenvs/test/managed_components/espressif__esp_hosted/host/drivers/rpc/wrap/rpc_wrap.c.o")
    assert (rpc_target,str(SCRIPT.with_name("hosted_rpc_compat.py"))) in env.dependencies
    rpc_source.write_text(original_rpc)
    rpc_callback([rpc_target],[SourceNode(rpc_source)],env)
    assert "!resp->u.wifi_scan_ap_list.out_list" in rpc_source.read_text()
    rpc_source.write_text(original_rpc)
    rpc_middleware,pattern=env.middleware[1]
    assert pattern=="*rpc_wrap.c"
    rpc_middleware(env,SourceNode(rpc_source))
    assert "!resp->u.wifi_scan_ap_list.out_list" in rpc_source.read_text()
    rpc_source.write_text(original_rpc)
    runpy.run_path(str(SCRIPT.with_name("hosted_spi_post.py")),init_globals={"env":env,"Import":lambda name:None})
    assert "!resp->u.wifi_scan_ap_list.out_list" in rpc_source.read_text()
print("PASS hosted SPI/RPC build hooks: refreshed source repaired before compile; idempotent and script-dependent")
