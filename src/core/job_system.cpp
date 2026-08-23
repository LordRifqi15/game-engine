#include "core/job_system.h"

namespace engine {

void JobSystem::init(size_t threadCount) {
    shutdown();
    if (threadCount == 0) return;

    threads_.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i) {
        threads_.emplace_back([this] { workerLoop(); });
    }
}

void JobSystem::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (threads_.empty()) return;
        shutdown_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
    threads_.clear();

    // Drain any leftovers (shouldn't happen after wait(), but be safe).
    std::queue<std::function<void()>> empty;
    jobs_.swap(empty);
    activeJobs_ = 0;
    shutdown_ = false;
}

void JobSystem::enqueue(std::function<void()> job) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++activeJobs_;
    jobs_.push(std::move(job));
    cv_.notify_one();
}

void JobSystem::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    doneCv_.wait(lock, [this] { return activeJobs_ == 0 && jobs_.empty(); });
}

void JobSystem::workerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return shutdown_ || !jobs_.empty(); });
            if (shutdown_ && jobs_.empty()) return;

            job = std::move(jobs_.front());
            jobs_.pop();
        }

        job();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            --activeJobs_;
            if (activeJobs_ == 0 && jobs_.empty()) doneCv_.notify_all();
        }
    }
}

} // namespace engine
