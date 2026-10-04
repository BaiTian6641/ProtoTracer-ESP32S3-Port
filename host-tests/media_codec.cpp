#include <cstdio>
#include <vector>
#include <algorithm>
#include "../src/Media/MediaCodec.h"
#include "../src/Media/BuiltinMedia.h"
struct RGB { uint8_t R, G, B; };
int main() {
    std::vector<RGB> pixels(2048);
    unsigned count = 0;
    for (const auto& clip : BuiltinMedia::Clips) {
        if (!MediaCodec::Header(clip.data, clip.size)) return 1;
        uint32_t offset = 12;
        for (unsigned i = 0; i < MediaCodec::U16(clip.data + 6); ++i) {
            const auto* f = clip.data + offset;
            const uint16_t n = MediaCodec::U16(f + 2);
            if (MediaCodec::U16(f) < 20 || offset + 4 + n > clip.size || !MediaCodec::Decode(f + 4, n, pixels.data())) return 2;
            offset += 4 + n; ++count;
        }
        if (offset != clip.size) return 3;
    }
    for (unsigned code = 0; code < 4; ++code) {
        const bool wide = code >= 2, rle = code & 1;
        std::vector<uint8_t> data = {uint8_t(code)};
        for (unsigned i = 0; i < 2048;) {
            unsigned run = rle ? std::min(255u, 2048 - i) : 1;
            if (rle) data.push_back(uint8_t(run));
            data.push_back(0xff); if (wide) data.push_back(0xff); i += run;
        }
        if (!MediaCodec::Decode(data.data(), data.size(), pixels.data())) return 4;
        for (const auto& p : pixels) if (p.R != 255 || p.G != 255 || p.B != 255) return 5;
        data.pop_back(); if (MediaCodec::Decode(data.data(), data.size(), pixels.data())) return 6;
    }
    const uint8_t zeroRun[] = {1, 0, 255};
    if (MediaCodec::Decode(zeroRun, sizeof(zeroRun), pixels.data())) return 7;
    std::printf("PASS media codec: 4 formats, truncation/zero run, %u built-in frames\n", count);
}
