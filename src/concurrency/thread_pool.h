#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>

namespace trader {

// Generic, bounded, fixed-size thread pool.
//
// Used today by the data-fetching layer to download multiple symbol/
// timeframe combinations concurrently without spawning a raw QThread per
// request the way the legacy `Task : public QThread` did.
//
// TODO(bot-mode): this same pool is intended to be reused by the future
// live-trading bot for:
//   - polling multiple markets' order books / tickers concurrently,
//   - handling periodic candle "keep fresh" polling per symbol,
//   - running strategy evaluation off the network I/O thread(s).
// Keep the pool's thread count configurable (currently CPU count based)
// so the bot can tune it separately from backtest workloads.
class ThreadPool {
public:
    explicit ThreadPool(size_t threadCount = std::max(2u, std::thread::hardware_concurrency()))
        : stop_(false) {
        for (size_t i = 0; i < threadCount; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) if (t.joinable()) t.join();
    }

    template <class F, class R = std::invoke_result_t<F>>
    std::future<R> submit(F&& f) {
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    size_t size() const { return workers_.size(); }

private:
    void workerLoop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                job = std::move(tasks_.front());
                tasks_.pop();
            }
            job();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_;
};

} // namespace trader
