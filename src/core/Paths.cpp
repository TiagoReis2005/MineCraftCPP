#include "core/Paths.h"

#include <filesystem>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace mc {

const std::string& exeDir() {
    static const std::string dir = [] {
#if defined(_WIN32)
        wchar_t buf[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::filesystem::path p(std::wstring(buf, len));
        return p.parent_path().string() + "/";
#else
        return std::filesystem::current_path().string() + "/";
#endif
    }();
    return dir;
}

std::string resolve(const std::string& relative) {
    return exeDir() + relative;
}

} // namespace mc
