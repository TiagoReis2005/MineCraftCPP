#pragma once

#include <string>

struct GLFWwindow;

namespace mc {

// Thin GLFW wrapper that owns the window. Created with no client API (Vulkan).
class Window {
public:
    void init(int width, int height, const std::string& title);
    void cleanup();

    bool shouldClose() const;
    void pollEvents() const;

    // Current framebuffer size in pixels (may differ from window size on HiDPI).
    void framebufferSize(int& width, int& height) const;

    // Blocks (pumping events) while the window is minimized to a 0-sized framebuffer,
    // then returns the valid size. Used before swapchain recreation.
    void waitForValidFramebuffer(int& width, int& height) const;

    // True once since the last call if the framebuffer was resized.
    bool consumeResized();

    // Accumulated mouse-wheel scroll since the last call (positive = scroll up).
    double consumeScroll();

    // Printable ASCII typed since the last call (menu text fields).
    std::string consumeText();

    // GLFW key/mouse code pressed since the last call (for key-bind rebinding); 0 = none.
    // Mouse buttons come back as KeyBinds' negative codes (-1 LMB, -2 RMB, -3 MMB).
    int consumeKeyPress();

    // Borderless fullscreen on the primary monitor; off restores the previous windowed
    // size/position. The swapchain recreates itself via the resize machinery.
    void setFullscreen(bool on);
    bool isFullscreen() const;
    // Windowed client size: live when windowed, the remembered one while fullscreen
    // (what options.txt persists).
    void windowedSize(int& w, int& h) const;

    GLFWwindow* handle() const { return window_; }

private:
    GLFWwindow* window_ = nullptr;
    bool resized_ = false;
    double scrollDelta_ = 0.0;
    std::string typed_;
    int lastKey_ = 0; // key/mouse code captured for rebinding
    int windowedX_ = 100, windowedY_ = 100; // restore point for leaving fullscreen
    int windowedW_ = 1280, windowedH_ = 720;

    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset);
    static void charCallback(GLFWwindow* window, unsigned int codepoint);
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
};

} // namespace mc
