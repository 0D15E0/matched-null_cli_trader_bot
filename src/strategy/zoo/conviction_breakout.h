#pragma once

#include "../strategy.h"
#include "zoo_common.h"
#include "../../indicators/indicators.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace trader {

// A breakout is more useful when it proves three things at once: price has
// escaped a recent range, the slow regime is rising, and the move has enough
// directional follow-through to clear noise. This strategy deliberately
// combines those tests without mutating state in onBar().
struct ConvictionBreakoutParams {
    int entryWindow = 55;
    int exitWindow = 20;
    int trendWindow = 200;
    int slopeWindow = 20;
    int momentumWindow = 90;
    double momentumThreshold = 0.0;
    int atrWindow = 20;
    double breakoutAtr = 0.25;
    double stopAtr = 3.0;
};

class ConvictionBreakoutStrategy : public Strategy {
public:
    explicit ConvictionBreakoutStrategy(ConvictionBreakoutParams p = {}) : p_(p) {}

    std::string name() const override { return "conviction_breakout"; }

    void prepare(const CandleSeries& s) override {
        int entry = std::max(2, p_.entryWindow);
        int exit = std::max(2, p_.exitWindow);
        int trend = std::max(2, p_.trendWindow);
        int slope = std::max(1, p_.slopeWindow);
        int momentum = std::max(1, p_.momentumWindow);
        int atrWindow = std::max(2, p_.atrWindow);

        upper_ = zoo::rollingMaxExclusive(s.high, entry);
        lower_ = zoo::rollingMinExclusive(s.low, exit);
        movingAverage_ = indicators::sma(s.close, trend);
        atr_ = indicators::atr(s.high, s.low, s.close, atrWindow);
        momentum_.assign(s.size(), std::nan(""));
        for (size_t i = static_cast<size_t>(momentum); i < s.size(); ++i) {
            double prior = s.close[i - static_cast<size_t>(momentum)];
            if (prior > 0.0) momentum_[i] = s.close[i] / prior - 1.0;
        }
        rising_.assign(s.size(), false);
        for (size_t i = static_cast<size_t>(slope); i < s.size(); ++i) {
            size_t prior = i - static_cast<size_t>(slope);
            rising_[i] = zoo::ok(movingAverage_[i]) && zoo::ok(movingAverage_[prior]) &&
                         movingAverage_[i] > movingAverage_[prior];
        }
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& position) override {
        if (i >= s.size() || i >= upper_.size() || !zoo::ok(upper_[i]) ||
            !zoo::ok(movingAverage_[i]) || !zoo::ok(momentum_[i]) ||
            !zoo::ok(atr_[i]) || atr_[i] <= 0.0) {
            return Signal::Hold;
        }

        if (position.inPosition) {
            double trailingStop = position.highestClose - p_.stopAtr * atr_[i];
            if ((zoo::ok(lower_[i]) && s.close[i] < lower_[i]) ||
                s.close[i] < movingAverage_[i] || s.close[i] < trailingStop) {
                return Signal::Sell;
            }
            return Signal::Hold;
        }

        bool breakout = s.close[i] > upper_[i] + p_.breakoutAtr * atr_[i];
        bool regime = rising_[i] && s.close[i] > movingAverage_[i];
        bool momentum = momentum_[i] > p_.momentumThreshold;
        return breakout && regime && momentum ? Signal::Buy : Signal::Hold;
    }

private:
    ConvictionBreakoutParams p_;
    std::vector<double> upper_, lower_, movingAverage_, atr_, momentum_;
    std::vector<char> rising_;
};

} // namespace trader