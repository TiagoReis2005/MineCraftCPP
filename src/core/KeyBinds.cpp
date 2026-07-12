#include "core/KeyBinds.h"

#include <GLFW/glfw3.h>

#include <cstdio>

namespace mc {

void KeyBinds::resetAll() {
    // id, category, display name, default key, rebindable
    binds_ = {
        {"forward", "Movement", "Walk Forwards", GLFW_KEY_W, GLFW_KEY_W, true},
        {"back", "Movement", "Walk Backwards", GLFW_KEY_S, GLFW_KEY_S, true},
        {"left", "Movement", "Strafe Left", GLFW_KEY_A, GLFW_KEY_A, true},
        {"right", "Movement", "Strafe Right", GLFW_KEY_D, GLFW_KEY_D, true},
        {"jump", "Movement", "Jump", GLFW_KEY_SPACE, GLFW_KEY_SPACE, true},
        {"sneak", "Movement", "Sneak", GLFW_KEY_LEFT_SHIFT, GLFW_KEY_LEFT_SHIFT, true},
        {"sprint", "Movement", "Sprint", GLFW_KEY_LEFT_CONTROL, GLFW_KEY_LEFT_CONTROL, true},
        {"attack", "Gameplay", "Attack/Destroy", kMouseLeft, kMouseLeft, false},
        {"use", "Gameplay", "Use Item/Place Block", kMouseRight, kMouseRight, false},
        {"pick", "Gameplay", "Pick Block", kMouseMiddle, kMouseMiddle, false},
        {"drop", "Gameplay", "Drop Selected Item", GLFW_KEY_Q, GLFW_KEY_Q, true},
        {"inventory", "Inventory", "Open/Close Inventory", GLFW_KEY_E, GLFW_KEY_E, true},
        {"gamemode", "Gameplay", "Cycle Game Mode", GLFW_KEY_G, GLFW_KEY_G, true},
        {"perspective", "Misc", "Toggle Perspective", GLFW_KEY_F5, GLFW_KEY_F5, true},
        {"debug", "Misc", "Debug Info (F3)", GLFW_KEY_F3, GLFW_KEY_F3, true},
    };
}

KeyBinds::Bind* KeyBinds::find(const std::string& id) {
    for (Bind& b : binds_) {
        if (b.id == id) return &b;
    }
    return nullptr;
}

const KeyBinds::Bind* KeyBinds::find(const std::string& id) const {
    for (const Bind& b : binds_) {
        if (b.id == id) return &b;
    }
    return nullptr;
}

int KeyBinds::key(const std::string& id) const {
    const Bind* b = find(id);
    return b ? b->key : 0;
}

void KeyBinds::setKey(const std::string& id, int code) {
    if (Bind* b = find(id)) b->key = code;
}

void KeyBinds::reset(const std::string& id) {
    if (Bind* b = find(id)) b->key = b->def;
}

void KeyBinds::load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        char id[64];
        int code = 0;
        if (std::sscanf(line, "%63[^:]:%d", id, &code) == 2) {
            if (Bind* b = find(id); b && b->rebindable) b->key = code;
        }
    }
    std::fclose(f);
}

void KeyBinds::save(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    for (const Bind& b : binds_) {
        if (b.rebindable) std::fprintf(f, "%s:%d\n", b.id.c_str(), b.key);
    }
    std::fclose(f);
}

std::string KeyBinds::keyName(int code) {
    switch (code) {
        case kMouseLeft: return "Left Button";
        case kMouseRight: return "Right Button";
        case kMouseMiddle: return "Middle Button";
        case GLFW_KEY_SPACE: return "Space";
        case GLFW_KEY_LEFT_SHIFT: return "Left Shift";
        case GLFW_KEY_RIGHT_SHIFT: return "Right Shift";
        case GLFW_KEY_LEFT_CONTROL: return "Left Control";
        case GLFW_KEY_RIGHT_CONTROL: return "Right Control";
        case GLFW_KEY_LEFT_ALT: return "Left Alt";
        case GLFW_KEY_RIGHT_ALT: return "Right Alt";
        case GLFW_KEY_ENTER: return "Enter";
        case GLFW_KEY_TAB: return "Tab";
        case GLFW_KEY_BACKSPACE: return "Backspace";
        case GLFW_KEY_ESCAPE: return "Escape";
        case GLFW_KEY_UP: return "Up";
        case GLFW_KEY_DOWN: return "Down";
        case GLFW_KEY_LEFT: return "Left";
        case GLFW_KEY_RIGHT: return "Right";
        case GLFW_KEY_GRAVE_ACCENT: return "`";
        case GLFW_KEY_MINUS: return "-";
        case GLFW_KEY_EQUAL: return "=";
        case GLFW_KEY_COMMA: return ",";
        case GLFW_KEY_PERIOD: return ".";
        case GLFW_KEY_SLASH: return "/";
        case GLFW_KEY_SEMICOLON: return ";";
        case GLFW_KEY_APOSTROPHE: return "'";
        case GLFW_KEY_LEFT_BRACKET: return "[";
        case GLFW_KEY_RIGHT_BRACKET: return "]";
        default: break;
    }
    if (code >= GLFW_KEY_A && code <= GLFW_KEY_Z) return std::string(1, static_cast<char>('A' + (code - GLFW_KEY_A)));
    if (code >= GLFW_KEY_0 && code <= GLFW_KEY_9) return std::string(1, static_cast<char>('0' + (code - GLFW_KEY_0)));
    if (code >= GLFW_KEY_F1 && code <= GLFW_KEY_F25) return "F" + std::to_string(code - GLFW_KEY_F1 + 1);
    if (code >= GLFW_KEY_KP_0 && code <= GLFW_KEY_KP_9) return "Keypad " + std::to_string(code - GLFW_KEY_KP_0);
    return "Key " + std::to_string(code);
}

} // namespace mc
