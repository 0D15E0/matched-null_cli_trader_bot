#pragma once
#include <chrono>
#include <mutex>
#include <thread>
#include <algorithm>

namespace trader {

// Simple thread-safe token-bucket rate limiter.
//
// Public REST endpoints (Poloniex, Kraken, etc.) enforce request-rate
// limits. The legacy code had no explicit throttling beyond a single
// QTimer per Task, which does not scale once many symbols/timeframes are
// fetched concurrently via a thread pool.
//
// TODO(bot-mode): a live trading bot must respect *per-endpoint* limits
// (public market data vs. private trading endpoints usually have separate,
// stricter, limits, and Poloniex trading endpoints are typically weighted).
// Extend this into a per-endpoint-class limiter registry, and consider
// a persistent WebSocket connection for market data instead
// of polling REST at all once in bot mode.
class RateLimiter {
public:
    // capacity: max burst size. refillPerSecond: tokens regenerated per second.
    RateLimiter(double capacity, double refillPerSecond)
        : capacity_(capacity), tokens_(capacity), refillPerSecond_(refillPerSecond),
          lastRefill_(std::chrono::steady_clock::now()) {}

    // Blocks (sleeping, not busy-waiting) until a token is available, then
    // consumes it. Safe to call from multiple threads.
    void acquire() {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                refill();
                if (tokens_ >= 1.0) {
                    tokens_ -= 1.0;
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

private:
    void refill() {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - lastRefill_).count();
        tokens_ = std::min(capacity_, tokens_ + elapsed * refillPerSecond_);
        lastRefill_ = now;
    }

    double capacity_;
    double tokens_;
    double refillPerSecond_;
    std::chrono::steady_clock::time_point lastRefill_;
    std::mutex mutex_;
};

} // namespace trader
