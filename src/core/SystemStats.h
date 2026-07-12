#pragma once

#include <cstdint>

namespace mc {

// Resident memory of this process, in bytes.
uint64_t processMemoryBytes();

// System-wide physical memory in use, 0..100. The world streamer stops generating and
// unloads harder above ~85% so filling RAM degrades gracefully instead of crashing.
int systemMemoryLoadPercent();

// This process's CPU usage as a percentage of one core (can exceed 100% across cores).
// Stateful: call about once per frame; the first call returns 0.
float cpuUsagePercent();

} // namespace mc
