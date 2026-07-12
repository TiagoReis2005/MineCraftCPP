#include "core/Window.h"

#include <GLFW/glfw3.h>

#include <stdexcept>

namespace mc {

void Window::init(int width, int height, const std::string& title) {
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialize GLFW");
    }
    if (!glfwVulkanSupported()) {
        glfwTerminate();
        throw std::runtime_error("GLFW reports Vulkan is not supported on this system");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // we manage Vulkan ourselves
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }
    windowedW_ = width;
    windowedH_ = height;
    glfwGetWindowPos(window_, &windowedX_, &windowedY_);

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
    glfwSetScrollCallback(window_, scrollCallback);
    glfwSetCharCallback(window_, charCallback);
    glfwSetKeyCallback(window_, keyCallback);
    glfwSetMouseButtonCallback(window_, mouseButtonCallback);
}

void Window::cleanup() {
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    glfwTerminate();
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(window_) != 0;
}

void Window::pollEvents() const {
    glfwPollEvents();
}

void Window::framebufferSize(int& width, int& height) const {
    glfwGetFramebufferSize(window_, &width, &height);
}

void Window::waitForValidFramebuffer(int& width, int& height) const {
    glfwGetFramebufferSize(window_, &width, &height);
    while (width == 0 || height == 0) {
        glfwWaitEvents();
        glfwGetFramebufferSize(window_, &width, &height);
    }
}

bool Window::consumeResized() {
    bool was = resized_;
    resized_ = false;
    return was;
}

double Window::consumeScroll() {
    double s = scrollDelta_;
    scrollDelta_ = 0.0;
    return s;
}

std::string Window::consumeText() {
    std::string t;
    t.swap(typed_);
    return t;
}

int Window::consumeKeyPress() {
    int k = lastKey_;
    lastKey_ = 0;
    return k;
}

void Window::setFullscreen(bool on) {
    if (on == isFullscreen()) return;
    if (on) {
        glfwGetWindowPos(window_, &windowedX_, &windowedY_);
        glfwGetWindowSize(window_, &windowedW_, &windowedH_);
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(window_, monitor, 0, 0, mode->width, mode->height,
                             mode->refreshRate);
    } else {
        glfwSetWindowMonitor(window_, nullptr, windowedX_, windowedY_, windowedW_,
                             windowedH_, 0);
    }
    resized_ = true; // swapchain recreates on the next frame
}

bool Window::isFullscreen() const {
    return glfwGetWindowMonitor(window_) != nullptr;
}

void Window::windowedSize(int& w, int& h) const {
    if (isFullscreen()) {
        w = windowedW_;
        h = windowedH_;
    } else {
        glfwGetWindowSize(window_, &w, &h);
    }
}

void Window::framebufferResizeCallback(GLFWwindow* window, int, int) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self) self->resized_ = true;
}

void Window::scrollCallback(GLFWwindow* window, double, double yoffset) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self) self->scrollDelta_ += yoffset;
}

void Window::charCallback(GLFWwindow* window, unsigned int codepoint) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    // Printable ASCII only (the bitmap font's range).
    if (self && codepoint >= 32 && codepoint < 127) {
        self->typed_ += static_cast<char>(codepoint);
    }
}

void Window::keyCallback(GLFWwindow* window, int key, int, int action, int) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    // Record the most recent key press for rebinding (Escape is reserved as cancel).
    if (self && action == GLFW_PRESS && key != GLFW_KEY_ESCAPE && key != GLFW_KEY_UNKNOWN) {
        self->lastKey_ = key;
    }
}

void Window::mouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    // Mouse buttons as KeyBinds' negative codes (-1 LMB, -2 RMB, -3 MMB).
    if (self && action == GLFW_PRESS && button >= 0 && button <= 2) {
        self->lastKey_ = -(button + 1);
    }
}

} // namespace mc
