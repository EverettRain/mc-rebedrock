#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace mc::core {

// Small persistent pool for the serial stages of the chunk pipeline. A caller
// submits one indexed batch at a time; the workers steal indexes until the batch
// is complete. Keeping the threads alive avoids paying thread construction and
// teardown once for generation, once for lighting and again for meshing on
// every streaming batch.
//
// run() is deliberately synchronous and non-reentrant. That matches the chunk
// worker, where the three parallel stages execute in order and must finish
// before the next one can see their output.
class ParallelWorkerPool final {
  public:
    [[nodiscard]] static std::size_t recommendedWorkerCount() {
        const std::size_t hardwareThreads =
            std::max<std::size_t>(1U, std::thread::hardware_concurrency());
        return std::clamp(hardwareThreads - 1U, std::size_t{1U}, std::size_t{7U});
    }

    explicit ParallelWorkerPool(
        std::size_t workerCount = recommendedWorkerCount())
        : workerCount_(std::max<std::size_t>(1U, workerCount)) {
        if (workerCount_ == 1U) return;
        workers_.reserve(workerCount_);
        for (std::size_t workerIndex = 0U; workerIndex < workerCount_; ++workerIndex) {
            workers_.emplace_back([this, workerIndex] { workerLoop(workerIndex); });
        }
    }

    ~ParallelWorkerPool() {
        {
            std::lock_guard lock{mutex_};
            stopping_ = true;
            ++generation_;
        }
        startCv_.notify_all();
    }

    ParallelWorkerPool(const ParallelWorkerPool&) = delete;
    ParallelWorkerPool& operator=(const ParallelWorkerPool&) = delete;

    [[nodiscard]] std::size_t workerCount() const { return workerCount_; }

    template <typename Function>
    void run(std::size_t taskCount, Function&& function) {
        if (taskCount == 0U) return;
        if (workers_.empty() || taskCount == 1U) {
            for (std::size_t index = 0U; index < taskCount; ++index) {
                std::invoke(function, index, std::size_t{0U});
            }
            return;
        }

        std::unique_lock lock{mutex_};
        finishedCv_.wait(lock, [this] { return !jobActive_; });
        task_ = std::forward<Function>(function);
        taskCount_ = taskCount;
        nextTask_ = 0U;
        remainingWorkers_ = workers_.size();
        jobError_ = nullptr;
        jobActive_ = true;
        ++generation_;
        startCv_.notify_all();
        finishedCv_.wait(lock, [this] { return !jobActive_; });
        const std::exception_ptr error = jobError_;
        task_ = {};
        lock.unlock();
        if (error) std::rethrow_exception(error);
    }

  private:
    void workerLoop(std::size_t workerIndex) {
        std::size_t observedGeneration = 0U;
        while (true) {
            std::function<void(std::size_t, std::size_t)> task;
            {
                std::unique_lock lock{mutex_};
                startCv_.wait(lock, [this, observedGeneration] {
                    return stopping_ || generation_ != observedGeneration;
                });
                if (stopping_) return;
                observedGeneration = generation_;
                task = task_;
            }

            while (true) {
                std::size_t index = 0U;
                {
                    std::lock_guard lock{mutex_};
                    if (nextTask_ >= taskCount_) break;
                    index = nextTask_++;
                }
                try {
                    task(index, workerIndex);
                } catch (...) {
                    std::lock_guard lock{mutex_};
                    if (!jobError_) jobError_ = std::current_exception();
                }
            }

            {
                std::lock_guard lock{mutex_};
                if (--remainingWorkers_ == 0U) {
                    jobActive_ = false;
                    finishedCv_.notify_all();
                }
            }
        }
    }

    const std::size_t workerCount_;
    std::mutex mutex_;
    std::condition_variable startCv_;
    std::condition_variable finishedCv_;
    std::function<void(std::size_t, std::size_t)> task_;
    std::size_t taskCount_ = 0U;
    std::size_t nextTask_ = 0U;
    std::size_t remainingWorkers_ = 0U;
    std::size_t generation_ = 0U;
    std::exception_ptr jobError_;
    bool jobActive_ = false;
    bool stopping_ = false;
    // Declared last so jthread destruction/join happens before the condition
    // variables and mutex they wait on are destroyed.
    std::vector<std::jthread> workers_;
};

} // namespace mc::core
