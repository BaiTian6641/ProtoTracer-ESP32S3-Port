"""Prepare the full S31 firmware's Arduino-library IDF component.

Libraries are read from the existing resolved S3 PlatformIO dependencies; no
paid or private source is copied into tracked files. Generated CMake is local.
"""
from pathlib import Path
import json
import shutil
from prepare_s31_probe import ROOT, PROJECT, prepare, snapshot_timestamps, restore_unchanged_timestamps

LIBRARIES = ["Adafruit GFX Library", "Adafruit BusIO", "Adafruit NeoPixel",
             "Adafruit Unified Sensor", "Adafruit BNO055", "NTPClient",
             "arduinoFFT", "ArduinoJson", "AsyncTCP", "ESPAsyncWebServer",
             "M5GFX", "NetWizard", "ProtoGC", "QRCode", "RevEng PAJ7620"]


def prepare_firmware():
    prepare(ROOT / "reference/arduino-s31-review", ROOT / "reference/esp-hub75-review")
    deps = ROOT / ".pio/libdeps/esp32s3"
    component = PROJECT / "components/prototracer_deps"
    component.mkdir(parents=True, exist_ok=True)
    timestamps = snapshot_timestamps([component / "CMakeLists.txt", *(component / "vendor").rglob("*")])
    include_dirs, sources = [], []
    for name in LIBRARIES:
        library = deps / name
        if not library.is_dir():
            raise RuntimeError(f"Missing resolved dependency {library}; install esp32s3 packages first")
        if name in ["M5GFX", "M5Unified"]:
            cached = component / "vendor" / name
            shutil.copytree(library / "src", cached / "src", dirs_exist_ok=True)
            for license_file in library.glob("[Ll][Ii][Cc][Ee][Nn][Ss][Ee]*"):
                if license_file.is_file(): shutil.copy2(license_file, cached / license_file.name)
            library = cached
        if name == "M5GFX":
            device = library / "src/lgfx/v1/platforms/device.hpp"
            text = device.read_text(encoding="utf-8")
            text = text.replace('#if defined (CONFIG_IDF_TARGET_ESP32C6)', '#if defined (CONFIG_IDF_TARGET_ESP32C6) || defined (CONFIG_IDF_TARGET_ESP32S31)', 1)
            device.write_text(text, encoding="utf-8")
            common = library / "src/lgfx/v1/platforms/esp32/common.hpp"
            text = common.read_text(encoding="utf-8")
            text = text.replace('#if defined ( CONFIG_IDF_TARGET_ESP32P4 )', '#if defined ( CONFIG_IDF_TARGET_ESP32P4 ) || defined ( CONFIG_IDF_TARGET_ESP32S31 )', 1)
            common.write_text(text, encoding="utf-8")
            common_cpp = library / "src/lgfx/v1/platforms/esp32/common.cpp"
            text = common_cpp.read_text(encoding="utf-8")
            text = text.replace('defined ( CONFIG_IDF_TARGET_ESP32P4 )', 'defined ( CONFIG_IDF_TARGET_ESP32P4 ) || defined ( CONFIG_IDF_TARGET_ESP32S31 )', 1)
            pin_begin = text.index('  void pinMode(int_fast16_t pin, pin_mode_t mode)')
            pin_end = text.index('\n//----------------------------------------------------------------------------\n', pin_begin)
            text = text[:pin_begin] + '''  void pinMode(int_fast16_t pin, pin_mode_t mode) {
    if (!GPIO_IS_VALID_GPIO(pin)) return;
    if (mode == pin_mode_t::output && !GPIO_IS_VALID_OUTPUT_GPIO(pin)) return;
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << pin;
    config.mode = mode == pin_mode_t::output ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
    config.pull_up_en = mode == pin_mode_t::input_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    config.pull_down_en = mode == pin_mode_t::input_pulldown ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&config);
  }
''' + text[pin_end:]
            begin = text.index('  namespace i2c\n  {')
            wrappers = text.index('    cpp::result<void, error_t> transactionWrite(', begin)
            temporary = text.index('    enum pin_index_t {', wrappers)
            end = text.index('\n  }\n\n//----------------------------------------------------------------------------', temporary)
            text = text[:begin] + '  namespace i2c\n  {\n#include "s31_m5_i2c.inc"\n' + text[wrappers:temporary] + '''
    i2c_temporary_switcher_t::i2c_temporary_switcher_t(int port, int sda, int scl) : _i2c_port(port) {
      if (!s31Wire(port)) return;
      auto& state = s31WireState[port];
      _pin_backup[0].setPin(state.sda); _pin_backup[1].setPin(state.scl);
      _need_reinit = state.initialized;
      _backuped = state.sda != sda || state.scl != scl;
      if (_backuped) init(port, sda, scl);
    }
    void i2c_temporary_switcher_t::restore() {
      if (!_backuped) return;
      release(_i2c_port);
      const int sda = _pin_backup[0].getPin(), scl = _pin_backup[1].getPin();
      if (_need_reinit) init(_i2c_port, sda, scl);
      else if (sda >= 0 && scl >= 0) setPins(_i2c_port, sda, scl);
      _backuped = false;
    }
''' + text[end:]
            common_cpp.write_text(text, encoding="utf-8")
            shutil.copy2(ROOT / "tools/s31_m5_i2c.inc", common_cpp.parent / "s31_m5_i2c.inc")
            spi_header = library / "src/lgfx/v1/platforms/esp32/Bus_SPI.hpp"
            text = spi_header.read_text(encoding="utf-8")
            text = text.replace('#if __has_include(<rom/lldesc.h>)', '#if defined(CONFIG_IDF_TARGET_ESP32S31)\n #include <esp_rom_lldesc.h>\n#elif __has_include(<rom/lldesc.h>)', 1)
            spi_header.write_text(text, encoding="utf-8")
        source = library / "src" if (library / "src").is_dir() else library
        include_dirs.append(source.as_posix())
        for path in source.rglob("*"):
            if path.suffix.lower() not in [".cpp", ".c", ".s"]: continue
            if any(part.lower() in ["examples", "test", "tests", ".git"] for part in path.parts): continue
            if path.name == "glcdfont.c": continue  # Included by Adafruit_GFX.cpp.
            if name == "M5GFX" and path.name in ["Bus_Parallel8.cpp", "Bus_Parallel16.cpp", "Bus_DSI.cpp", "Panel_CVBS.cpp"]: continue
            sources.append(path.as_posix())
    # Preserve the existing licensed-library selection; files remain external.
    ota = ROOT / "lib/ElegantOTAPro-3-1-4/src"
    if not ota.is_dir(): ota = deps / "ElegantOTA/src"
    if not ota.is_dir(): raise RuntimeError("Install the configured ElegantOTA dependency first")
    include_dirs.append(ota.as_posix())
    sources.extend(p.as_posix() for p in ota.glob("*.cpp"))
    cmake = 'idf_component_register(\n SRCS\n' + ''.join(f'  "{p}"\n' for p in sources)
    cmake += ' INCLUDE_DIRS\n' + ''.join(f'  "{p}"\n' for p in include_dirs)
    cmake += ' REQUIRES arduino nvs_flash efuse esp_mm esp_adc esp_lcd esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_ledc esp_driver_rmt esp_timer esp_wifi esp_http_client\n)\n'
    cmake += 'target_compile_definitions(${COMPONENT_LIB} PUBLIC ELEGANTOTA_USE_ASYNC_WEBSERVER=1 NETWIZARD_USE_ASYNC_WEBSERVER=1)\n'
    cmake += 'target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers -Wno-error=overloaded-virtual)\n'
    fft_source = (deps / "arduinoFFT/src/arduinoFFT.cpp").as_posix()
    cmake += f'set_source_files_properties("{fft_source}" PROPERTIES COMPILE_OPTIONS "-Wno-error=cpp")\n'
    qr_source = (deps / "QRCode/src/qrcode.c").as_posix()
    # This legacy C library defines bool itself; it predates native C23 bool.
    cmake += f'set_source_files_properties("{qr_source}" PROPERTIES COMPILE_OPTIONS "-std=gnu17;-Wno-error=unknown-pragmas;-Wno-error=type-limits")\n'
    (component / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
    assets = PROJECT / "test-data"
    assets.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "src/Morph/universal_face.json", assets / "universal_face.json")
    shutil.copy2(ROOT / "src/Animation/example_animation.json", assets / "example_animation.json")
    # Minimal public test settings; firmware still derives device_id from eFuse.
    (assets / "user_config.json").write_text(json.dumps({"username":"ProtoTracer-S31", "user_animation":"example_animation.json", "user_brightness":50}), encoding="utf-8")
    restore_unchanged_timestamps(timestamps)
    print(f"Prepared full firmware dependencies ({len(sources)} sources) and public test assets")


if __name__ == "__main__":
    prepare_firmware()
