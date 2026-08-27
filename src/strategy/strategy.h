#pragma once
#include "../core/candle.h"
#include <cstdint>
#include <string>

namespace trader {

enum class Signal { Hold, Buy, Sell };

// The position a strategy is being asked to reason about, owned and updated
// by whoever is driving the strategy (BacktestEngine or LiveTrader) rather
// than by the strategy itself.
//
// Why this exists: strategies used to keep their own `inPosition_` /
// `highestSinceEntry_` members, mutated inside onBar(). That worked in the
// backtest (which replays every bar in order under a single prepare()) but
// broke everywhere else:
//   * LiveTrader called prepare() every poll cycle, which reset the flag, so
//     the exit branch of a stateful strategy was unreachable - the bot could
//     buy but never sell.
//   * A decorator that suppressed a Buy (HurstRegimeFilter) left the inner
//     strategy believing it held a position the engine never opened.
// Making the position an explicit input keeps strategies pure functions of
// (series, index, position), so the same call produces the same signal in
// backtest, replay and live, which is the precondition for backtest results
// predicting anything at all about live results.
struct PositionContext {
    bool    inPosition   = false;
    double  entryPrice   = 0.0;  // fill price of the open position
    double  highestClose = 0.0;  // highest close seen since entry (trailing stops)
    double  lowestClose  = 0.0;  // lowest close seen since entry
    int64_t entryTime    = 0;    // timestamp of the bar the position opened on
    size_t  entryIndex   = 0;    // series index of that bar

    void open(double price, int64_t time, size_t index) {
        inPosition = true;
        entryPrice = price;
        highestClose = price;
        lowestClose = price;
        entryTime = time;
        entryIndex = index;
    }
    void close() { *this = PositionContext{}; }

    // Called once per bar by the driver while a position is open, before the
    // strategy is consulted, so trailing-stop logic sees the current bar.
    void observe(double close) {
        if (!inPosition) return;
        if (close > highestClose) highestClose = close;
        if (close < lowestClose) lowestClose = close;
    }
};

// Pluggable strategy interface. A strategy sees the full series (for
// indicator context) plus the current bar index, and must only look at data
// at or before `index` (the drivers call with increasing indices, but
// strategies must not peek ahead into series arrays beyond `index`).
//
// The same interface backs backtesting and live trading: LiveTrader replays
// every not-yet-processed closed bar through onBar() exactly the way
// BacktestEngine does, passing the position it is actually holding.
class Strategy {
public:
    virtual ~Strategy() = default;
    virtual std::string name() const = 0;

    // Called once with the full historical series before the first onBar,
    // so the strategy can precompute indicators over the whole series
    // (much faster than recomputing a rolling window per bar). Must be
    // idempotent: the live loop calls it again whenever new candles arrive.
    virtual void prepare(const CandleSeries& series) = 0;

    // Evaluate the strategy at bar `index` given the position currently
    // held. Must be O(1) after prepare(), and must not mutate any state
    // that affects future calls - re-evaluating the same (index, position)
    // must return the same signal.
    virtual Signal onBar(const CandleSeries& series, size_t index,
                          const PositionContext& position) = 0;
};

} // namespace trader
