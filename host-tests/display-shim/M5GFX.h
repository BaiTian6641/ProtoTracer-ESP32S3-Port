#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

constexpr uint16_t TFT_BLACK = 0;
constexpr uint16_t TFT_WHITE = 0xffff;

namespace hostglyph
{
#define PROGMEM
#include <lgfx/Fonts/glcdfont.h>
#undef PROGMEM
}

namespace lgfx { struct IFont { int width; int height; }; }
namespace fonts
{
    inline const lgfx::IFont Font0{6, 8};
    inline const lgfx::IFont TomThumb{4, 6};
}
enum class textdatum_t { top_left, middle_center };
struct TextStyle
{
    float size_x = 1, size_y = 1;
    textdatum_t datum = textdatum_t::top_left;
    unsigned padding_x = 0;
    uint16_t foreground = TFT_WHITE, background = TFT_BLACK;
};

struct String : std::string
{
    using std::string::string;
    bool isEmpty() const { return empty(); }
};

struct M5GFX
{
    int width() const { return 128; }
    int height() const { return 64; }
    void startWrite() {}
    void endWrite() {}
    void display() { ++displays; }
    M5GFX* panel() { return this; }
    void setSleep(bool value) { asleep = value; ++sleepWrites; }
    void writeCommand(uint8_t command) { commands.push_back(command); }
    const TextStyle& getTextStyle() const { return style; }
    void setTextStyle(const TextStyle& value) { style = value; }
    const lgfx::IFont* getFont() const { return currentFont; }
    void setFont(const lgfx::IFont* value) { currentFont = value; }
    void setTextSize(float value) { style.size_x = style.size_y = value; }
    void setTextDatum(textdatum_t value) { style.datum = value; }
    void setTextPadding(unsigned value) { style.padding_x = value; }
    void setTextColor(uint16_t foreground, uint16_t background)
    {
        style.foreground = foreground;
        style.background = background;
    }
    void drawString(const char* text, int x, int y, const lgfx::IFont* font)
    {
        drawnStrings.emplace_back(text);
        for (const unsigned char* ch = reinterpret_cast<const unsigned char*>(text); *ch; ++ch)
        {
            for (int row = 0; row < font->height - 1; ++row)
                for (int col = 0; col < font->width - 1; ++col)
                {
                    const int sourceCol = font == &fonts::Font0 ? col : col * 2;
                    const int sourceRow = font == &fonts::Font0 ? row : row * 6 / 4;
                    if ((hostglyph::font[*ch * 5 + sourceCol] >> sourceRow) & 1)
                        fillRect(x + col, y + row, 1, 1, style.foreground);
                }
            x += font->width;
        }
    }
    void fillScreen(uint16_t color) { pixels.fill(color ? 255 : 0); drawnStrings.clear(); }
    void fillRect(int x, int y, int w, int h, uint16_t color)
    {
        for (int row = y; row < y + h; ++row)
            for (int col = x; col < x + w; ++col)
                if (row >= 0 && row < height() && col >= 0 && col < width())
                    pixels[row * width() + col] = color ? 255 : 0;
    }
    std::array<uint8_t, 128 * 64> pixels{};
    std::vector<uint8_t> commands;
    std::vector<std::string> drawnStrings;
    TextStyle style;
    const lgfx::IFont* currentFont = &fonts::Font0;
    bool asleep = false;
    int sleepWrites = 0;
    int displays = 0;
};
