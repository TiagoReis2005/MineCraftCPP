#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace mc {

// Minimal fixed-size worker pool for CPU jobs (chunk generation + meshing).
class ThreadPool {
public:
    ~ThreadPool() { stop(); }

    void start(unsigned threadCount = 0); // 0 = auto (hardware_concurrency - 1)
    void stop();

    // lowPriority tasks (far LOD generation) only run when no normal task (full-detail
    // chunk gen/mesh) is waiting, so the real chunks never wait behind LOD work -- LOD
    // just soaks up whatever worker time the chunks leave free.
    void enqueue(std::function<void()> task, bool lowPriority = false);
    unsigned threadCount() const { return static_cast<unsigned>(workers_.size()); }

private:
    void workerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;    // normal priority (chunks)
    std::queue<std::function<void()>> lowTasks_; // low priority (LOD)
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

} // namespace mc
