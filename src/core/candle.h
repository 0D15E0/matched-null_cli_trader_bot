#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <optional>

namespace trader {

// Single OHLCV candle. Kept POD/trivially-copyable for fast bulk I/O.
struct Candle {
    int64_t  timestamp = 0; // unix epoch seconds (candle open time)
    double   open = 0.0;
    double   high = 0.0;
    double   low  = 0.0;
    double   close = 0.0;
    double   volume = 0.0;
};

// Structure-of-Arrays candle series: much more cache-friendly than the
// legacy AoS `QList<QPointF>` "studies" used by PortfolioManager, and lets
// indicators operate on contiguous double vectors (vectorizable, better
// for SIMD/auto-vectorization, avoids per-candle heap objects).
struct CandleSeries {
    std::string symbol;       // e.g. "BTC_USDT"
    int64_t     periodSeconds = 0; // candle timeframe, e.g. 14400 = 4h

    std::vector<int64_t> timestamp;
    std::vector<double>  open;
    std::vector<double>  high;
    std::vector<double>  low;
    std::vector<double>  close;
    std::vector<double>  volume;

    size_t size() const { return timestamp.size(); }
    bool   empty() const { return timestamp.empty(); }

    // How many bars this series actually delivers per calendar year,
    // measured from the data rather than derived from `periodSeconds`.
    //
    // This matters because it is the annualization factor for Sharpe and
    // CAGR, and the naive `31557600 / periodSeconds` is wrong for anything
    // that doesn't trade continuously: daily equity bars arrive ~252 times a
    // year, not 365, because of weekends and holidays (and Yahoo's daily
    // timestamps additionally shift by an hour across DST). Measuring
    // elapsed wall-clock against the bar count handles crypto (24/7),
    // equities, and gappy history uniformly, with no per-venue table.
    double barsPerYear() const {
        constexpr double kSecondsPerYear = 365.25 * 24.0 * 3600.0;
        if (size() >= 30) {
            double span = static_cast<double>(timestamp.back() - timestamp.front());
            if (span > 0.0) return (static_cast<double>(size()) - 1.0) * kSecondsPerYear / span;
        }
        // Too short to measure: fall back to the nominal period, which
        // assumes continuous trading (right for crypto, ~45% high for
        // equities - but a series this short cannot support a meaningful
        // annualized statistic anyway).
        if (periodSeconds > 0) return kSecondsPerYear / static_cast<double>(periodSeconds);
        return 0.0;
    }

    // Elapsed calendar years covered by the series (used for CAGR).
    double spanYears() const {
        constexpr double kSecondsPerYear = 365.25 * 24.0 * 3600.0;
        if (size() < 2) return 0.0;
        return static_cast<double>(timestamp.back() - timestamp.front()) / kSecondsPerYear;
    }

    void push(const Candle& c) {
        timestamp.push_back(c.timestamp);
        open.push_back(c.open);
        high.push_back(c.high);
        low.push_back(c.low);
        close.push_back(c.close);
        volume.push_back(c.volume);
    }

    void reserve(size_t n) {
        timestamp.reserve(n); open.reserve(n); high.reserve(n);
        low.reserve(n); close.reserve(n); volume.reserve(n);
    }

    Candle at(size_t i) const {
        return Candle{timestamp[i], open[i], high[i], low[i], close[i], volume[i]};
    }

    // Returns a new series containing only candles with
    // startInclusive <= timestamp <= endInclusive (unbounded on either
    // side if the corresponding optional is empty). Useful for isolating
    // a specific historical regime (e.g. a bear market) for backtesting,
    // without needing a separate on-disk store per date range.
    CandleSeries slice(std::optional<int64_t> startInclusive, std::optional<int64_t> endInclusive) const {
        CandleSeries out;
        out.symbol = symbol;
        out.periodSeconds = periodSeconds;
        out.reserve(size());
        for (size_t i = 0; i < size(); ++i) {
            if (startInclusive.has_value() && timestamp[i] < *startInclusive) continue;
            if (endInclusive.has_value() && timestamp[i] > *endInclusive) continue;
            out.push(at(i));
        }
        return out;
    }
};

} // namespace trader
