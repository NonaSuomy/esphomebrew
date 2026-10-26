"""Apply the upstream TinyCrypt ECC pairing fix to the pinned ESP-IDF 6.1 SDK."""
from pathlib import Path
import os
import subprocess


def patch_idf_ecc(root, patch=None):
    root = Path(root).resolve()
    source = root / "components/bt/common/tinycrypt/src/ecc.c"
    if not source.is_file():
        return False
    patch = Path(patch or Path(__file__).resolve().parents[1] / "patches/0008-idf61-tinycrypt-ecc-pairing.patch").resolve()
    # Prevent a source archive inside a workspace from inheriting the parent
    # repository's path rules. git apply also supports ordinary source archives.
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(root.parent))
    def check(reverse=False):
        command = ["git", "apply", "--check"]
        if reverse:
            command.append("--reverse")
        return subprocess.run(command + [str(patch)], cwd=root, env=env, capture_output=True, text=True)
    if check(True).returncode == 0:
        return False
    # Newer SDKs may already carry the equivalent upstream fix.
    text = source.read_text()
    port = (root / "components/bt/common/tinycrypt/port/esp_tinycrypt_port.c").read_text()
    dh = (source.parent / "ecc_dh.c").read_text()
    if "num_bits == curve->num_n_bits" in text and "uecc_vli_native_to_le" in port and "Use the software ladder for ECDH" in dh:
        return False
    result = check()
    if result.returncode:
        raise RuntimeError("ESP-IDF TinyCrypt source differs from the supported ECC backport: " + result.stderr)
    subprocess.run(["git", "apply", str(patch)], cwd=root, env=env, check=True)
    return True
