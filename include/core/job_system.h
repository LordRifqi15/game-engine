#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace engine {

// Minimal thread pool: fixed workers, FIFO queue, wait-for-all.
// ponytail: mutex+cv queue; lock-free MPMC queue if profiling ever demands it.
class JobSystem {
public:
    void init(size_t threadCount);
    void shutdown();

    void enqueue(std::function<void()> job);

    // Blocks until all queued (and in-flight) jobs finish.
    void wait();

    size_t threadCount() const { return threads_.size(); }

private:
    void workerLoop();

    std::vector<std::thread> threads_;
    std::queue<std::function<void()>> jobs_;
    std::mutex mutex_;
    std::condition_variable cv_;         // wakes idle workers
    std::condition_variable doneCv_;     // wakes wait()
    uint32_t activeJobs_ = 0;
    bool shutdown_ = false;
};

// Splits [0, count) into per-thread ranges and runs func(begin, end) on each.
// Blocks until all chunks complete. Falls back to serial when count is tiny
// or the system has one thread.
template <typename Func>
void parallel_for(JobSystem& jobs, size_t count, Func&& func) {
    const size_t nThreads = jobs.threadCount();
    if (nThreads <= 1 || count < 2 * nThreads) {
        func(0, count);
        return;
    }

    const size_t chunk = (count + nThreads - 1) / nThreads;
    for (size_t t = 0; t < nThreads; ++t) {
        size_t begin = t * chunk;
        if (begin >= count) break;
        size_t end = begin + chunk;
        if (end > count) end = count;
        jobs.enqueue([&func, begin, end] { func(begin, end); });
    }
    jobs.wait();
}

} // namespace engine
