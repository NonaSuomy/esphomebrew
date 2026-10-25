"""Small ESPHome/ESP-IDF 6.1 generated-source compatibility fixes."""

from pathlib import Path

Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
generated_main = project_dir / "src" / "main.cpp"
if generated_main.exists():
    source = generated_main.read_text()
    fixed = source.replace("using std::isnan;\n", "")
    if fixed != source:
        generated_main.write_text(fixed)
        print("IDF 6.1 compatibility: removed conflicting generated using std::isnan")


# ESP-IDF 6.1 ships Mbed TLS 4, which removed the legacy mbedtls_sha256_* API
# still used by ESPHome's SHA256 helper (including OTA authentication). Adapt
# the generated ESPHome helper to the PSA hash API supplied by Mbed TLS 4.
sha_header = project_dir / "src" / "esphome" / "components" / "sha256" / "sha256.h"
sha_source = project_dir / "src" / "esphome" / "components" / "sha256" / "sha256.cpp"
if sha_header.exists() and sha_source.exists():
    header = sha_header.read_text()
    fixed_header = header.replace('#include "mbedtls/sha256.h"', "#include <psa/crypto.h>")
    fixed_header = fixed_header.replace(
        "mbedtls_sha256_context ctx_{};",
        "psa_hash_operation_t ctx_{};\n  bool ctx_valid_{false};",
    )
    if fixed_header != header:
        sha_header.write_text(fixed_header)

    source = sha_source.read_text()
    old_sha_impl = '''SHA256::~SHA256() { mbedtls_sha256_free(&this->ctx_); }

void SHA256::init() {
  mbedtls_sha256_init(&this->ctx_);
  mbedtls_sha256_starts(&this->ctx_, 0);  // 0 = SHA256, not SHA224
}

void SHA256::add(const uint8_t *data, size_t len) { mbedtls_sha256_update(&this->ctx_, data, len); }

void SHA256::calculate() { mbedtls_sha256_finish(&this->ctx_, this->digest_); }'''
    new_sha_impl = '''SHA256::~SHA256() {
  if (this->ctx_valid_) {
    psa_hash_abort(&this->ctx_);
  }
}

void SHA256::init() {
  if (this->ctx_valid_) {
    psa_hash_abort(&this->ctx_);
  }
  this->ctx_ = psa_hash_operation_init();
  this->ctx_valid_ = psa_crypto_init() == PSA_SUCCESS &&
                     psa_hash_setup(&this->ctx_, PSA_ALG_SHA_256) == PSA_SUCCESS;
}

void SHA256::add(const uint8_t *data, size_t len) {
  if (this->ctx_valid_ && psa_hash_update(&this->ctx_, data, len) != PSA_SUCCESS) {
    psa_hash_abort(&this->ctx_);
    this->ctx_valid_ = false;
  }
}

void SHA256::calculate() {
  size_t digest_len = 0;
  if (!this->ctx_valid_ ||
      psa_hash_finish(&this->ctx_, this->digest_, sizeof(this->digest_), &digest_len) != PSA_SUCCESS ||
      digest_len != sizeof(this->digest_)) {
    std::memset(this->digest_, 0, sizeof(this->digest_));
  }
  this->ctx_valid_ = false;
}'''
    fixed_source = source.replace(old_sha_impl, new_sha_impl)
    if fixed_source != source:
        sha_source.write_text(fixed_source)
        print("IDF 6.1 compatibility: switched ESPHome SHA256 helper to PSA")


# LVGL 9.5's ESP-IDF component is laid out as ``lvgl/lvgl``.  Its source
# files include the public header as ``lvgl/lvgl.h``, which requires the
# package parent (the first ``lvgl`` directory) on the include path.  The
# component's generated registration currently exposes only ``.`` and
# ``src``; that works for ``lvgl.h`` but not for the qualified include used
# by the font sources.  Patch the managed copy after ESPHome has fetched it.
# Keep this idempotent because PlatformIO may run extra scripts more than
# once during a build.
managed_roots = (
    project_dir / ".esphome" / "pio_components" / "idf",
    project_dir / "managed_components",
)
for managed_root in managed_roots:
    if not managed_root.exists():
        continue
    for cmake_file in managed_root.rglob("CMakeLists.txt"):
        if cmake_file.parent.name != "lvgl" or cmake_file.parent.parent.name != "lvgl":
            continue
        source = cmake_file.read_text()
        if "src/font/lv_font_montserrat_20.c" not in source:
            continue
        old = 'INCLUDE_DIRS "." "src"'
        new = 'INCLUDE_DIRS ".." "." "src"'
        if old in source and new not in source:
            cmake_file.write_text(source.replace(old, new, 1))
            print(f"IDF 6.1 compatibility: fixed LVGL include path in {cmake_file}")

# IDF 6.1 requires an explicit transaction flag for external DMA buffers.
# Patch again immediately before this translation unit compiles: ESP-IDF's
# CMake/component refresh can replace managed sources after this pre-script.
from runpy import run_path
compat_directory = next(Path(path).parent for path in env.GetExtraScripts("pre") if Path(path).name == "idf61_compat.py")
patch_hosted_spi_source = run_path(str(compat_directory / "hosted_spi_dma.py"))["patch_hosted_spi_source"]

for managed_root in managed_roots:
    patch_hosted_spi_source(managed_root / "espressif__esp_hosted" / "host" / "port" / "esp" /
                            "freertos" / "src" / "port_esp_hosted_host_spi.c")


# Middleware runs while PlatformIO constructs its source build graph, after
# CMake's managed-component refresh, including when an existing object is reused.
def patch_hosted_spi_middleware(env, node):
    patch_hosted_spi_source(Path(node.srcnode().get_abspath()))
    return node


env.AddBuildMiddleware(patch_hosted_spi_middleware, "*port_esp_hosted_host_spi.c")


def patch_hosted_spi_before_compile(target, source, env):
    patch_hosted_spi_source(Path(source[0].srcnode().get_abspath()))


hosted_spi_object = env.File(env.subst("$BUILD_DIR/managed_components/espressif__esp_hosted/host/port/esp/"
                             "freertos/src/port_esp_hosted_host_spi.c.o"))
env.AddPreAction(hosted_spi_object, patch_hosted_spi_before_compile)
# Rebuild this object when the compatibility script itself changes, too.
for compat_script in env.GetExtraScripts("pre"):
    if Path(compat_script).name == "idf61_compat.py":
        env.Depends(hosted_spi_object, str(Path(compat_script).resolve()))

# Hosted SPI installs the shared GPIO ISR service before touchscreen setup.
# ESPHome must attach to that existing service rather than dropping its ISR.
gpio_source = project_dir / "src" / "esphome" / "components" / "esp32" / "gpio.cpp"
if gpio_source.exists():
    source = gpio_source.read_text()
    old = "auto res = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3);\n    if (res != ESP_OK) {"
    new = "auto res = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3);\n    if (res != ESP_OK && res != ESP_ERR_INVALID_STATE) {"
    if old in source:
        gpio_source.write_text(source.replace(old, new, 1))
        print("IDF compatibility: reuse existing hosted GPIO ISR service")

# Respect per-pin pull-up settings during I2C recovery as well as operation.
i2c_source = project_dir / "src/esphome/components/i2c/i2c_bus_esp_idf.cpp"
if i2c_source.exists():
    text = i2c_source.read_text()
    for field, setting in (("scl_config", "scl_pullup_enabled_"), ("sda_conf", "sda_pullup_enabled_")):
        text = text.replace(f"{field}.pull_up_en = GPIO_PULLUP_ENABLE;",
                            f"{field}.pull_up_en = {setting} ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;")
    i2c_source.write_text(text)

# A missing AP-info payload must return an error instead of asserting in memcpy.
patch_hosted_rpc_source = run_path(str(compat_directory / "hosted_rpc_compat.py"))["patch_hosted_rpc_source"]
for managed_root in managed_roots:
    patch_hosted_rpc_source(managed_root / "espressif__esp_hosted/host/drivers/rpc/wrap/rpc_wrap.c")

def patch_hosted_rpc_middleware(env, node):
    patch_hosted_rpc_source(Path(node.srcnode().get_abspath()))
    return node

env.AddBuildMiddleware(patch_hosted_rpc_middleware, "*rpc_wrap.c")

def patch_hosted_rpc_before_compile(target, source, env):
    patch_hosted_rpc_source(Path(source[0].srcnode().get_abspath()))

hosted_rpc_object = env.File(env.subst("$BUILD_DIR/managed_components/espressif__esp_hosted/host/drivers/rpc/wrap/rpc_wrap.c.o"))
env.AddPreAction(hosted_rpc_object, patch_hosted_rpc_before_compile)
env.Depends(hosted_rpc_object, str(compat_directory / "hosted_rpc_compat.py"))
