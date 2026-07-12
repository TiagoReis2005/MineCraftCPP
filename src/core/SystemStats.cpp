#include "core/SystemStats.h"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <psapi.h>
#  pragma comment(lib, "psapi.lib")
#  include <cstring>
#endif

namespace mc {

uint64_t processMemoryBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<uint64_t>(pmc.WorkingSetSize);
    }
#endif
    return 0;
}

int systemMemoryLoadPercent() {
#if defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) return static_cast<int>(ms.dwMemoryLoad);
#endif
    return 0;
}

float cpuUsagePercent() {
#if defined(_WIN32)
    static bool initialized = false;
    static int numProcessors = 1;
    static ULARGE_INTEGER lastWall{}, lastKernel{}, lastUser{};

    FILETIME ftWall, ftCreate, ftExit, ftKernel, ftUser;
    GetSystemTimeAsFileTime(&ftWall);
    GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit, &ftKernel, &ftUser);

    ULARGE_INTEGER wall, kernel, user;
    std::memcpy(&wall, &ftWall, sizeof(FILETIME));
    std::memcpy(&kernel, &ftKernel, sizeof(FILETIME));
    std::memcpy(&user, &ftUser, sizeof(FILETIME));

    if (!initialized) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        numProcessors = static_cast<int>(si.dwNumberOfProcessors);
        lastWall = wall;
        lastKernel = kernel;
        lastUser = user;
        initialized = true;
        return 0.0f;
    }

    double busy = static_cast<double>((kernel.QuadPart - lastKernel.QuadPart) +
                                      (user.QuadPart - lastUser.QuadPart));
    double elapsed = static_cast<double>(wall.QuadPart - lastWall.QuadPart);
    lastWall = wall;
    lastKernel = kernel;
    lastUser = user;

    if (elapsed <= 0.0) return 0.0f;
    return static_cast<float>(busy / elapsed / numProcessors * 100.0);
#else
    return 0.0f;
#endif
}

} // namespace mc
