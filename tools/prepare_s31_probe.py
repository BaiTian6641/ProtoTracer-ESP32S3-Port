"""Prepare pinned Arduino/PARLIO components for the isolated S31 probe.

Only generated, gitignored component copies are patched. The existing S3
PlatformIO packages and reference checkouts are never modified.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "experiments" / "esp32s31-parlio"
ARDUINO_REV = "76f683d935b390f2805301ec10a5562bbbb37811"
HUB75_REV = "d0bb571fdae6ec6215615de4b73a6e8ce8868a85"


def check_revision(path, revision):
    head = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(
        ["git", "-C", str(path), "status", "--porcelain", "--untracked-files=no"], text=True
    ).strip()
    if head != revision or dirty:
        raise RuntimeError(f"Expected clean {revision} at {path}; got {head}, dirty={bool(dirty)}")


def replace(path, before, after, count):
    text = path.read_text(encoding="utf-8")
    if text.count(before) != count:
        raise RuntimeError(f"Audited source anchor changed in {path}: {before!r}")
    path.write_text(text.replace(before, after), encoding="utf-8", newline="\n")


def prepare(arduino, hub75):
    check_revision(arduino, ARDUINO_REV)
    check_revision(hub75, HUB75_REV)
    components = PROJECT / "components"
    for name, source in [("arduino", arduino), ("hub75", hub75 / "components" / "hub75")]:
        dest = components / name
        dest.mkdir(parents=True, exist_ok=True)
        if name == "arduino":
            for directory in ["cores", "libraries", "variants"]:
                shutil.copytree(source / directory, dest / directory, dirs_exist_ok=True)
            for filename in ["CMakeLists.txt", "Kconfig.projbuild", "idf_component.yml", "LICENSE.md"]:
                shutil.copy2(source / filename, dest / filename)
        else:
            shutil.copytree(source, dest, dirs_exist_ok=True)

    hub = components / "hub75"
    replace(hub / "CMakeLists.txt",
            'IDF_TARGET STREQUAL "esp32p4" OR IDF_TARGET STREQUAL "esp32c6"',
            'IDF_TARGET STREQUAL "esp32p4" OR IDF_TARGET STREQUAL "esp32c6" OR IDF_TARGET STREQUAL "esp32s31"', 1)
    replace(hub / "CMakeLists.txt", "CONFIG_IDF_TARGET_ESP32P4 OR CONFIG_IDF_TARGET_ESP32C6",
            "CONFIG_IDF_TARGET_ESP32P4 OR CONFIG_IDF_TARGET_ESP32C6 OR CONFIG_IDF_TARGET_ESP32S31", 1)
    replace(hub / "idf_component.yml", "  - esp32p4\n", "  - esp32p4\n  - esp32s31\n", 1)
    replace(hub / "src/platforms/platform_detect.h", '#else\n#error "Unsupported ESP32 variant',
            '#elif defined(CONFIG_IDF_TARGET_ESP32S31)\n#define HUB75_PLATFORM_ESP32S31\n#define HUB75_DMA_ENGINE_PARLIO\n\n#else\n#error "Unsupported ESP32 variant', 1)
    replace(hub / "src/platforms/platform_detect.h", '  return "ESP32-P4";\n#else',
            '  return "ESP32-P4";\n#elif defined(HUB75_PLATFORM_ESP32S31)\n  return "ESP32-S31";\n#else', 1)
    replace(hub / "src/core/hub75_driver.cpp",
            "defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32C6)",
            "defined(CONFIG_IDF_TARGET_ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32S31)", 2)
    replace(hub / "include/hub75_config.h",
            "!defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(CONFIG_IDF_TARGET_ESP32P4)",
            "!defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(CONFIG_IDF_TARGET_ESP32P4) && !defined(CONFIG_IDF_TARGET_ESP32S31)", 1)
    replace(hub / "Kconfig", "depends on IDF_TARGET_ESP32S3 || IDF_TARGET_ESP32P4",
            "depends on IDF_TARGET_ESP32S3 || IDF_TARGET_ESP32P4 || IDF_TARGET_ESP32S31", 1)
    replace(hub / "src/platforms/parlio/parlio_dma.cpp",
            ".sample_edge = config_.clk_phase_inverted ? PARLIO_SAMPLE_EDGE_NEG : PARLIO_SAMPLE_EDGE_POS,",
            ".shift_edge = config_.clk_phase_inverted ? PARLIO_SHIFT_EDGE_NEG : PARLIO_SHIFT_EDGE_POS,", 1)
    print("Prepared Arduino 4.0.0-RC1 and S31 PARLIO component copies")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arduino", type=Path, default=ROOT / "reference/arduino-s31-review")
    parser.add_argument("--hub75", type=Path, default=ROOT / "reference/esp-hub75-review")
    args = parser.parse_args()
    prepare(args.arduino.resolve(), args.hub75.resolve())
