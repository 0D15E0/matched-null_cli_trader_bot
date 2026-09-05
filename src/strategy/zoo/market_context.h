#pragma once
#include "../../core/candle.h"
#include "../../core/candle_store.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Reference series for strategies that read MORE THAN ONE instrument.
//
// Every family in this zoo up to 2026-09-05 was a function of a single price
// series, because that is all the Strategy interface hands over: prepare()
// receives the instrument being traded and nothing else. A strategy that wants
// to condition on another market (the crypto factor read off BTC, a breadth
// count, a macro proxy) needs a way to get at a second store, and a registry
// factory that takes a flat vector of doubles cannot carry a file path.
//
// So the "where is the data" fact lives here, process-wide: main.cpp sets the
// data directory once from --data-dir (the same flag every command already
// takes) and a strategy asks for a symbol at a period. The context loads the
// store lazily and caches it; the tournament runs strategies on a thread pool,
// hence the mutex.
//
// CAUSALITY IS THE CALLER'S JOB, AND THE HELPER BELOW IS HOW IT IS DONE. The
// reference store on disk is the FULL history to the present, whatever slice
// of the traded series a walk-forward fold or --end handed to prepare(). A
// strategy must therefore only ever read reference bars whose timestamp is at
// or before the bar it is deciding on - alignByTimestamp() returns exactly
// that mapping, with an optional lag in bars. Read a reference bar by INDEX
// (series index i -> reference index i) and the result is garbage on two
// counts: the stores have different lengths and gaps, and a truncated traded
// series would silently map onto the wrong dates. causality_check exercises
// every registry family, including the ones that use this, by re-running
// prepare() on a truncated prefix; a strategy that reads the reference
// correctly gives identical signals either way, because nothing it reads
// depends on how long the traded series is.
//
// WHY A LAG EXISTS. Live, each sleeve is its own process fetching its own
// symbol. When the ETH sleeve evaluates the bar that closed at T, the BTC
// process may not yet have fetched BTC's bar for T. A strategy that reads the
// reference at T - 1 bar only needs the BTC store to be at most one bar stale,
// which the 60s poll loop guarantees in practice, and the backtest reads the
// same lagged bar - so the parity tool sees the same signal in both. A
// strategy that insists on the contemporaneous bar would be causal in the
// backtest and quietly different live.
namespace trader::zoo {

class MarketContext {
public:
    static MarketContext& instance() {
        static MarketContext ctx;
        return ctx;
    }

    void setDataDir(std::string dir) {
        std::lock_guard<std::mutex> lock(mu_);
        if (dir != dataDir_) cache_.clear();
        dataDir_ = std::move(dir);
    }

    std::string dataDir() const {
        std::lock_guard<std::mutex> lock(mu_);
        return dataDir_;
    }

    // Inject a series directly (tests, tools). Overrides anything on disk.
    void setSeries(const std::string& symbol, int64_t period, CandleSeries s) {
        std::lock_guard<std::mutex> lock(mu_);
        int64_t last = s.empty() ? 0 : s.timestamp.back();
        cache_[key(symbol, period)] = Entry{std::make_shared<const CandleSeries>(std::move(s)), last, true};
    }

    // The reference series, or nullptr when no store exists for it. Never
    // throws: the STRATEGY decides what a missing reference means (and it
    // should refuse to run rather than silently degrade to a different rule).
    //
    // A cached series is re-read when the store on disk has grown. The live
    // loop calls prepare() every poll cycle for as long as the process lives,
    // and the reference store is being appended to by ANOTHER sleeve's
    // process; a cache filled once at start-up would freeze the factor at the
    // moment the bot was launched. The check is one header read plus a file
    // size (CandleStore::lastTimestamp), not a load. Injected series (tests)
    // are never re-read.
    std::shared_ptr<const CandleSeries> get(const std::string& symbol, int64_t period) {
        std::lock_guard<std::mutex> lock(mu_);
        auto k = key(symbol, period);
        auto it = cache_.find(k);
        if (it != cache_.end() && it->second.injected) return it->second.series;
        CandleStore store(dataDir_ + "/" + symbol + "_" + std::to_string(period) + ".ctc");
        if (!store.exists()) return it != cache_.end() ? it->second.series : nullptr;
        auto last = store.lastTimestamp();
        if (it != cache_.end() && last.has_value() && it->second.lastTimestamp == *last)
            return it->second.series;
        auto loaded = std::make_shared<const CandleSeries>(store.load());
        if (loaded->empty()) return nullptr;
        cache_[k] = Entry{loaded, last.value_or(0), false};
        return loaded;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        cache_.clear();
    }

private:
    MarketContext() = default;
    static std::string key(const std::string& symbol, int64_t period) {
        return symbol + ":" + std::to_string(period);
    }

    struct Entry {
        std::shared_ptr<const CandleSeries> series;
        int64_t lastTimestamp = 0;   // of the store when loaded; drives the re-read check
        bool injected = false;       // setSeries(): never re-read from disk
    };

    mutable std::mutex mu_;
    std::string dataDir_ = "data";
    std::map<std::string, Entry> cache_;
};

// For every bar i of `traded`, the index of the LATEST bar of `reference` whose
// timestamp is <= traded.timestamp[i] - lagBars * traded.periodSeconds, or -1
// when no such bar exists. O(n + m): both timestamp vectors are sorted.
//
// This is the only correct way to read one series while deciding on another.
// It is causal by construction (nothing after the decision bar is reachable),
// it is independent of how long the traded series happens to be (so truncating
// it changes nothing before the truncation point), and it handles the stores'
// different starts, ends and gaps by construction. A missing reference bar
// (venue outage on one side only) resolves to the most recent earlier one,
// which is what a live process would also see.
inline std::vector<long> alignByTimestamp(const CandleSeries& traded, const CandleSeries& reference,
                                          int lagBars) {
    std::vector<long> out(traded.size(), -1);
    if (reference.empty()) return out;
    const int64_t lag = static_cast<int64_t>(std::max(0, lagBars)) * traded.periodSeconds;
    size_t j = 0;
    bool have = false;
    for (size_t i = 0; i < traded.size(); ++i) {
        const int64_t target = traded.timestamp[i] - lag;
        while (j + 1 < reference.size() && reference.timestamp[j + 1] <= target) { ++j; have = true; }
        if (!have && reference.timestamp[j] <= target) have = true;
        out[i] = have ? static_cast<long>(j) : -1;
    }
    return out;
}

} // namespace trader::zoo
