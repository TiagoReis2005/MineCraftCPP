#pragma once

#include <glm/glm.hpp>

#include <array>
#include <string>
#include <unordered_map>

namespace mc {

class UIRenderer;

// Bitmap font using the classic 128x128 (16x16 grid of 8x8 glyphs) ascii.png atlas,
// plus vanilla's accented.png (16 columns of 9x12 glyphs) for Latin accents. Strings
// are UTF-8: ASCII draws from ascii.png, Latin-1/Extended-A accents from accented.png
// (rows 0-3, verified layout), and e-grave/acute/circumflex/diaeresis from ascii.png's
// CP437 half. Glyph widths are measured from the atlases so text is proportional.
class Font {
public:
    void init(UIRenderer& ui, const std::string& asciiPngPath,
              const std::string& accentedPngPath = "");

    // Draws text (top-left origin); scale = screen pixels per font pixel. Returns pixel width.
    float drawText(UIRenderer& ui, const std::string& text, float x, float y, float scale,
                   const glm::vec4& color, bool shadow = true) const;
    float textWidth(const std::string& text, float scale) const;
    float lineHeight(float scale) const { return 8.0f * scale; }

private:
    int texId_ = 0;
    int accTexId_ = 0;
    float accW_ = 1.0f, accH_ = 1.0f; // accented.png dimensions (UV divisors)
    std::array<int, 256> widths_{};
    struct ExtGlyph {
        int cellX = 0, cellY = 0; // pixel origin of the 9x12 cell in accented.png
        int w = 0;                // measured glyph width
    };
    std::unordered_map<uint32_t, ExtGlyph> ext_; // codepoint -> accented.png glyph

    void drawRaw(UIRenderer& ui, const std::string& text, float x, float y, float scale,
                 const glm::vec4& color) const;
};

} // namespace mc
