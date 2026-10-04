// PROTOTRACER_CHAINED_PACK_V1: included inside the pinned driver's method.
// CIE correction once per input pixel; one combined RGB1/RGB2 write per word.
if (!initialized || !rgb888) return;
const uint8_t cdepth = m_cfg.getPixelColorDepthBits();
const uint16_t physW = PIXELS_PER_ROW;
const uint16_t rpf = ROWS_PER_FRAME;
if (physW != 128 || rpf != 16 || cdepth > 12) return;
static constexpr auto spread = []() {
    std::array<uint32_t, 256> values{};
    for (unsigned value = 0; value < 256; ++value)
        for (unsigned bit = 0; bit < 8; ++bit)
            values[value] |= ((value >> bit) & 1u) << (bit * 3);
    return values;
}();

for (uint16_t row = 0; row < 16; ++row)
{
    // About 1.5 KiB stack at most; no persistent internal heap allocation.
    union {
        uint16_t corrected[6][64];
        uint32_t packed[2][64];
    } values;
    bool changed = false;
    for (uint16_t x = 0; x < 64; ++x)
    {
        for (uint16_t half = 0; half < 2; ++half)
        {
            const uint8_t* pixel = rgb888 + ((31 - row - half * 16) * 64 + x) * 3;
            uint8_t red = pixel[0], green = pixel[1], blue = pixel[2];
            DO_BRIGHTNESS_COMPENSATION()
            if (cdepth <= 8) {
                values.packed[half][x] = spread[red_val & 255] |
                    (spread[green_val & 255] << 1) | (spread[blue_val & 255] << 2);
            } else {
                values.corrected[half * 3][x] = red_val;
                values.corrected[half * 3 + 1][x] = green_val;
                values.corrected[half * 3 + 2][x] = blue_val;
            }
        }
    }
    for (uint8_t bp = 0; bp < cdepth; ++bp)
    {
        ESP32_I2S_DMA_STORAGE_TYPE* p = getRowDataPtr(row, bp);
        for (uint16_t x = 0; x < 64; ++x)
        {
            const uint16_t bits = cdepth <= 8 ?
                ((values.packed[0][x] >> (bp * 3)) & 7) |
                (((values.packed[1][x] >> (bp * 3)) & 7) << 3) :
                ((values.corrected[0][x] >> bp) & 1) |
                (((values.corrected[1][x] >> bp) & 1) << 1) |
                (((values.corrected[2][x] >> bp) & 1) << 2) |
                (((values.corrected[3][x] >> bp) & 1) << 3) |
                (((values.corrected[4][x] >> bp) & 1) << 4) |
                (((values.corrected[5][x] >> bp) & 1) << 5);
            const uint16_t left = ESP32_TX_FIFO_POSITION_ADJUST(x);
            const uint16_t right = ESP32_TX_FIFO_POSITION_ADJUST(uint16_t(127 - x));
            const uint16_t leftValue = (p[left] & BITMASK_RGB12_CLEAR) | bits;
            const uint16_t rightValue = (p[right] & BITMASK_RGB12_CLEAR) | bits;
            if (p[left] != leftValue) { p[left] = leftValue; changed = true; }
            if (p[right] != rightValue) { p[right] = rightValue; changed = true; }
        }
#if defined(SPIRAM_DMA_BUFFER) && !HUB75_SPIRAM_DEFERRED_CACHE_FLUSH
        Cache_WriteBack_Addr((uint32_t)p, physW * sizeof(ESP32_I2S_DMA_STORAGE_TYPE));
#endif
    }
#if defined(SPIRAM_DMA_BUFFER) && HUB75_SPIRAM_DEFERRED_CACHE_FLUSH
    if (changed) fb->markDirty(row);
#endif
}
#if defined(SPIRAM_DMA_BUFFER) && HUB75_SPIRAM_DEFERRED_CACHE_FLUSH
// The back buffer contains the frame from two flips ago. Compare against that
// actual buffer, and flush only rows whose DMA words changed. Existing dirty
// marks from other drawing operations are retained and flushed as well.
flushDMAFramebuffer(false);
#endif
