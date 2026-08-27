#pragma once
#include "../strategy.h"
#include "zoo_common.h"
#include <string>

namespace trader {

// CONTROLS. Neither of these is a strategy anybody should trade; both exist
// so the tournament's answers can be read.
//
// A search that reports "the best of 4,000 candidates reached excess Sharpe
// 0.4" has said nothing until you know what the best of 4,000 candidates
// reaches when none of them can possibly work. The random-entry control is
// that measurement, run through the identical fee model, fill timing, sizing
// and fitness function as everything else. If it survives the same threshold
// the real strategies are judged by, the threshold is measuring luck.
//
// The deflated Sharpe ratio in metrics.h answers the same question
// analytically. Having both matters: the analytic version assumes
// independent, normally distributed trials, and a population of mutated
// survivors is neither.

// Coin-flip entries with a fixed holding period.
//
// The randomness is a hash of (seed, bar timestamp), not a stateful RNG,
// because the Strategy contract requires that re-evaluating a bar return the
// same signal - the live loop and the backtest both depend on it - and
// because a control whose result changes between runs cannot be compared to
// anything.
struct RandomEntryParams {
    double entryProb = 0.02;   // probability of opening on any given bar
    int holdBars = 20;
    int seed = 12345;
};

class RandomEntryStrategy : public Strategy {
public:
    explicit RandomEntryStrategy(RandomEntryParams p = {}) : p_(p) {}
    std::string name() const override { return "control_random"; }
    void prepare(const CandleSeries&) override {}

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& pos) override {
        if (pos.inPosition) {
            return i >= pos.entryIndex + static_cast<size_t>(std::max(1, p_.holdBars))
                       ? Signal::Sell : Signal::Hold;
        }
        double u = zoo::uniform01(static_cast<uint64_t>(p_.seed), s.timestamp[i]);
        return u < p_.entryProb ? Signal::Buy : Signal::Hold;
    }

private:
    RandomEntryParams p_;
};

// Buy the first bar and never sell. Run through the same engine, this is
// buy-and-hold with one round trip of fees - so its excess Sharpe should sit
// a hair below zero by construction. It is the arithmetic check that the
// benchmark inside BacktestReport is measuring what it says it is.
class AlwaysLongStrategy : public Strategy {
public:
    std::string name() const override { return "control_always_long"; }
    void prepare(const CandleSeries&) override {}
    Signal onBar(const CandleSeries&, size_t, const PositionContext& pos) override {
        return pos.inPosition ? Signal::Hold : Signal::Buy;
    }
};

} // namespace trader
