#pragma once

#include <string>
#include <vector>

namespace mc {

// Rebindable key bindings. Each action has a stable id, a category + display name for the
// Key Binds screen, and a GLFW key code (mouse buttons stored as negative: -1 LMB, -2 RMB,
// -3 MMB). Rebindable actions can be changed in the menu; mouse actions are fixed for now.
// Persisted to keybinds.txt as "id:code" lines. Gameplay code queries key("forward") etc.
class KeyBinds {
public:
    struct Bind {
        std::string id;
        std::string category;
        std::string name;
        int def = 0;            // default GLFW key
        int key = 0;            // current
        bool rebindable = true; // false = fixed mouse button (display only)
    };

    static constexpr int kMouseLeft = -1, kMouseRight = -2, kMouseMiddle = -3;

    KeyBinds() { resetAll(); }

    void resetAll();                      // restore every bind to its default
    void load(const std::string& path);   // overlay saved codes onto the defaults
    void save(const std::string& path) const;

    // GLFW key code for an action (kMouse* for the fixed mouse binds); 0 if unknown.
    int key(const std::string& id) const;
    void setKey(const std::string& id, int code);
    void reset(const std::string& id);

    std::vector<Bind>& binds() { return binds_; }
    const std::vector<Bind>& binds() const { return binds_; }

    // Human-readable label for a key code ("Space", "Left Shift", "A", "Left Button").
    static std::string keyName(int code);

private:
    std::vector<Bind> binds_;
    Bind* find(const std::string& id);
    const Bind* find(const std::string& id) const;
};

} // namespace mc
