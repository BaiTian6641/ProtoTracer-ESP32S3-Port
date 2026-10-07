"""Prepare pinned Arduino/PARLIO components for the isolated S31 probe.

Only generated, gitignored component copies are patched. The existing S3
PlatformIO packages and reference checkouts are never modified.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import hashlib
import os

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "experiments" / "esp32s31-parlio"
ARDUINO_REV = "76f683d935b390f2805301ec10a5562bbbb37811"
HUB75_REV = "d0bb571fdae6ec6215615de4b73a6e8ce8868a85"


def snapshot_timestamps(paths):
    return {path: (hashlib.sha256(path.read_bytes()).digest(), path.stat().st_atime_ns, path.stat().st_mtime_ns)
            for path in paths if path.is_file()}


def restore_unchanged_timestamps(snapshot):
    # Regeneration should not trigger a costly CMake/dependency-manager pass
    # or recompile unchanged libraries. Content changes keep their new time.
    for path, (digest, atime, mtime) in snapshot.items():
        if path.is_file() and hashlib.sha256(path.read_bytes()).digest() == digest:
            os.utime(path, ns=(atime, mtime))


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
    timestamps = snapshot_timestamps((components / "hub75").rglob("*"))
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
    shutil.copy2(ROOT / "tools/s31_parlio_frame_sync.h", hub / "src/platforms/parlio/s31_parlio_frame_sync.h")
    shutil.copy2(ROOT / "tools/s31_parlio_frame_sync.inc", hub / "src/platforms/parlio/s31_parlio_frame_sync.inc")
    replace(hub / "src/platforms/parlio/parlio_dma.h", '#include <cstddef>',
            '#include <cstddef>\n#include "s31_parlio_frame_sync.h"', 1)
    replace(hub / "src/platforms/parlio/parlio_dma.h", '  void flip_buffer() override;',
            '  void flip_buffer() override;\n  bool wait_for_frame(uint32_t timeout_ms);\n  bool present_frame(uint32_t timeout_ms);', 1)
    replace(hub / "src/platforms/parlio/parlio_dma.h", '  parlio_tx_unit_handle_t tx_unit_;',
            '  S31ParlioFrameSync frame_sync_;\n  parlio_tx_unit_handle_t tx_unit_;', 1)
    replace(hub / "src/platforms/parlio/parlio_dma.cpp", '  // Allocate row buffers with BCM padding',
            '  if (!frame_sync_.Initialize(tx_unit_)) return false;\n\n  // Allocate row buffers with BCM padding', 1)
    replace(hub / "src/platforms/parlio/parlio_dma.cpp", '  // Initialize buffers with blank data',
            '  if (config_.double_buffer && !is_double_buffered_) return false;\n\n  // Initialize buffers with blank data', 1)
    replace(hub / "src/platforms/parlio/parlio_dma.cpp", 'void ParlioDma::set_brightness_oe() {',
            'void ParlioDma::set_brightness_oe() {\n  if (transfer_started_) return; // Apply only to the owned back buffer at present.', 1)
    cpp = hub / "src/platforms/parlio/parlio_dma.cpp"
    replace(cpp, '// Buffer structure: [pixels (LAT on last pixel)][padding]',
            '// Buffer structure: [pixel shift clocks][LAT and blanked BCM padding]', 1)
    replace(cpp, '// Initialize pixel section (LAT on last pixel)',
            '// Initialize pixel section (LAT stays low throughout shifting)', 1)
    replace(cpp, '    const float time_per_row_us = transmissions * buffer_time_us;',
            '''    // Count the serialized shift and padding words, rather than
    // applying the I2S repetition estimate to the PARLIO stream.
    size_t words_per_row = 0;
    for (int bit = 0; bit < bit_depth_; ++bit)
      words_per_row += dma_width_ + calculate_bcm_padding(bit);
    const float time_per_row_us = words_per_row * 1000000.0f / actual_clock_hz_;''', 1)
    replace(cpp, '''        // LAT pulse on last pixel
        if (x == bp.pixel_words - 1) {
          word |= (1 << LAT_BIT);
        }
''', '', 1)
    replace(cpp, '        bp.data[bp.pixel_words + i] = word;',
            '        if (i == 0) word |= (1 << LAT_BIT); // Latch after the final shift clock.\n        bp.data[bp.pixel_words + i] = word;', 1)
    replace(cpp, '        bp.data[bp.pixel_words + bp.padding_words - 1 - i] |= (1 << OE_BIT);',
            '        bp.data[bp.pixel_words + i] |= (1 << OE_BIT); // Keep LAT and settling blanked.\n        bp.data[bp.pixel_words + bp.padding_words - 1 - i] |= (1 << OE_BIT);', 1)
    text = cpp.read_text(encoding="utf-8")
    begin = text.index("void ParlioDma::flip_buffer() {")
    end = text.index("\n}  // namespace hub75", begin)
    cpp.write_text(text[:begin] + '#include "s31_parlio_frame_sync.inc"\n' + text[end:], encoding="utf-8", newline="\n")
    replace(cpp, '  parlio_tx_unit_disable(tx_unit_);\n  transfer_started_ = false;',
            '  if (parlio_tx_unit_disable(tx_unit_) != ESP_OK) return;\n  transfer_started_ = false;', 1)
    replace(cpp, '  if (tx_unit_) {\n    parlio_tx_unit_disable(tx_unit_);\n    parlio_del_tx_unit(tx_unit_);',
            '  if (transfer_started_) { frame_sync_.Abandon(); return; }\n  if (tx_unit_) {\n    parlio_tx_unit_disable(tx_unit_);\n    if (parlio_del_tx_unit(tx_unit_) != ESP_OK) { frame_sync_.Abandon(); return; }', 1)
    replace(hub / "include/hub75.h", '  void flip_buffer();',
            '  void flip_buffer();\n  bool wait_for_frame(uint32_t timeout_ms);\n  bool present_frame(uint32_t timeout_ms);', 1)
    driver = hub / "src/core/hub75_driver.cpp"
    with driver.open("a", encoding="utf-8", newline="\n") as output:
        output.write('''
bool Hub75Driver::wait_for_frame(uint32_t timeout_ms) {
  return dma_ && static_cast<hub75::ParlioDma*>(dma_)->wait_for_frame(timeout_ms);
}
bool Hub75Driver::present_frame(uint32_t timeout_ms) {
  return dma_ && static_cast<hub75::ParlioDma*>(dma_)->present_frame(timeout_ms);
}
''')
    restore_unchanged_timestamps(timestamps)
    print("Prepared Arduino 4.0.0-RC1 and S31 PARLIO component copies")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arduino", type=Path, default=ROOT / "reference/arduino-s31-review")
    parser.add_argument("--hub75", type=Path, default=ROOT / "reference/esp-hub75-review")
    args = parser.parse_args()
    prepare(args.arduino.resolve(), args.hub75.resolve())
