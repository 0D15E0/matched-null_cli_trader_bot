#include "backtest/portfolio_benchmark.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

bool closeEnough(double actual, double expected) {
    return std::fabs(actual - expected) <= 1e-12;
}

} // namespace

int main() {
    trader::CandleSeries first;
    first.push({0, 10.0, 10.0, 10.0, 10.0, 1.0});
    first.push({1, 11.0, 12.0, 11.0, 12.0, 1.0});
    first.push({2, 12.0, 14.0, 12.0, 14.0, 1.0});

    trader::CandleSeries second;
    second.push({0, 20.0, 20.0, 20.0, 20.0, 1.0});
    second.push({1, 22.0, 22.0, 18.0, 18.0, 1.0});
    second.push({2, 24.0, 24.0, 16.0, 16.0, 1.0});

    std::vector<const trader::CandleSeries*> series{&first, &second};
    std::vector<std::vector<size_t>> indices{{0, 1, 2}, {0, 1, 2}};
    constexpr double fee = 0.10;
    constexpr double slippage = 0.05;
    const auto curve = trader::equalWeightBuyAndHoldCurve(series, indices, fee, slippage);

    const double budget = 0.5;
    const double gross = budget / (1.0 + fee);
    const double firstUnits = gross / (11.0 * (1.0 + slippage));
    const double secondUnits = gross / (22.0 * (1.0 + slippage));
    const double expectedAtMiddle = firstUnits * 12.0 + secondUnits * 18.0;
    const double firstProceeds = firstUnits * 14.0 * (1.0 - slippage);
    const double secondProceeds = secondUnits * 16.0 * (1.0 - slippage);
    const double expectedAtEnd =
        (firstProceeds - firstProceeds * fee) +
        (secondProceeds - secondProceeds * fee);

    if (curve.size() != 3 || !closeEnough(curve[0], 1.0) ||
        !closeEnough(curve[1], expectedAtMiddle) ||
        !closeEnough(curve[2], expectedAtEnd)) {
        std::cerr << "portfolio benchmark curve mismatch\n";
        return 1;
    }

    bool rejected = false;
    try {
        trader::equalWeightBuyAndHoldCurve(series, {{0}}, fee, slippage);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected) {
        std::cerr << "portfolio benchmark accepted an unusable grid\n";
        return 1;
    }
    return 0;
}