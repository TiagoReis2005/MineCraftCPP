#include "ui/MenuScreen.h"

#include "core/KeyBinds.h"
#include "core/Lang.h"
#include "core/Settings.h"
#include "gfx/UIRenderer.h"
#include "ui/Font.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mc {
namespace {

constexpr float kScale = 3.0f; // gui px -> screen px (matches the HUD/inventory)
constexpr float kBtnW = 200.0f * kScale;
constexpr float kBtnH = 20.0f * kScale;
constexpr float kHalfW = 98.0f * kScale;  // pause/worlds halves (aligned to kBtnW)
constexpr float kOptW = 172.0f * kScale;  // options sub-page columns (wider: long labels)
constexpr float kSquare = 20.0f * kScale;
constexpr float kGap = 4.0f * kScale;
constexpr float kHandleW = 8.0f * kScale;
constexpr float kListW = 260.0f * kScale;
constexpr float kRowH = 30.0f * kScale;

const glm::vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};
const glm::vec4 kGray{0.63f, 0.63f, 0.63f, 1.0f};

const float kFillNormal[4] = {0.42f, 0.42f, 0.42f, 0.92f};
const float kFillHover[4] = {0.44f, 0.48f, 0.62f, 0.95f};
const float kFillDisabled[4] = {0.25f, 0.25f, 0.25f, 0.92f};
const float kFillField[4] = {0.05f, 0.05f, 0.05f, 0.95f};

bool inside(const MenuScreen::Input& in, float x, float y, float w, float h) {
    return in.mouseX >= x && in.mouseX < x + w && in.mouseY >= y && in.mouseY < y + h;
}

std::string trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(' ');
    size_t b = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

} // namespace

std::string MenuScreen::tr(const char* key, const char* fallback) const {
    if (!lang_) return fallback;
    return lang_->tr(key, fallback);
}

// Widget sprites are 200x20 with a 4px border ring: 3-slice horizontally so any width
// keeps crisp corners (heights match 20 gui px everywhere, so vertical stretch is 1:1).
void MenuScreen::drawWidget(UIRenderer& ui, int tex, float x, float y, float w, float h,
                            const float fallback[4]) {
    if (tex <= 0) {
        glm::vec4 fill(fallback[0], fallback[1], fallback[2], fallback[3]);
        float b = kScale;
        ui.quad(x - b, y - b, w + 2.0f * b, h + 2.0f * b, glm::vec4(0.0f, 0.0f, 0.0f, 0.8f));
        ui.quad(x, y, w, h, fill);
        return;
    }
    const float cu = 4.0f / 200.0f;  // corner width in UV
    float cw = std::min(4.0f * kScale, w * 0.5f);
    ui.texQuad(tex, x, y, cw, h, 0.0f, 0.0f, cu, 1.0f);
    ui.texQuad(tex, x + cw, y, w - 2.0f * cw, h, cu, 0.0f, 1.0f - cu, 1.0f);
    ui.texQuad(tex, x + w - cw, y, cw, h, 1.0f - cu, 0.0f, 1.0f, 1.0f);
}

bool MenuScreen::button(UIRenderer& ui, const Font& font, float x, float y, float w, float h,
                        const std::string& label, const Input& in, bool enabled) {
    bool hovered = enabled && inside(in, x, y, w, h);
    hoverAny_ = hoverAny_ || inside(in, x, y, w, h);
    int tex = !enabled ? tex_.buttonDisabled : (hovered ? tex_.buttonHighlighted : tex_.button);
    drawWidget(ui, tex, x, y, w, h, !enabled ? kFillDisabled : (hovered ? kFillHover : kFillNormal));
    float tw = font.textWidth(label, kScale);
    font.drawText(ui, label, x + (w - tw) * 0.5f, y + (h - font.lineHeight(kScale)) * 0.5f,
                  kScale, enabled ? kWhite : kGray);
    return hovered && in.leftPressed;
}

bool MenuScreen::iconButton(UIRenderer& ui, float x, float y, int icon, const Input& in) {
    bool hovered = inside(in, x, y, kSquare, kSquare);
    hoverAny_ = hoverAny_ || hovered;
    int tex = hovered ? tex_.buttonHighlighted : tex_.button;
    drawWidget(ui, tex, x, y, kSquare, kSquare, hovered ? kFillHover : kFillNormal);
    if (icon > 0) {
        float pad = 3.0f * kScale; // 14x14 gui icon centered in the 20x20 square
        ui.sprite(icon, x + pad, y + pad, kSquare - 2.0f * pad, kSquare - 2.0f * pad);
    }
    return hovered && in.leftPressed;
}

float MenuScreen::slider(UIRenderer& ui, const Font& font, int id, float x, float y, float w,
                         float h, const std::string& label, float t, const Input& in) {
    bool hovered = inside(in, x, y, w, h);
    hoverAny_ = hoverAny_ || hovered;
    if (hovered && in.leftPressed) activeSlider_ = id;
    bool dragging = activeSlider_ == id;
    if (dragging && in.leftDown) {
        t = std::clamp((in.mouseX - x - kHandleW * 0.5f) / (w - kHandleW), 0.0f, 1.0f);
    }

    drawWidget(ui, tex_.slider, x, y, w, h, kFillField);
    float hx = x + t * (w - kHandleW);
    int handleTex = (hovered || dragging) ? tex_.sliderHandleHighlighted : tex_.sliderHandle;
    if (handleTex > 0) {
        ui.sprite(handleTex, hx, y, kHandleW, h); // handle art is 8x20: drawn 1:1
    } else {
        ui.quad(hx, y, kHandleW, h,
                (hovered || dragging) ? glm::vec4(0.55f, 0.58f, 0.70f, 1.0f)
                                      : glm::vec4(0.48f, 0.48f, 0.48f, 1.0f));
    }

    float tw = font.textWidth(label, kScale);
    font.drawText(ui, label, x + (w - tw) * 0.5f, y + (h - font.lineHeight(kScale)) * 0.5f,
                  kScale, kWhite);
    return t;
}

void MenuScreen::textField(UIRenderer& ui, const Font& font, int id, float x, float y, float w,
                           float h, std::string& value, const Input& in, size_t maxLen) {
    bool hovered = inside(in, x, y, w, h);
    hoverAny_ = hoverAny_ || hovered;
    if (in.leftPressed) {
        if (hovered) focusField_ = id;
        else if (focusField_ == id) focusField_ = -1;
    }
    bool focused = focusField_ == id;

    if (focused) {
        for (char ch : in.typed) {
            if (value.size() < maxLen) value += ch;
        }
        if (in.backspace && !value.empty()) value.pop_back();
    }

    drawWidget(ui, (focused || hovered) ? tex_.textFieldHighlighted : tex_.textField,
               x, y, w, h, kFillField);
    bool caret = focused && std::fmod(in.time, 0.7) < 0.35;
    std::string shown = value + (caret ? "_" : "");
    font.drawText(ui, shown, x + 4.0f * kScale, y + (h - font.lineHeight(kScale)) * 0.5f,
                  kScale, kWhite);
}

MenuScreen::Action MenuScreen::build(UIRenderer& ui, const Font& font, uint32_t screenW,
                                     uint32_t screenH, const Input& in, Settings& settings) {
    if (!in.leftDown) activeSlider_ = -1;
    hoverAny_ = false;
    switch (page_) {
        case Page::Title:         return buildTitle(ui, font, screenW, screenH, in, settings);
        case Page::Worlds:        return buildWorlds(ui, font, screenW, screenH, in);
        case Page::CreateWorld:   return buildCreate(ui, font, screenW, screenH, in);
        case Page::ConfirmDelete: return buildConfirmDelete(ui, font, screenW, screenH, in);
        case Page::Pause:         return buildPause(ui, font, screenW, screenH, in);
        case Page::Options:       return buildOptions(ui, font, screenW, screenH, in, settings);
        case Page::Language:      return buildLanguage(ui, font, screenW, screenH, in, settings);
        case Page::SkinCustom:    buildSkinCustom(ui, font, screenW, screenH, in, settings); break;
        case Page::MusicSounds:   buildMusicSounds(ui, font, screenW, screenH, in, settings); break;
        case Page::VideoSettings: buildVideo(ui, font, screenW, screenH, in, settings); break;
        case Page::Controls:      buildControls(ui, font, screenW, screenH, in, settings); break;
        case Page::MouseSettings: buildMouse(ui, font, screenW, screenH, in, settings); break;
        case Page::KeyBinds:      buildKeyBinds(ui, font, screenW, screenH, in); break;
        case Page::ChatSettings:  buildChat(ui, font, screenW, screenH, in, settings); break;
        case Page::Accessibility: buildAccessibility(ui, font, screenW, screenH, in, settings); break;
        case Page::ResourcePacks: buildResourcePacks(ui, font, screenW, screenH, in); break;
        case Page::None:          break;
    }
    return Action::None;
}

std::string MenuScreen::createName() const {
    std::string n = trimmed(nameField_);
    return n.empty() ? "New World" : n;
}

MenuScreen::Action MenuScreen::buildTitle(UIRenderer& ui, const Font& font, uint32_t sw,
                                          uint32_t sh, const Input& in, Settings& s) {
    float w = static_cast<float>(sw), h = static_cast<float>(sh);

    float ts = kScale * 3.0f;
    const std::string title = "MineCraftCPP";
    font.drawText(ui, title, (w - font.textWidth(title, ts)) * 0.5f, h * 0.12f, ts, kWhite);

    Action a = Action::None;
    float x = (w - kBtnW) * 0.5f;
    float y = h * 0.34f;
    if (button(ui, font, x, y, kBtnW, kBtnH, tr("menu.singleplayer", "Singleplayer"), in)) {
        a = Action::OpenWorlds;
    }
    y += kBtnH + kGap;
    button(ui, font, x, y, kBtnW, kBtnH, tr("menu.multiplayer", "Multiplayer"), in,
           /*enabled=*/false);
    y += kBtnH + kGap;
    button(ui, font, x, y, kBtnW, kBtnH, tr("menu.online", "Minecraft Realms"), in,
           /*enabled=*/false);
    y += kBtnH + kGap;

    // Icon squares (friends / language / accessibility), centered under Realms.
    float sqRowW = 3.0f * kSquare + 2.0f * kGap;
    float rx = (w - sqRowW) * 0.5f;
    iconButton(ui, rx, y, tex_.iconFriends, in);
    if (iconButton(ui, rx + kSquare + kGap, y, tex_.iconLanguage, in)) {
        langReturn_ = Page::Title;
        a = Action::OpenLanguages;
    }
    iconButton(ui, rx + 2.0f * (kSquare + kGap), y, tex_.iconAccessibility, in);
    y += kSquare + kGap * 2.0f;

    // Options / Quit halves.
    float halfRowW = 2.0f * kHalfW + kGap;
    rx = (w - halfRowW) * 0.5f;
    if (button(ui, font, rx, y, kHalfW, kBtnH, tr("menu.options", "Options..."), in)) {
        returnPage_ = Page::Title;
        page_ = Page::Options;
    }
    if (button(ui, font, rx + kHalfW + kGap, y, kHalfW, kBtnH, tr("menu.quit", "Quit Game"),
               in)) {
        a = Action::Quit;
    }

    // Right column: name box, the player-model preview (offscreen render drawn as a
    // sprite; dragging over it spins the model), and the skin upload button.
    float colW = 110.0f * kScale;
    float cx = w * 0.78f - colW * 0.5f;
    font.drawText(ui, tr("profile.name", "Player Name"), cx,
                  h * 0.20f - font.lineHeight(kScale) - 2.0f, kScale, kGray);
    textField(ui, font, 20, cx, h * 0.20f, colW, kBtnH, s.playerName, in, 16);
    if (tex_.playerPreview > 0) {
        float top = h * 0.20f + kBtnH + kGap;
        float bottom = h * 0.80f - kGap;
        float ph = bottom - top;         // 1:2 texture drawn letterboxed
        float pw = ph * 0.5f;
        ui.sprite(tex_.playerPreview, w * 0.78f - pw * 0.5f, top, pw, ph);
    }
    if (button(ui, font, cx, h * 0.80f, colW, kBtnH, tr("profile.uploadSkin", "Upload Skin..."),
               in)) {
        a = Action::UploadSkin;
    }

    font.drawText(ui, "MineCraftCPP (Vulkan) pre-alpha", 2.0f * kScale,
                  h - font.lineHeight(kScale) - 2.0f * kScale, kScale,
                  glm::vec4(1.0f, 1.0f, 1.0f, 0.75f));
    std::string who = s.playerName + " / " + (lang_ ? lang_->displayName() : s.language);
    font.drawText(ui, who, w - font.textWidth(who, kScale) - 2.0f * kScale,
                  h - font.lineHeight(kScale) - 2.0f * kScale, kScale,
                  glm::vec4(1.0f, 1.0f, 1.0f, 0.75f));
    return a;
}

MenuScreen::Action MenuScreen::buildWorlds(UIRenderer& ui, const Font& font, uint32_t sw,
                                           uint32_t sh, const Input& in) {
    float w = static_cast<float>(sw), h = static_cast<float>(sh);
    ui.quad(0.0f, 0.0f, w, h, glm::vec4(0.0f, 0.0f, 0.0f, 0.60f));

    const std::string head = tr("selectWorld.title", "Select World");
    float hs = kScale * 1.5f;
    font.drawText(ui, head, (w - font.textWidth(head, hs)) * 0.5f, h * 0.05f, hs, kWhite);

    Action a = Action::None;
    float listX = (w - kListW) * 0.5f;
    float listY = h * 0.13f;
    float buttonsY = h - 3.0f * (kBtnH + kGap) - 4.0f * kScale;
    float listH = buttonsY - listY - 2.0f * kGap;
    int visible = std::max(1, static_cast<int>(listH / (kRowH + 2.0f)));

    if (in.scroll != 0.0) {
        worldScroll_ -= static_cast<int>(in.scroll);
    }
    int maxScroll = std::max(0, static_cast<int>(worlds_.size()) - visible);
    worldScroll_ = std::clamp(worldScroll_, 0, maxScroll);

    if (worlds_.empty()) {
        const std::string none = tr("selectWorld.empty", "No worlds yet - create one!");
        font.drawText(ui, none, (w - font.textWidth(none, kScale)) * 0.5f,
                      listY + listH * 0.5f, kScale, kGray);
    }

    float rowY = listY;
    for (int i = worldScroll_; i < static_cast<int>(worlds_.size()); ++i) {
        if (rowY + kRowH > listY + listH) break;
        const WorldEntry& e = worlds_[static_cast<size_t>(i)];
        bool selected = i == selectedWorld_;
        bool hovered = inside(in, listX, rowY, kListW, kRowH);
        if (selected) { // white selection frame around the row
            ui.quad(listX - kScale, rowY - kScale, kListW + 2.0f * kScale,
                    kRowH + 2.0f * kScale, glm::vec4(1.0f, 1.0f, 1.0f, 0.9f));
        }
        ui.quad(listX, rowY, kListW, kRowH,
                glm::vec4(0.0f, 0.0f, 0.0f, hovered ? 0.65f : 0.5f));
        font.drawText(ui, e.name, listX + 3.0f * kScale, rowY + 3.0f * kScale, kScale, kWhite);
        font.drawText(ui, e.subtitle, listX + 3.0f * kScale, rowY + 15.0f * kScale,
                      kScale * 2.0f / 3.0f * 1.0f, kGray);
        if (hovered && in.leftPressed) {
            if (selectedWorld_ == i && lastRowClicked_ == i &&
                in.time - lastRowClickTime_ < 0.35) {
                actionFolder_ = e.folder; // double-click = play
                a = Action::PlayWorld;
            }
            selectedWorld_ = i;
            lastRowClicked_ = i;
            lastRowClickTime_ = in.time;
        }
        rowY += kRowH + 2.0f;
    }

    bool haveSel = selectedWorld_ >= 0 && selectedWorld_ < static_cast<int>(worlds_.size());
    float x = (w - kBtnW) * 0.5f;
    float y = buttonsY;
    if (button(ui, font, x, y, kBtnW, kBtnH, tr("selectWorld.select", "Play Selected World"),
               in, haveSel) &&
        haveSel) {
        actionFolder_ = worlds_[static_cast<size_t>(selectedWorld_)].folder;
        a = Action::PlayWorld;
    }
    y += kBtnH + kGap;
    if (button(ui, font, x, y, kBtnW, kBtnH, tr("selectWorld.create", "Create New World"), in)) {
        nameField_ = tr("selectWorld.newWorld", "New World");
        seedField_.clear();
        createSurvival_ = false;
        focusField_ = -1;
        page_ = Page::CreateWorld;
    }
    y += kBtnH + kGap;
    if (button(ui, font, x, y, kHalfW, kBtnH, tr("selectWorld.delete", "Delete"), in, haveSel) &&
        haveSel) {
        actionFolder_ = worlds_[static_cast<size_t>(selectedWorld_)].folder;
        page_ = Page::ConfirmDelete;
    }
    if (button(ui, font, x + kHalfW + kGap, y, kHalfW, kBtnH, tr("gui.cancel", "Cancel"), in)) {
        page_ = Page::Title;
    }
    return a;
}

MenuScreen::Action MenuScreen::buildCreate(UIRenderer& ui, const Font& font, uint32_t sw,
                                           uint32_t sh, const Input& in) {
    float w = static_cast<float>(sw), h = static_cast<float>(sh);
    ui.quad(0.0f, 0.0f, w, h, glm::vec4(0.0f, 0.0f, 0.0f, 0.60f));

    const std::string head = tr("selectWorld.create", "Create New World");
    float hs = kScale * 1.5f;
    font.drawText(ui, head, (w - font.textWidth(head, hs)) * 0.5f, h * 0.10f, hs, kWhite);

    Action a = Action::None;
    float x = (w - kBtnW) * 0.5f;
    float y = h * 0.24f;
    font.drawText(ui, tr("selectWorld.enterName", "World Name"), x,
                  y - font.lineHeight(kScale) - 2.0f, kScale, kGray);
    textField(ui, font, 0, x, y, kBtnW, kBtnH, nameField_, in, 32);
    y += kBtnH + kGap * 3.0f;

    font.drawText(ui, tr("selectWorld.enterSeed", "Seed (leave blank for random)"), x,
                  y - font.lineHeight(kScale) - 2.0f, kScale, kGray);
    textField(ui, font, 1, x, y, kBtnW, kBtnH, seedField_, in, 19);
    y += kBtnH + kGap * 3.0f;

    std::string modeLabel = tr("selectWorld.gameMode", "Game Mode") + std::string(": ") +
                            (createSurvival_ ? tr("gameMode.survival", "Survival")
                                             : tr("gameMode.creative", "Creative"));
    if (button(ui, font, x, y, kBtnW, kBtnH, modeLabel, in)) createSurvival_ = !createSurvival_;
    y += kBtnH + kGap * 4.0f;

    if (button(ui, font, x, y, kBtnW, kBtnH, tr("selectWorld.create", "Create New World"), in)) {
        a = Action::CreateWorld;
    }
    y += kBtnH + kGap;
    if (button(ui, font, x, y, kBtnW, kBtnH, tr("gui.cancel", "Cancel"), in)) {
        page_ = Page::Worlds;
    }
    return a;
}

MenuScreen::Action MenuScreen::buildConfirmDelete(UIRenderer& ui, const Font& font, uint32_t sw,
                                                  uint32_t sh, const Input& in) {
    float w = static_cast<float>(sw), h = static_cast<float>(sh);
    ui.quad(0.0f, 0.0f, w, h, glm::vec4(0.0f, 0.0f, 0.0f, 0.60f));

    std::string name = actionFolder_;
    for (const WorldEntry& e : worlds_) {
        if (e.folder == actionFolder_) { name = e.name; break; }
    }
    std::string q = tr("selectWorld.deleteQuestion", "Delete") + (" '" + name + "'?");
    font.drawText(ui, q, (w - font.textWidth(q, kScale * 1.5f)) * 0.5f, h * 0.30f,
                  kScale * 1.5f, kWhite);
    const std::string warn =
        tr("selectWorld.deleteWarning", "This world will be lost forever! (A long time!)");
    font.drawText(ui, warn, (w - font.textWidth(warn, kScale)) * 0.5f, h * 0.38f, kScale, kGray);

    Action a = Action::None;
    float x = (w - (2.0f * kHalfW + kGap)) * 0.5f;
    float y = h * 0.48f;
    if (button(ui, font, x, y, kHalfW, kBtnH, tr("selectWorld.deleteButton", "Delete"), in)) {
        a = Action::DeleteWorld;
        page_ = Page::Worlds;
    }
    if (button(ui, font, x + kHalfW + kGap, y, kHalfW, kBtnH, tr("gui.cancel", "Cancel"), in)) {
        page_ = Page::Worlds;
    }
    return a;
}

MenuScreen::Action MenuScreen::buildPause(UIRenderer& ui, const Font& font, uint32_t sw,
                                          uint32_t sh, const Input& in) {
    float w = static_cast<float>(sw), h = static_cast<float>(sh);
    ui.quad(0.0f, 0.0f, w, h, glm::vec4(0.0f, 0.0f, 0.0f, 0.45f));

    Action a = Action::None;
    // Vanilla layout: Back to Game (full) / Advancements|Statistics / four icon squares /
    // Options|Multiplayer / Save and Quit to Title (full). Centered as a block with the
    // title above it.
    float row = kBtnH + kGap;
    float blockH = 4.0f * kBtnH + kSquare + 4.0f * kGap;
    float headH = font.lineHeight(kScale) + kGap * 2.0f;
    float y = std::floor((h - blockH - headH) * 0.5f);

    const std::string head = tr("menu.game", "Game Menu");
    font.drawText(ui, head, (w - font.textWidth(head, kScale)) * 0.5f, y, kScale, kWhite);
    y += headH;

    float x = (w - kBtnW) * 0.5f;                       // full-width buttons
    float xh = (w - (2.0f * kHalfW + kGap)) * 0.5f;     // half-width pair
    float xh2 = xh + kHalfW + kGap;

    if (button(ui, font, x, y, kBtnW, kBtnH, tr("menu.returnToGame", "Back to Game"), in)) {
        a = Action::Resume;
    }
    y += row;

    // Advancements / Statistics — both locked for now.
    button(ui, font, xh, y, kHalfW, kBtnH, tr("gui.advancements", "Advancements"), in, false);
    button(ui, font, xh2, y, kHalfW, kBtnH, tr("gui.stats", "Statistics"), in, false);
    y += row;

    // Icon row: bug tracker / feedback / friends / social interactions (all decorative
    // until their screens exist; centered as a group).
    float iconRowW = 4.0f * kSquare + 3.0f * kGap;
    float ix = (w - iconRowW) * 0.5f;
    iconButton(ui, ix, y, tex_.iconBug, in);
    iconButton(ui, ix + (kSquare + kGap), y, tex_.iconFeedback, in);
    iconButton(ui, ix + 2.0f * (kSquare + kGap), y, tex_.iconFriends, in);
    iconButton(ui, ix + 3.0f * (kSquare + kGap), y, tex_.iconSocial, in);
    y += kSquare + kGap;

    // Options (works) / Multiplayer (locked).
    if (button(ui, font, xh, y, kHalfW, kBtnH, tr("menu.options", "Options..."), in)) {
        returnPage_ = Page::Pause;
        page_ = Page::Options;
    }
    button(ui, font, xh2, y, kHalfW, kBtnH, tr("menu.shareToLan", "Multiplayer..."), in, false);
    y += row;

    if (button(ui, font, x, y, kBtnW, kBtnH, tr("menu.returnToMenu", "Save and Quit to Title"),
               in)) {
        a = Action::SaveQuit;
    }
    return a;
}

// ---- Options-menu widget helpers ------------------------------------------------------

void MenuScreen::optCycle(UIRenderer& ui, const Font& font, float x, float y, float w, float h,
                          const char* labelKey, const char* labelEng,
                          std::initializer_list<const char*> values, Settings& s,
                          const char* key, const Input& in, bool enabled) {
    int n = static_cast<int>(values.size());
    int idx = std::clamp(static_cast<int>(std::lround(s.optGet(key, 0.0f))), 0, std::max(0, n - 1));
    std::string txt = tr(labelKey, labelEng) + ": " + *(values.begin() + idx);
    if (button(ui, font, x, y, w, h, txt, in, enabled) && enabled) {
        s.optSet(key, static_cast<float>((idx + 1) % n));
    }
}

void MenuScreen::optPercent(UIRenderer& ui, const Font& font, int id, float x, float y, float w,
                            float h, const char* labelKey, const char* labelEng, Settings& s,
                            const char* key, float def, const Input& in) {
    float v = std::clamp(s.optGet(key, def), 0.0f, 1.0f);
    char lbl[96];
    std::snprintf(lbl, sizeof(lbl), "%s: %d%%", tr(labelKey, labelEng).c_str(),
                  static_cast<int>(std::lround(v * 100.0f)));
    s.optSet(key, slider(ui, font, id, x, y, w, h, lbl, v, in));
}

void MenuScreen::subButton(UIRenderer& ui, const Font& font, float x, float y, float w, float h,
                           const char* labelKey, const char* labelEng, Page target,
                           const Input& in, bool enabled) {
    if (button(ui, font, x, y, w, h, tr(labelKey, labelEng), in, enabled) && enabled) {
        optScroll_ = 0;
        page_ = target;
    }
}

float MenuScreen::subHeader(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                            const char* titleKey, const char* titleEng) {
    float w = static_cast<float>(sw), h = static_cast<float>(sh);
    ui.quad(0.0f, 0.0f, w, h, glm::vec4(0.0f, 0.0f, 0.0f, 0.55f));
    std::string head = tr(titleKey, titleEng);
    float hs = kScale * 1.5f;
    font.drawText(ui, head, (w - font.textWidth(head, hs)) * 0.5f, h * 0.05f, hs, kWhite);
    return h * 0.15f;
}

bool MenuScreen::doneButton(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                            const Input& in, Page parent) {
    float w = static_cast<float>(sw), h = static_cast<float>(sh);
    float x = (w - kBtnW) * 0.5f;
    float y = h - kBtnH - 6.0f * kScale;
    if (button(ui, font, x, y, kBtnW, kBtnH, tr("gui.done", "Done"), in)) {
        page_ = parent;
        return true;
    }
    return false;
}

MenuScreen::Action MenuScreen::buildOptions(UIRenderer& ui, const Font& font, uint32_t sw,
                                            uint32_t sh, const Input& in, Settings& s) {
    float w = static_cast<float>(sw), h = static_cast<float>(sh);
    ui.quad(0.0f, 0.0f, w, h, glm::vec4(0.0f, 0.0f, 0.0f, 0.55f));

    const std::string head = tr("options.title", "Options");
    float hs = kScale * 1.5f;
    font.drawText(ui, head, (w - font.textWidth(head, hs)) * 0.5f, h * 0.05f, hs, kWhite);

    const float kHalfW = kOptW; // wider columns so "Skin Customization..." etc. fit
    float xh = (w - (2.0f * kHalfW + kGap)) * 0.5f, xh2 = xh + kHalfW + kGap;
    float row = kBtnH + kGap;
    float y = h * 0.14f;
    char label[96];

    // Top row: FOV slider (left), Online (right, locked).
    std::snprintf(label, sizeof(label), "%s: %d", tr("options.fov", "FOV").c_str(),
                  static_cast<int>(std::lround(s.fovDeg)));
    float t = slider(ui, font, 0, xh, y, kHalfW, kBtnH, label, (s.fovDeg - 30.0f) / 80.0f, in);
    s.fovDeg = 30.0f + std::round(t * 80.0f);
    button(ui, font, xh2, y, kHalfW, kBtnH, tr("options.online", "Online..."), in, false);
    y += row * 2.0f;

    // Sub-menu grid (2 columns), matching vanilla.
    subButton(ui, font, xh, y, kHalfW, kBtnH, "options.skinCustomisation.title",
              "Skin Customization...", Page::SkinCustom, in);
    subButton(ui, font, xh2, y, kHalfW, kBtnH, "options.sounds", "Music & Sounds...",
              Page::MusicSounds, in);
    y += row;
    subButton(ui, font, xh, y, kHalfW, kBtnH, "options.video", "Video Settings...",
              Page::VideoSettings, in);
    subButton(ui, font, xh2, y, kHalfW, kBtnH, "options.controls", "Controls...",
              Page::Controls, in);
    y += row;
    if (button(ui, font, xh, y, kHalfW, kBtnH, tr("options.language", "Language..."), in)) {
        langReturn_ = Page::Options;
        return Action::OpenLanguages;
    }
    subButton(ui, font, xh2, y, kHalfW, kBtnH, "options.chat.title", "Chat Settings...",
              Page::ChatSettings, in);
    y += row;
    subButton(ui, font, xh, y, kHalfW, kBtnH, "resourcePack.title", "Resource Packs...",
              Page::ResourcePacks, in);
    subButton(ui, font, xh2, y, kHalfW, kBtnH, "options.accessibility.title",
              "Accessibility Settings...", Page::Accessibility, in);

    if (doneButton(ui, font, sw, sh, in, returnPage_)) {
        focusField_ = -1;
        return Action::Done;
    }
    return Action::None;
}

void MenuScreen::buildSkinCustom(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                                 const Input& in, Settings& s) {
    float w = static_cast<float>(sw);
    float y = subHeader(ui, font, sw, sh, "options.skinCustomisation.title", "Skin Customization");
    const float kHalfW = kOptW; // wider columns so option labels fit
    float xh = (w - (2.0f * kHalfW + kGap)) * 0.5f, xh2 = xh + kHalfW + kGap, row = kBtnH + kGap;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.modelPart.cape", "Cape", {"ON", "OFF"}, s, "skinCape", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.modelPart.jacket", "Jacket", {"ON", "OFF"}, s, "skinJacket", in);
    y += row;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.modelPart.left_sleeve", "Left Sleeve", {"ON", "OFF"}, s, "skinLSleeve", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.modelPart.right_sleeve", "Right Sleeve", {"ON", "OFF"}, s, "skinRSleeve", in);
    y += row;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.modelPart.left_pants_leg", "Left Pants Leg", {"ON", "OFF"}, s, "skinLPants", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.modelPart.right_pants_leg", "Right Pants Leg", {"ON", "OFF"}, s, "skinRPants", in);
    y += row;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.modelPart.hat", "Hat", {"ON", "OFF"}, s, "skinHat", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.mainHand", "Main Hand", {"Right", "Left"}, s, "mainHand", in);
    doneButton(ui, font, sw, sh, in, Page::Options);
}

void MenuScreen::buildMusicSounds(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                                  const Input& in, Settings& s) {
    float w = static_cast<float>(sw);
    float y = subHeader(ui, font, sw, sh, "options.sounds", "Music & Sound Options");
    const float kHalfW = kOptW;
    float x = (w - kBtnW) * 0.5f;
    float xh = (w - (2.0f * kHalfW + kGap)) * 0.5f, xh2 = xh + kHalfW + kGap, row = kBtnH + kGap;
    optPercent(ui, font, 10, x, y, kBtnW, kBtnH, "soundCategory.master", "Master Volume", s, "volMaster", 1.0f, in);
    y += row;
    optPercent(ui, font, 11, xh, y, kHalfW, kBtnH, "soundCategory.music", "Music", s, "volMusic", 1.0f, in);
    optPercent(ui, font, 12, xh2, y, kHalfW, kBtnH, "soundCategory.record", "Jukebox/Note Blocks", s, "volRecord", 1.0f, in);
    y += row;
    optPercent(ui, font, 13, xh, y, kHalfW, kBtnH, "soundCategory.weather", "Weather", s, "volWeather", 1.0f, in);
    optPercent(ui, font, 14, xh2, y, kHalfW, kBtnH, "soundCategory.blocks", "Blocks", s, "volBlocks", 1.0f, in);
    y += row;
    optPercent(ui, font, 15, xh, y, kHalfW, kBtnH, "soundCategory.hostile", "Hostile Creatures", s, "volHostile", 1.0f, in);
    optPercent(ui, font, 16, xh2, y, kHalfW, kBtnH, "soundCategory.neutral", "Friendly Creatures", s, "volFriendly", 1.0f, in);
    y += row;
    optPercent(ui, font, 17, xh, y, kHalfW, kBtnH, "soundCategory.player", "Players", s, "volPlayers", 1.0f, in);
    optPercent(ui, font, 18, xh2, y, kHalfW, kBtnH, "soundCategory.ambient", "Ambient/Environment", s, "volAmbient", 1.0f, in);
    y += row;
    optPercent(ui, font, 19, xh, y, kHalfW, kBtnH, "soundCategory.voice", "Voice/Speech", s, "volVoice", 1.0f, in);
    y += row;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.showSubtitles", "Show Subtitles", {"OFF", "ON"}, s, "subtitles", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.directionalAudio", "Directional Audio", {"OFF", "ON"}, s, "directionalAudio", in);
    doneButton(ui, font, sw, sh, in, Page::Options);
}

void MenuScreen::buildVideo(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                            const Input& in, Settings& s) {
    float w = static_cast<float>(sw);
    float y = subHeader(ui, font, sw, sh, "options.video", "Video Settings");
    const float kHalfW = kOptW;
    float xh = (w - (2.0f * kHalfW + kGap)) * 0.5f, xh2 = xh + kHalfW + kGap, row = kBtnH + kGap;
    char label[96];

    // Render Distance (real: 2..12) and Simulation Distance (cosmetic mirror).
    std::snprintf(label, sizeof(label), "%s: %d", tr("options.renderDistance", "Render Distance").c_str(),
                  s.renderDistance);
    float t = slider(ui, font, 20, xh, y, kHalfW, kBtnH, label,
                     static_cast<float>(s.renderDistance - 2) / 62.0f, in);
    s.renderDistance = 2 + static_cast<int>(std::lround(t * 62.0f));
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.graphics", "Graphics", {"Fancy", "Fast"}, s, "graphics", in);
    y += row;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.ao", "Smooth Lighting", {"Maximum", "Minimum", "OFF"}, s, "smoothLight", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.viewBobbing", "View Bobbing", {"ON", "OFF"}, s, "viewBobbing", in);
    y += row;
    // Fullscreen (real: caller applies s.fullscreen to the window live).
    char fsLabel[96];
    std::snprintf(fsLabel, sizeof(fsLabel), "%s: %s", tr("options.fullscreen", "Fullscreen").c_str(),
                  s.fullscreen ? tr("options.on", "ON").c_str() : tr("options.off", "OFF").c_str());
    if (button(ui, font, xh, y, kHalfW, kBtnH, fsLabel, in)) s.fullscreen = !s.fullscreen;
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.vsync", "VSync", {"ON", "OFF"}, s, "vsync", in);
    y += row;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.guiScale", "GUI Scale", {"Auto", "1", "2", "3", "4"}, s, "guiScale", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.clouds", "Clouds", {"Fancy", "Fast", "OFF"}, s, "clouds", in);
    y += row;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.particles", "Particles", {"All", "Decreased", "Minimal"}, s, "particles", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.entityShadows", "Entity Shadows", {"ON", "OFF"}, s, "entityShadows", in);
    y += row;
    // LOD Distance (real: 0 = off, else chunks of far "distant horizons" terrain).
    if (s.lodDistance <= 0) {
        std::snprintf(label, sizeof(label), "%s: %s", tr("options.lodDistance", "LOD Distance").c_str(),
                      tr("options.off", "OFF").c_str());
    } else {
        std::snprintf(label, sizeof(label), "%s: %d", tr("options.lodDistance", "LOD Distance").c_str(),
                      s.lodDistance);
    }
    float tl = slider(ui, font, 21, xh, y, kHalfW, kBtnH, label,
                      static_cast<float>(s.lodDistance) / 64.0f, in);
    s.lodDistance = static_cast<int>(std::lround(tl * 64.0f));
    doneButton(ui, font, sw, sh, in, Page::Options);
}

void MenuScreen::buildControls(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                               const Input& in, Settings& s) {
    float w = static_cast<float>(sw);
    float y = subHeader(ui, font, sw, sh, "controls.title", "Controls");
    const float kHalfW = kOptW;
    float xh = (w - (2.0f * kHalfW + kGap)) * 0.5f, xh2 = xh + kHalfW + kGap, row = kBtnH + kGap;
    subButton(ui, font, xh, y, kHalfW, kBtnH, "options.mouse_settings", "Mouse Settings...", Page::MouseSettings, in);
    subButton(ui, font, xh2, y, kHalfW, kBtnH, "controls.keybinds", "Key Binds...", Page::KeyBinds, in);
    y += row;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "key.sneak", "Sneak", {"Hold", "Toggle"}, s, "sneakMode", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "key.sprint", "Sprint", {"Hold", "Toggle"}, s, "sprintMode", in);
    y += row;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.autoJump", "Auto-Jump", {"OFF", "ON"}, s, "autoJump", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.operatorItemsTab", "Operator Items Tab", {"ON", "OFF"}, s, "opItemsTab", in);
    doneButton(ui, font, sw, sh, in, Page::Options);
}

void MenuScreen::buildMouse(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                            const Input& in, Settings& s) {
    float w = static_cast<float>(sw);
    float y = subHeader(ui, font, sw, sh, "options.mouse_settings.title", "Mouse Settings");
    const float kHalfW = kOptW;
    float xh = (w - (2.0f * kHalfW + kGap)) * 0.5f, xh2 = xh + kHalfW + kGap, row = kBtnH + kGap;
    char label[96];
    // Sensitivity (real: 10..300%).
    int percent = static_cast<int>(std::lround(s.sensitivity * 1250.0f));
    std::snprintf(label, sizeof(label), "%s: %d%%", tr("options.sensitivity", "Sensitivity").c_str(), percent);
    float t = slider(ui, font, 30, xh, y, kHalfW, kBtnH, label,
                     std::clamp((static_cast<float>(percent) - 10.0f) / 290.0f, 0.0f, 1.0f), in);
    s.sensitivity = (10.0f + std::round(t * 290.0f)) / 1250.0f;
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.invertMouse", "Invert Mouse", {"OFF", "ON"}, s, "invertMouse", in);
    y += row;
    optPercent(ui, font, 31, xh, y, kHalfW, kBtnH, "options.mouseWheelSensitivity", "Scroll Sensitivity", s, "scrollSens", 0.25f, in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.discrete_mouse_scroll", "Discrete Scrolling", {"OFF", "ON"}, s, "discreteScroll", in);
    y += row;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.rawMouseInput", "Raw input", {"ON", "OFF"}, s, "rawInput", in);
    doneButton(ui, font, sw, sh, in, Page::Controls);
}

void MenuScreen::buildKeyBinds(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                               const Input& in) {
    float w = static_cast<float>(sw), h = static_cast<float>(sh);
    float top = subHeader(ui, font, sw, sh, "controls.keybinds", "Key Binds");
    if (!binds_) {
        doneButton(ui, font, sw, sh, in, Page::Controls);
        return;
    }
    std::vector<KeyBinds::Bind>& list = binds_->binds();

    // A pending rebind grabs the next key press (Escape, handled by Window, keeps the
    // current key by never arriving here).
    if (rebinding_ >= 0 && rebinding_ < static_cast<int>(list.size())) {
        if (in.keyPressed != 0) {
            list[rebinding_].key = in.keyPressed;
            keyBindsDirty_ = true;
            rebinding_ = -1;
        } else if (in.leftPressed) { // clicking elsewhere cancels
            rebinding_ = -1;
        }
    }

    float nameX = w * 0.24f;
    float keyW = 120.0f * kScale, resetW = 60.0f * kScale;
    float keyX = w * 0.52f, resetX = keyX + keyW + kGap;
    float bottomY = h - kBtnH - 6.0f * kScale;
    float rowH = kBtnH + kGap * 0.5f;

    // Flatten into rows: a category header before each new category, then its binds.
    struct Row { bool header; int bind; std::string text; };
    std::vector<Row> rows;
    std::string cat;
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        if (list[static_cast<size_t>(i)].category != cat) {
            cat = list[static_cast<size_t>(i)].category;
            rows.push_back({true, -1, cat});
        }
        rows.push_back({false, i, {}});
    }
    int total = static_cast<int>(rows.size());
    int visible = std::max(1, static_cast<int>((bottomY - top - kGap) / rowH));
    if (in.scroll != 0.0) optScroll_ -= static_cast<int>(in.scroll);
    optScroll_ = std::clamp(optScroll_, 0, std::max(0, total - visible));

    float y = top;
    for (int r = optScroll_; r < total && y + kBtnH <= bottomY - kGap; ++r) {
        const Row& row = rows[static_cast<size_t>(r)];
        if (row.header) {
            font.drawText(ui, row.text, (w - font.textWidth(row.text, kScale)) * 0.5f,
                          y + 6.0f * kScale, kScale, glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
            y += rowH;
            continue;
        }
        int i = row.bind;
        KeyBinds::Bind& b = list[static_cast<size_t>(i)];
        font.drawText(ui, b.name, nameX, y + 6.0f * kScale, kScale, kWhite);
        bool waiting = rebinding_ == i;
        bool hoverKey = inside(in, keyX, y, keyW, kBtnH);
        std::string klabel = waiting ? "> ? <" : KeyBinds::keyName(b.key);
        drawWidget(ui, b.rebindable ? (hoverKey ? tex_.buttonHighlighted : tex_.button)
                                    : tex_.buttonDisabled,
                   keyX, y, keyW, kBtnH, b.rebindable ? kFillNormal : kFillDisabled);
        font.drawText(ui, klabel, keyX + (keyW - font.textWidth(klabel, kScale)) * 0.5f,
                      y + (kBtnH - font.lineHeight(kScale)) * 0.5f, kScale,
                      waiting ? glm::vec4(1.0f, 1.0f, 0.4f, 1.0f) : kWhite);
        if (b.rebindable && hoverKey && in.leftPressed) rebinding_ = i;
        bool changed = b.key != b.def && b.rebindable;
        if (button(ui, font, resetX, y, resetW, kBtnH, tr("controls.reset", "Reset"), in, changed) &&
            changed) {
            b.key = b.def;
            keyBindsDirty_ = true;
        }
        y += rowH;
    }

    // Bottom row: Reset Keys (all) | Done.
    float bx = (w - (2.0f * kHalfW + kGap)) * 0.5f;
    if (button(ui, font, bx, bottomY, kHalfW, kBtnH, tr("controls.resetAll", "Reset Keys"), in)) {
        binds_->resetAll();
        keyBindsDirty_ = true;
    }
    if (button(ui, font, bx + kHalfW + kGap, bottomY, kHalfW, kBtnH, tr("gui.done", "Done"), in)) {
        rebinding_ = -1;
        page_ = Page::Controls;
    }
}

void MenuScreen::buildChat(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                           const Input& in, Settings& s) {
    float w = static_cast<float>(sw);
    float y = subHeader(ui, font, sw, sh, "options.chat.title", "Chat Settings");
    const float kHalfW = kOptW;
    float xh = (w - (2.0f * kHalfW + kGap)) * 0.5f, xh2 = xh + kHalfW + kGap, row = kBtnH + kGap;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.chat.visibility", "Chat", {"Shown", "Commands Only", "Hidden"}, s, "chatVis", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.chat.color", "Colors", {"ON", "OFF"}, s, "chatColors", in);
    y += row;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.chat.links", "Web Links", {"ON", "OFF"}, s, "chatLinks", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.chat.links.prompt", "Prompt on Links", {"ON", "OFF"}, s, "chatPrompt", in);
    y += row;
    optPercent(ui, font, 40, xh, y, kHalfW, kBtnH, "options.chat.opacity", "Chat Text Opacity", s, "chatOpacity", 1.0f, in);
    optPercent(ui, font, 41, xh2, y, kHalfW, kBtnH, "options.accessibility.text_background_opacity", "Text Background Opacity", s, "chatBgOpacity", 0.5f, in);
    y += row;
    optPercent(ui, font, 42, xh, y, kHalfW, kBtnH, "options.chat.scale", "Chat Text Size", s, "chatScale", 1.0f, in);
    optPercent(ui, font, 43, xh2, y, kHalfW, kBtnH, "options.chat.line_spacing", "Line Spacing", s, "chatLineSpace", 0.0f, in);
    y += row;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.narrator", "Narrator", {"OFF", "ON"}, s, "narrator", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.autoSuggestCommands", "Command Suggestions", {"ON", "OFF"}, s, "cmdSuggest", in);
    doneButton(ui, font, sw, sh, in, Page::Options);
}

void MenuScreen::buildAccessibility(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                                    const Input& in, Settings& s) {
    float w = static_cast<float>(sw);
    float y = subHeader(ui, font, sw, sh, "options.accessibility.title", "Accessibility Settings");
    const float kHalfW = kOptW;
    float xh = (w - (2.0f * kHalfW + kGap)) * 0.5f, xh2 = xh + kHalfW + kGap, row = kBtnH + kGap;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.narrator", "Narrator", {"OFF", "ON"}, s, "narrator", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.showSubtitles", "Show Subtitles", {"OFF", "ON"}, s, "subtitles", in);
    y += row;
    optCycle(ui, font, xh, y, kHalfW, kBtnH, "options.accessibility.high_contrast", "High Contrast", {"OFF", "ON"}, s, "highContrast", in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.autoJump", "Auto-Jump", {"OFF", "ON"}, s, "autoJump", in);
    y += row;
    optPercent(ui, font, 50, xh, y, kHalfW, kBtnH, "options.accessibility.text_background_opacity", "Text Background Opacity", s, "chatBgOpacity", 0.5f, in);
    optCycle(ui, font, xh2, y, kHalfW, kBtnH, "options.accessibility.text_background", "Text Background", {"Chat", "Everywhere"}, s, "textBg", in);
    y += row;
    optPercent(ui, font, 51, xh, y, kHalfW, kBtnH, "options.chat.opacity", "Chat Text Opacity", s, "chatOpacity", 1.0f, in);
    optPercent(ui, font, 52, xh2, y, kHalfW, kBtnH, "options.chat.line_spacing", "Line Spacing", s, "chatLineSpace", 0.0f, in);
    y += row;
    optPercent(ui, font, 53, xh, y, kHalfW, kBtnH, "options.accessibility.panorama_speed", "Panorama Scroll Speed", s, "panoSpeed", 1.0f, in);
    optPercent(ui, font, 54, xh2, y, kHalfW, kBtnH, "options.notifications.display_time", "Notification Time", s, "notifTime", 1.0f, in);
    y += row;
    optPercent(ui, font, 55, xh, y, kHalfW, kBtnH, "options.darknessEffectScale", "Darkness Pulsing", s, "darkPulse", 1.0f, in);
    optPercent(ui, font, 56, xh2, y, kHalfW, kBtnH, "options.damageTiltStrength", "Damage Tilt", s, "damageTilt", 1.0f, in);
    doneButton(ui, font, sw, sh, in, Page::Options);
}

void MenuScreen::buildResourcePacks(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                                    const Input& in) {
    float w = static_cast<float>(sw), h = static_cast<float>(sh);
    subHeader(ui, font, sw, sh, "resourcePack.title", "Resource Packs");
    const std::string note = tr("resourcePack.folderInfo", "Drop textures into assets/ - packs coming later");
    font.drawText(ui, note, (w - font.textWidth(note, kScale)) * 0.5f, h * 0.45f, kScale, kGray);
    doneButton(ui, font, sw, sh, in, Page::Options);
}

MenuScreen::Action MenuScreen::buildLanguage(UIRenderer& ui, const Font& font, uint32_t sw,
                                             uint32_t sh, const Input& in, const Settings& s) {
    float w = static_cast<float>(sw), h = static_cast<float>(sh);
    ui.quad(0.0f, 0.0f, w, h, glm::vec4(0.0f, 0.0f, 0.0f, 0.60f));

    const std::string head = tr("options.language.title", "Language");
    float hs = kScale * 1.5f;
    font.drawText(ui, head, (w - font.textWidth(head, hs)) * 0.5f, h * 0.08f, hs, kWhite);

    Action a = Action::None;
    float x = (w - kBtnW) * 0.5f;
    float listY = h * 0.20f;
    float doneY = h - kBtnH - 6.0f * kScale;
    int visible = std::max(1, static_cast<int>((doneY - listY - kGap) / (kBtnH + kGap)));

    if (in.scroll != 0.0) langScroll_ -= static_cast<int>(in.scroll);
    int maxScroll = std::max(0, static_cast<int>(langs_.size()) - visible);
    langScroll_ = std::clamp(langScroll_, 0, maxScroll);

    if (langs_.empty()) {
        const std::string none =
            tr("options.language.empty", "No language files (assets/lang/*.json)");
        font.drawText(ui, none, (w - font.textWidth(none, kScale)) * 0.5f,
                      listY + kBtnH, kScale, kGray);
    }
    float y = listY;
    for (int i = langScroll_; i < static_cast<int>(langs_.size()); ++i) {
        if (y + kBtnH > doneY - kGap) break;
        const LangEntry& e = langs_[static_cast<size_t>(i)];
        bool current = e.code == s.language;
        std::string label = current ? "> " + e.name + " <" : e.name;
        if (button(ui, font, x, y, kBtnW, kBtnH, label, in) && !current) {
            actionLanguage_ = e.code;
            a = Action::SetLanguage;
        }
        y += kBtnH + kGap;
    }

    if (button(ui, font, x, doneY, kBtnW, kBtnH, tr("gui.done", "Done"), in)) {
        page_ = langReturn_;
    }
    return a;
}

} // namespace mc
