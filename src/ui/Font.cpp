#include "ui/Font.h"

#include "gfx/UIRenderer.h"

#include <stb_image.h>

#include <cstdint>

namespace mc {
namespace {

// Minimal UTF-8 decode: returns the codepoint at i and advances it. Anything beyond
// the two-byte range (or malformed) comes back as '?'.
uint32_t nextCodepoint(const std::string& s, size_t& i) {
    unsigned char b = static_cast<unsigned char>(s[i++]);
    if (b < 0x80) return b;
    if ((b & 0xE0) == 0xC0 && i < s.size()) {
        unsigned char b2 = static_cast<unsigned char>(s[i++]);
        return ((b & 0x1Fu) << 6) | (b2 & 0x3Fu);
    }
    if ((b & 0xF0) == 0xE0) i += (i + 1 < s.size()) ? 2 : 0;
    else if ((b & 0xF8) == 0xF0) i += (i + 2 < s.size()) ? 3 : 0;
    return '?';
}

// accented.png rows 0-3 (16 cells each), layout verified against the texture:
// Latin-1 order minus the glyphs vanilla keeps in ascii.png's CP437 half.
constexpr uint32_t kAccented[4][16] = {
    {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF},
    {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xE0, 0xE1, 0xE2, 0xE3},
    {0xE4, 0xE5, 0xE6, 0xE7, 0xEC, 0xED, 0xEE, 0xEF, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF9, 0xFA},
    {0xFB, 0xFC, 0xFD, 0xFF, 0x100, 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107, 0x108, 0x109, 0x10A, 0x10B},
};

// Lowercase e-accents live only in ascii.png's CP437 area (vanilla layout).
int cp437For(uint32_t cp) {
    switch (cp) {
        case 0x00E8: return 0x8A; // e-grave
        case 0x00E9: return 0x82; // e-acute
        case 0x00EA: return 0x88; // e-circumflex
        case 0x00EB: return 0x89; // e-diaeresis
        default: return -1;
    }
}

// Many packs leave that legacy area empty (this one does), and accented.png skips
// these four on purpose — so compose them: the plain 'e' glyph plus a hand-placed
// pixel accent in the free top rows of the cell.
struct Accent {
    int n;
    int px[3][2]; // accent pixels in font px, relative to the glyph's top-left
};
const Accent* accentFor(uint32_t cp) {
    static const Accent kGrave{2, {{1, 0}, {2, 1}, {0, 0}}};
    static const Accent kAcute{2, {{3, 0}, {2, 1}, {0, 0}}};
    static const Accent kCirc{3, {{2, 0}, {1, 1}, {3, 1}}};
    static const Accent kDia{2, {{1, 1}, {3, 1}, {0, 0}}};
    switch (cp) {
        case 0x00E8: return &kGrave;
        case 0x00E9: return &kAcute;
        case 0x00EA: return &kCirc;
        case 0x00EB: return &kDia;
        default: return nullptr;
    }
}

} // namespace

void Font::init(UIRenderer& ui, const std::string& asciiPngPath,
                const std::string& accentedPngPath) {
    widths_.fill(6);

    int w = 0, h = 0, ch = 0;
    stbi_uc* data = stbi_load(asciiPngPath.c_str(), &w, &h, &ch, 4);
    if (data && w == 128 && h == 128) {
        for (int c = 0; c < 256; ++c) {
            int cellX = (c % 16) * 8;
            int cellY = (c / 16) * 8;
            int rightmost = -1;
            for (int gy = 0; gy < 8; ++gy) {
                for (int gx = 0; gx < 8; ++gx) {
                    uint8_t a = data[((cellY + gy) * w + (cellX + gx)) * 4 + 3];
                    if (a > 0 && gx > rightmost) rightmost = gx;
                }
            }
            widths_[c] = rightmost + 1; // 0 for blank glyphs
        }
        widths_[static_cast<int>(' ')] = 3;
    }
    if (data) stbi_image_free(data);

    texId_ = ui.registerTexture(asciiPngPath);

    // Accented atlas: 16 columns of 9x12 cells; widths measured like the ascii ones.
    if (!accentedPngPath.empty()) {
        int aw = 0, ah = 0, ach = 0;
        stbi_uc* adata = stbi_load(accentedPngPath.c_str(), &aw, &ah, &ach, 4);
        if (adata && aw >= 144 && ah >= 48) {
            accW_ = static_cast<float>(aw);
            accH_ = static_cast<float>(ah);
            for (int row = 0; row < 4; ++row) {
                for (int col = 0; col < 16; ++col) {
                    int cellX = col * 9, cellY = row * 12;
                    int rightmost = -1;
                    for (int gy = 0; gy < 12; ++gy) {
                        for (int gx = 0; gx < 9; ++gx) {
                            uint8_t a = adata[((cellY + gy) * aw + (cellX + gx)) * 4 + 3];
                            if (a > 0 && gx > rightmost) rightmost = gx;
                        }
                    }
                    if (rightmost >= 0) {
                        ext_[kAccented[row][col]] = {cellX, cellY, rightmost + 1};
                    }
                }
            }
            accTexId_ = ui.registerTexture(accentedPngPath);
        }
        if (adata) stbi_image_free(adata);
    }
}

float Font::textWidth(const std::string& text, float scale) const {
    float x = 0.0f;
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = nextCodepoint(text, i);
        int cb = cp437For(cp);
        if (cb >= 0) {
            // Legacy glyph if the pack has one, composed 'e' + accent otherwise.
            x += ((widths_[cb] > 0 ? widths_[cb] : widths_[static_cast<int>('e')]) + 1) * scale;
            continue;
        }
        if (cp < 0x100 && (cp < 0x80 || ext_.find(cp) == ext_.end())) {
            x += (widths_[cp & 0xFF] + 1) * scale;
            continue;
        }
        auto it = ext_.find(cp);
        x += ((it != ext_.end() ? it->second.w : widths_[static_cast<int>('?')]) + 1) * scale;
    }
    return x;
}

void Font::drawRaw(UIRenderer& ui, const std::string& text, float x, float y, float scale,
                   const glm::vec4& color) const {
    float cursor = x;
    float gsize = 8.0f * scale;
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = nextCodepoint(text, i);
        int cb = cp437For(cp);
        if (cb >= 0) {
            if (widths_[cb] > 0) {
                cp = static_cast<uint32_t>(cb); // the pack ships the legacy glyph
            } else if (const Accent* acc = accentFor(cp)) {
                unsigned char e = 'e';
                float u0 = (e % 16) / 16.0f, v0 = (e / 16) / 16.0f;
                ui.texQuad(texId_, cursor, y, gsize, gsize, u0, v0, u0 + 1.0f / 16.0f,
                           v0 + 1.0f / 16.0f, color);
                for (int k = 0; k < acc->n; ++k) {
                    ui.quad(cursor + acc->px[k][0] * scale, y + acc->px[k][1] * scale,
                            scale, scale, color);
                }
                cursor += (widths_[e] + 1) * scale;
                continue;
            }
        }

        if (cp < 0x80 || (cp < 0x100 && ext_.find(cp) == ext_.end())) {
            unsigned char c = static_cast<unsigned char>(cp);
            if (c != ' ' && widths_[c] > 0) {
                float u0 = (c % 16) / 16.0f;
                float v0 = (c / 16) / 16.0f;
                ui.texQuad(texId_, cursor, y, gsize, gsize, u0, v0, u0 + 1.0f / 16.0f,
                           v0 + 1.0f / 16.0f, color);
            }
            cursor += (widths_[c] + 1) * scale;
            continue;
        }
        auto it = ext_.find(cp);
        if (it != ext_.end() && accTexId_ > 0) {
            // 9x12 cell whose letter body starts 3px down (accents poke above the
            // line, descenders below), so it aligns with the 8px ascii glyphs.
            const ExtGlyph& g = it->second;
            float u0 = g.cellX / accW_, v0 = g.cellY / accH_;
            ui.texQuad(accTexId_, cursor, y - 3.0f * scale, 9.0f * scale, 12.0f * scale,
                       u0, v0, u0 + 9.0f / accW_, v0 + 12.0f / accH_, color);
            cursor += (g.w + 1) * scale;
        } else {
            unsigned char q = '?';
            float u0 = (q % 16) / 16.0f, v0 = (q / 16) / 16.0f;
            ui.texQuad(texId_, cursor, y, gsize, gsize, u0, v0, u0 + 1.0f / 16.0f,
                       v0 + 1.0f / 16.0f, color);
            cursor += (widths_[q] + 1) * scale;
        }
    }
}

float Font::drawText(UIRenderer& ui, const std::string& text, float x, float y, float scale,
                     const glm::vec4& color, bool shadow) const {
    if (shadow) {
        drawRaw(ui, text, x + scale, y + scale, scale, glm::vec4(0.12f, 0.12f, 0.12f, color.a));
    }
    drawRaw(ui, text, x, y, scale, color);
    return textWidth(text, scale);
}

} // namespace mc
