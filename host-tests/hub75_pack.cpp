// Executes the actual production packer against the pinned driver's old loop.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#define PIXEL_COLOR_DEPTH_BITS 8
#include "cie_luts.h"

#define ESP32_I2S_DMA_STORAGE_TYPE uint16_t
#define PIXELS_PER_ROW 128
#define ROWS_PER_FRAME 16
#define BITMASK_RGB12_CLEAR 0xffc0
#define SPIRAM_DMA_BUFFER 1
#define HUB75_SPIRAM_DEFERRED_CACHE_FLUSH 1
#define getRowDataPtr(row, bp) (words[row] + (bp) * 128)
#define ESP32_TX_FIFO_POSITION_ADJUST(x) (fifoSwap ? ((x) ^ 1) : (x))
#define DO_BRIGHTNESS_COMPENSATION() \
    auto red_val = Correct(red); auto green_val = Correct(green); auto blue_val = Correct(blue);

struct Packer
{
    struct Config { unsigned depth = 8; uint8_t getPixelColorDepthBits() { return uint8_t(depth); } } m_cfg;
    bool initialized = true, fifoSwap = false, linear = false;
    uint16_t words[16][12 * 128];
    struct Frame {
        unsigned dirty = 0;
        void markDirty(unsigned row) { dirty |= 1u << row; }
    } frame;
    Frame* fb = &frame;
    unsigned flushed = 0;
    void flushDMAFramebuffer(bool all)
    {
        flushed = all ? 0xffff : frame.dirty;
        frame.dirty = 0;
    }
    uint16_t Correct(uint8_t v)
    {
        if (!linear) return lumConvTab[v];
        unsigned shift = 16 - m_cfg.depth;
        return uint16_t(std::min(((unsigned(v) * 256 + (1u << (shift - 1)) - 1) >> shift),
                                (1u << m_cfg.depth) - 1));
    }
    void Pack(const uint8_t* rgb888)
    {
#include "../tools/hub75_chained.inc"
    }
    void Legacy(const uint8_t* rgb888)
    {
        for (unsigned bp = 0; bp < m_cfg.depth; ++bp)
            for (unsigned sy = 0; sy < 32; ++sy) {
                const uint8_t* source = rgb888 + (31 - sy) * 64 * 3;
                uint16_t* p = words[sy % 16] + bp * 128;
                const unsigned offset = sy >= 16 ? 3 : 0;
                const uint16_t clear = sy >= 16 ? 0xffc7 : 0xfff8;
                for (unsigned x = 0; x < 64; ++x) {
                    uint16_t bits = ((Correct(source[x * 3]) >> bp) & 1) |
                        (((Correct(source[x * 3 + 1]) >> bp) & 1) << 1) |
                        (((Correct(source[x * 3 + 2]) >> bp) & 1) << 2);
                    bits <<= offset;
                    const unsigned left = ESP32_TX_FIFO_POSITION_ADJUST(x);
                    const unsigned right = ESP32_TX_FIFO_POSITION_ADJUST(127 - x);
                    p[left] = (p[left] & clear) | bits;
                    p[right] = (p[right] & clear) | bits;
                }
            }
    }
};

int main()
{
    Packer old, current;
    uint8_t rgb[6144];
    uint32_t random = 1234567;
    auto next = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
    unsigned cases = 0;
    for (unsigned mode = 0; mode < 2; ++mode)
        for (unsigned depth = 2; depth <= 12; ++depth)
            for (unsigned swap = 0; swap < 2; ++swap)
                for (unsigned frame = 0; frame < 32; ++frame) {
                    old.m_cfg.depth = current.m_cfg.depth = depth;
                    old.linear = current.linear = mode != 0;
                    old.fifoSwap = current.fifoSwap = swap != 0;
                    for (auto& byte : rgb) byte = frame < 8 ? uint8_t(frame * 32 + (&byte - rgb) % 32) : uint8_t(next());
                    for (auto& row : old.words) for (auto& word : row) word = uint16_t(next());
                    memcpy(current.words, old.words, sizeof(old.words));
                    rgb[17 * 64 * 3] = 0;
                    old.Legacy(rgb);
                    current.Pack(rgb);
                    if (memcmp(old.words, current.words, sizeof(old.words))) {
                        printf("FAIL mode=%u depth=%u swap=%u frame=%u\n", mode, depth, swap, frame);
                        return 1;
                    }
                    // A repeated frame needs no writeback; a single changed source
                    // row must invalidate its matching physical row, including both panels.
                    current.Pack(rgb);
                    if (current.flushed != 0) return 2;
                    rgb[17 * 64 * 3] ^= 255;
                    current.frame.dirty = 1u << 15; // retain another drawing operation's mark
                    current.Pack(rgb);
                    old.Legacy(rgb);
                    if (memcmp(old.words, current.words, sizeof(old.words)) ||
                        current.flushed != ((1u << 14) | (1u << 15))) return 3;
                    ++cases;
                }
    printf("PASS HUB75 pack: %u cases, complete DMA words/control bits byte-identical\n", cases);
}
