#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace mc {

class UIRenderer;
class Font;
class Lang;
class KeyBinds;
struct Settings;

// Title, world-select, create-world, pause, and options screens. Widgets use the
// vanilla gui/sprites/widget/*.png art (3-sliced buttons, sliders, text fields) with
// flat-quad fallbacks when a sprite is missing. The title screen draws over the live
// world render; every other page dims it.
class MenuScreen {
public:
    enum class Page {
        None, Title, Worlds, CreateWorld, ConfirmDelete, Pause, Options, Language,
        SkinCustom, MusicSounds, VideoSettings, Controls, MouseSettings, KeyBinds,
        ChatSettings, Accessibility, ResourcePacks
    };
    enum class Action {
        None,
        OpenWorlds,    // title Singleplayer: caller scans saves/ and calls setWorlds
        PlayWorld,     // worlds list: load actionFolder()
        CreateWorld,   // create page: createName()/createSeedText()/createSurvival()
        DeleteWorld,   // confirm page: delete actionFolder(), then rescan
        Resume,        // pause: back to game
        Done,          // options closed: caller persists settings
        SaveQuit,      // pause: save the world and return to the title screen
        UploadSkin,    // title: open a file picker and install the chosen skin
        OpenLanguages, // caller scans assets/lang/ and calls setLanguages
        SetLanguage,   // language page: switch to actionLanguage() (live)
        Quit,          // quit the game
    };

    struct Textures {
        int button = 0, buttonHighlighted = 0, buttonDisabled = 0;
        int slider = 0, sliderHandle = 0, sliderHandleHighlighted = 0;
        int textField = 0, textFieldHighlighted = 0;
        int iconFriends = 0, iconLanguage = 0, iconAccessibility = 0;
        int iconBug = 0, iconFeedback = 0, iconSocial = 0; // pause-menu icon row
        int playerPreview = 0; // offscreen player-model render (square, transparent bg)
    };

    struct Input {
        float mouseX = 0.0f, mouseY = 0.0f; // framebuffer pixels
        bool leftPressed = false;           // edge
        bool leftDown = false;              // level (slider drag)
        std::string typed;                  // printable chars typed this frame
        bool backspace = false;             // edge/repeat for text fields
        int keyPressed = 0;                 // GLFW code pressed this frame (key rebinding)
        double scroll = 0.0;                // wheel notches (worlds list)
        double time = 0.0;                  // seconds (caret blink, double-click)
    };

    struct WorldEntry {
        std::string folder;   // saves/<folder>
        std::string name;     // display name
        std::string subtitle; // gray info line (mode, day, folder)
    };

    struct LangEntry {
        std::string code; // "en_us", "pt_pt"...
        std::string name; // native display name from the lang file
    };

    void init(const Textures& tex, const Lang* lang, KeyBinds* binds) {
        tex_ = tex;
        lang_ = lang;
        binds_ = binds;
    }
    // True if a key bind was changed since the last call (caller re-saves keybinds.txt).
    bool takeKeyBindsDirty() {
        bool d = keyBindsDirty_;
        keyBindsDirty_ = false;
        return d;
    }

    Page page() const { return page_; }
    void setPage(Page p) { page_ = p; }
    bool open() const { return page_ != Page::None; }

    // ESC step-back; the title page stays put.
    void back() {
        switch (page_) {
            case Page::Options:       page_ = returnPage_; break;
            case Page::Worlds:        page_ = Page::Title; break;
            case Page::CreateWorld:   page_ = Page::Worlds; break;
            case Page::ConfirmDelete: page_ = Page::Worlds; break;
            case Page::Language:      page_ = langReturn_; break;
            case Page::Pause:         page_ = Page::None; break;
            // Options sub-screens step back to Options; Mouse/KeyBinds to Controls.
            case Page::MouseSettings:
            case Page::KeyBinds:      page_ = Page::Controls; break;
            case Page::SkinCustom:
            case Page::MusicSounds:
            case Page::VideoSettings:
            case Page::Controls:
            case Page::ChatSettings:
            case Page::Accessibility:
            case Page::ResourcePacks: page_ = Page::Options; break;
            default: break;
        }
    }

    // The saves list shown on the Worlds page (most recent first).
    void setWorlds(std::vector<WorldEntry> worlds) {
        worlds_ = std::move(worlds);
        selectedWorld_ = worlds_.empty() ? -1 : 0;
        worldScroll_ = 0;
    }

    // Available languages for the Language page (from Lang::list).
    void setLanguages(std::vector<LangEntry> langs) {
        langs_ = std::move(langs);
        langScroll_ = 0;
    }

    // Draws the current page and handles its clicks. Sliders/fields mutate `settings`
    // live (the caller applies + persists). Returns what the user chose this frame.
    Action build(UIRenderer& ui, const Font& font, uint32_t screenW, uint32_t screenH,
                 const Input& in, Settings& settings);

    // Payloads for the returned action.
    const std::string& actionFolder() const { return actionFolder_; }
    const std::string& actionLanguage() const { return actionLanguage_; }
    std::string createName() const;
    const std::string& createSeedText() const { return seedField_; }
    bool createSurvival() const { return createSurvival_; }

    // True if the pointer was over any widget during the last build (the title screen
    // uses it to keep model-preview dragging off the buttons/fields).
    bool pointerOverWidget() const { return hoverAny_; }

private:
    Textures tex_{};
    const Lang* lang_ = nullptr; // vanilla lang keys; English fallbacks built in
    KeyBinds* binds_ = nullptr;  // rebindable keys (Key Binds screen)
    int rebinding_ = -1;         // bind index awaiting a key press (-1 = none)
    bool keyBindsDirty_ = false; // a bind changed; caller persists
    std::string tr(const char* key, const char* fallback) const;
    Page page_ = Page::Title; // the game boots into the title screen
    Page returnPage_ = Page::Title;
    int activeSlider_ = -1;
    int focusField_ = -1;
    bool hoverAny_ = false; // any widget hovered during the current build

    std::vector<WorldEntry> worlds_;
    int selectedWorld_ = -1;
    int worldScroll_ = 0; // first visible row
    int lastRowClicked_ = -1;
    double lastRowClickTime_ = -1.0; // double-click a row = play

    std::string nameField_ = "New World";
    std::string seedField_;
    bool createSurvival_ = false;
    std::string actionFolder_;
    std::vector<LangEntry> langs_;
    int langScroll_ = 0;
    Page langReturn_ = Page::Title; // where Done leaves the language page
    std::string actionLanguage_;

    int optScroll_ = 0; // scroll offset (rows) for long options sub-pages

    void drawWidget(UIRenderer& ui, int tex, float x, float y, float w, float h,
                    const float fallback[4]);
    bool button(UIRenderer& ui, const Font& font, float x, float y, float w, float h,
                const std::string& label, const Input& in, bool enabled = true);
    bool iconButton(UIRenderer& ui, float x, float y, int icon, const Input& in);
    float slider(UIRenderer& ui, const Font& font, int id, float x, float y, float w,
                 float h, const std::string& label, float t, const Input& in);
    void textField(UIRenderer& ui, const Font& font, int id, float x, float y, float w,
                   float h, std::string& value, const Input& in, size_t maxLen);

    // A button showing "Label: Value" that cycles the Settings.opt[key] index on click.
    void optCycle(UIRenderer& ui, const Font& font, float x, float y, float w, float h,
                  const char* labelKey, const char* labelEng,
                  std::initializer_list<const char*> values, Settings& s, const char* key,
                  const Input& in, bool enabled = true);
    // A percent slider (0..100%) bound to Settings.opt[key] (stored 0..1).
    void optPercent(UIRenderer& ui, const Font& font, int id, float x, float y, float w,
                    float h, const char* labelKey, const char* labelEng, Settings& s,
                    const char* key, float def, const Input& in);
    // A button that opens a sub-page.
    void subButton(UIRenderer& ui, const Font& font, float x, float y, float w, float h,
                   const char* labelKey, const char* labelEng, Page target, const Input& in,
                   bool enabled = true);

    Action buildTitle(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                      const Input& in, Settings& s);
    Action buildWorlds(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                       const Input& in);
    Action buildCreate(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                       const Input& in);
    Action buildConfirmDelete(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                              const Input& in);
    Action buildPause(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                      const Input& in);
    Action buildOptions(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                        const Input& in, Settings& s);
    Action buildLanguage(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                         const Input& in, const Settings& s);
    // Options sub-screens (Done returns to their parent; values persist in Settings.opt).
    void buildSkinCustom(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                         const Input& in, Settings& s);
    void buildMusicSounds(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                          const Input& in, Settings& s);
    void buildVideo(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                    const Input& in, Settings& s);
    void buildControls(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                       const Input& in, Settings& s);
    void buildMouse(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                    const Input& in, Settings& s);
    void buildKeyBinds(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                       const Input& in);
    void buildChat(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                   const Input& in, Settings& s);
    void buildAccessibility(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                            const Input& in, Settings& s);
    void buildResourcePacks(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                            const Input& in);
    // Shared chrome for a sub-page: dim + title + Done button (returns to `parent`).
    // Returns the y where content should begin.
    float subHeader(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh,
                    const char* titleKey, const char* titleEng);
    bool doneButton(UIRenderer& ui, const Font& font, uint32_t sw, uint32_t sh, const Input& in,
                    Page parent);
};

} // namespace mc
