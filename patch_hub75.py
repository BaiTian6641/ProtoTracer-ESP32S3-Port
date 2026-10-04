"""Reproducible, hash-guarded patch of the pinned HUB75 chained packer.

The original method is retained as fallback and an optional on-device oracle.
Only generated .pio/libdeps sources are patched; no upstream checkout changes.
"""
import hashlib
import os
from pathlib import Path
from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()

source = (Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"] /
          "ESP32 HUB75 LED MATRIX PANEL DMA Display/src/ESP32-HUB75-MatrixPanel-I2S-DMA.cpp")
if not source.exists():
    raise RuntimeError("Pinned HUB75 source unavailable; install project dependencies first")
include = source.parent / "hub75_chained.inc"
payload = (Path(env["PROJECT_DIR"]) / "tools/hub75_chained.inc").read_bytes()
if not include.exists() or include.read_bytes() != payload:
    include.write_bytes(payload)
config = source.parent / "hub75_pack_config.h"
config_text = "#undef HUB75_VERIFY_PACK\n#define HUB75_VERIFY_PACK " + (
    "1\n" if os.environ.get("PROTOTRACER_HUB75_VERIFY") == "1" else "0\n")
if not config.exists() or config.read_text(encoding="utf-8") != config_text:
    config.write_text(config_text, encoding="utf-8")
text = source.read_text(encoding="utf-8")
marker = "// PROTOTRACER_CHAINED_WRAPPER_V1"
begin = text.index("void MatrixPanel_I2S_DMA::fillBufferRgb888Chained(")
end = text.index("\nvoid MatrixPanel_I2S_DMA::flushDMAFramebuffer", begin)
original = text[begin:end]
if marker not in original:
    expected = "f6dd70163dd6364d0da0a3205cc893e23ee7d98b67add9e4d197210a6353f256"
    if hashlib.sha256(original.encode()).hexdigest() != expected:
        raise RuntimeError("HUB75 packer differs from audited revision; refusing patch")
    body = original[original.index("{") + 1:original.rindex("}")]
else:
    body_start = original.index("auto legacy = [&]() { ") + len("auto legacy = [&]() { ")
    body_end = original.index(" };\n#if defined(TASESP32S3)", body_start)
    body = original[body_start:body_end]
if hashlib.sha256(body.encode()).hexdigest() != "8f1ea00458bdd203e7bbaf350ec360febb377d7f0b55fd37e44f86f57075b0be":
    raise RuntimeError("HUB75 legacy oracle changed; refusing patch")
config_include = '#include "hub75_pack_config.h"\n'
if text[:begin].endswith(config_include + "#include <array>\n"):
    begin -= len(config_include + "#include <array>\n")
elif text[:begin].endswith(config_include):
    begin -= len(config_include)
wrapper = config_include + '''#include <array>
void MatrixPanel_I2S_DMA::fillBufferRgb888Chained(const uint8_t* rgb888)
{
// PROTOTRACER_CHAINED_WRAPPER_V1
    auto legacy = [&]() { ORIGINAL_BODY };
#if defined(TASESP32S3)
    if (PIXELS_PER_ROW == 128 && ROWS_PER_FRAME == 16 && initialized && rgb888 &&
        m_cfg.getPixelColorDepthBits() <= 12) {
#if HUB75_VERIFY_PACK
        static unsigned checked = 0;
        uint16_t* reference = nullptr;
        const unsigned depth = m_cfg.getPixelColorDepthBits();
        const size_t bytes = 16 * depth * 128 * sizeof(uint16_t);
        if (checked < 64) {
            reference = static_cast<uint16_t*>(heap_caps_malloc(bytes * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!reference) Serial.println("[HUBVERIFY] allocation failed");
            else {
                for (unsigned row = 0; row < 16; ++row)
                    memcpy(reference + bytes / sizeof(uint16_t) + row * depth * 128,
                           getRowDataPtr(row, 0), depth * 128 * sizeof(uint16_t));
                legacy();
                for (unsigned row = 0; row < 16; ++row) {
                    memcpy(reference + row * depth * 128, getRowDataPtr(row, 0), depth * 128 * sizeof(uint16_t));
                    memcpy(getRowDataPtr(row, 0), reference + bytes / sizeof(uint16_t) + row * depth * 128,
                           depth * 128 * sizeof(uint16_t));
                }
            }
        }
#endif
#include "hub75_chained.inc"
#if HUB75_VERIFY_PACK
        if (reference) {
            unsigned diffs = 0;
            for (unsigned row = 0; row < 16; ++row) {
                const uint16_t* actual = getRowDataPtr(row, 0);
                for (unsigned i = 0; i < depth * 128; ++i)
                    diffs += actual[i] != reference[row * depth * 128 + i];
                if (diffs) memcpy(getRowDataPtr(row, 0), reference + row * depth * 128, depth * 128 * sizeof(uint16_t));
            }
            if (diffs) flushDMAFramebuffer(true);
            heap_caps_free(reference);
            ++checked;
            Serial.printf("[HUBVERIFY] checked=%u diffs=%u\\n", checked, diffs);
        }
#endif
        return;
    }
#endif
    legacy();
}
'''.replace("ORIGINAL_BODY", body)
patched = text[:begin] + wrapper + text[end:]
if patched != text:
    with source.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(patched)
    print("Applied audited HUB75 chained pack optimization")

header = source.with_suffix(".h")
header_text = header.read_text(encoding="utf-8")
debug_marker = "// PROTOTRACER_DMA_DEBUG_VIEW_V1"
if debug_marker not in header_text:
    anchor = "  void fillBufferRgb888Chained(const uint8_t* rgb888);"
    if header_text.count(anchor) != 1:
        raise RuntimeError("Pinned HUB75 public method anchor changed; refusing debug accessor patch")
    accessor = '''
  // PROTOTRACER_DMA_DEBUG_VIEW_V1
  // Frame-boundary inspection only. No DMA descriptors/control registers exposed.
  const void* debugRowView(bool front, unsigned row, size_t& bytes, unsigned& width, unsigned& depth)
  {
    bytes = 0; width = 0; depth = 0;
    if (!initialized) return nullptr;
    const unsigned id = m_cfg.double_buff ? (front ? (back_buffer_id ^ 1) : back_buffer_id) : 0;
    frameStruct& frame = frame_buffer[id];
    if (!frame.rowBits || row >= frame.rows) return nullptr;
    rowBitStruct& bits = frame.rowBits[row];
    bytes = bits.getColorDepthSize(false);
    width = bits.width;
    depth = bits.colour_depth;
    return bits.data;
  }
'''
    with header.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(header_text.replace(anchor, anchor + accessor))
