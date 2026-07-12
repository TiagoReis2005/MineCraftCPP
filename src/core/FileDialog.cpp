#include "core/FileDialog.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

namespace mc {

std::string openPngFileDialog() {
    char file[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = "PNG skin (64x64)\0*.png\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Choose a skin";
    // NOCHANGEDIR: the dialog must not move our working directory (asset paths).
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return {};
    return file;
}

} // namespace mc

#else

namespace mc {
std::string openPngFileDialog() { return {}; }
} // namespace mc

#endif
