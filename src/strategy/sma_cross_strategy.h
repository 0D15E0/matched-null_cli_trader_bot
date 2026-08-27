#pragma once
#include "strategy.h"
#include "../indicators/indicators.h"
#include <cmath>

namespace trader {

// Simple SMA crossover strategy, included as a second, minimal example of
// implementing the Strategy interface (useful for smoke-testing the
// backtest engine independent of the more complex OdiseoStrategy).
class SmaCrossStrategy : public Strategy {
public:
    SmaCrossStrategy(int fast = 10, int slow = 30) : fast_(fast), slow_(slow) {}

    std::string name() const override { return "sma_cross"; }

    void prepare(const CandleSeries& series) override {
        fastSma_ = indicators::sma(series.close, fast_);
        slowSma_ = indicators::sma(series.close, slow_);
    }

    Signal onBar(const CandleSeries&, size_t i, const PositionContext&) override {
        if (i == 0 || std::isnan(fastSma_[i]) || std::isnan(slowSma_[i]) ||
            std::isnan(fastSma_[i - 1]) || std::isnan(slowSma_[i - 1]))
            return Signal::Hold;

        bool wasBelow = fastSma_[i - 1] <= slowSma_[i - 1];
        bool isAbove  = fastSma_[i] > slowSma_[i];
        if (wasBelow && isAbove) return Signal::Buy;

        bool wasAbove = fastSma_[i - 1] >= slowSma_[i - 1];
        bool isBelow  = fastSma_[i] < slowSma_[i];
        if (wasAbove && isBelow) return Signal::Sell;

        return Signal::Hold;
    }

private:
    int fast_, slow_;
    std::vector<double> fastSma_, slowSma_;
};

} // namespace trader
