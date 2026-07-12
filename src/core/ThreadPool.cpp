#include "core/ThreadPool.h"

#include <algorithm>

namespace mc {

void ThreadPool::start(unsigned threadCount) {
    if (threadCount == 0) {
        unsigned hc = std::thread::hardware_concurrency();
        threadCount = hc > 1 ? hc - 1 : 1; // leave a core for the main/render thread
    }
    stop_ = false;
    for (unsigned i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

void ThreadPool::stop() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stop_ && workers_.empty()) return;
        stop_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
    std::queue<std::function<void()>> empty, emptyLow;
    std::swap(tasks_, empty);
    std::swap(lowTasks_, emptyLow);
}

void ThreadPool::enqueue(std::function<void()> task, bool lowPriority) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        (lowPriority ? lowTasks_ : tasks_).push(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !tasks_.empty() || !lowTasks_.empty(); });
            if (stop_) return; // drop any remaining queued work for a fast, safe shutdown
            // Always drain normal-priority (chunk) work before touching low-priority (LOD).
            std::queue<std::function<void()>>& q = tasks_.empty() ? lowTasks_ : tasks_;
            task = std::move(q.front());
            q.pop();
        }
        task();
    }
}

} // namespace mc
