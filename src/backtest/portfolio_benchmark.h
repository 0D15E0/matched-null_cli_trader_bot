#pragma once

#include "../core/candle.h"

#include <stdexcept>
#include <vector>

namespace trader {

// Build an equal-weight buy-and-hold curve over a common timestamp grid.
// Each sleeve enters at the first common grid bar's next open and is
// liquidated at the final close, with the same fee and slippage convention as
// buyAndHoldStats().
inline std::vector<double> equalWeightBuyAndHoldCurve(
    const std::vector<const CandleSeries*>& series,
    const std::vector<std::vector<size_t>>& indices,
    double feePct,
    double slippagePct) {
    if (series.empty() || series.size() != indices.size() || indices.front().size() < 2)
        throw std::invalid_argument("cannot build an empty portfolio benchmark");

    const size_t sleeveCount = series.size();
    const size_t gridSize = indices.front().size();
    const double sleeveBudget = 1.0 / static_cast<double>(sleeveCount);
    std::vector<double> cash(sleeveCount, 0.0);
    std::vector<double> units(sleeveCount, 0.0);

    for (size_t i = 0; i < sleeveCount; ++i) {
        if (indices[i].size() != gridSize || indices[i][1] >= series[i]->size())
            throw std::invalid_argument("portfolio benchmark grid is inconsistent");
        const size_t entryIndex = indices[i][1];
        const double fillPrice = series[i]->open[entryIndex] * (1.0 + slippagePct);
        if (fillPrice <= 0.0)
            throw std::invalid_argument("portfolio benchmark has a non-positive entry price");
        const double gross = sleeveBudget / (1.0 + feePct);
        units[i] = gross / fillPrice;
        cash[i] = sleeveBudget - gross - gross * feePct;
    }

    std::vector<double> curve(gridSize, 0.0);
    curve[0] = 1.0;
    for (size_t t = 1; t < gridSize; ++t) {
        double equity = 0.0;
        for (size_t i = 0; i < sleeveCount; ++i) {
            const size_t index = indices[i][t];
            if (index >= series[i]->size())
                throw std::invalid_argument("portfolio benchmark index is out of bounds");
            if (t + 1 == gridSize) {
                const double exitPrice = series[i]->close[index] * (1.0 - slippagePct);
                const double proceeds = units[i] * exitPrice;
                equity += cash[i] + proceeds - proceeds * feePct;
            } else {
                equity += cash[i] + units[i] * series[i]->close[index];
            }
        }
        curve[t] = equity;
    }
    return curve;
}

} // namespace trader