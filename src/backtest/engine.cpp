#include "engine.h"
#include "../indicators/indicators.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace trader {
namespace {

// Turns an equity curve into the risk/return summary. `lowCurve` is the
// account marked at each bar's LOW rather than its close: a position can be
// deep underwater intrabar and recover by the close, and a drawdown number
// that never looks inside the bar systematically understates the pain a real
// account would have felt (and, worse, understates the risk penalty the
// evolutionary search applies).
PerformanceStats statsFromCurve(const std::vector<double>& closeCurve,
                                 const std::vector<double>& lowCurve,
                                 double startEquity, double barsPerYear, double years,
                                 metrics::ReturnStats* statsOut = nullptr) {
    PerformanceStats p;
    if (closeCurve.empty() || startEquity <= 0.0) return p;

    double endEquity = closeCurve.back();
    p.totalReturnPct = (endEquity - startEquity) / startEquity * 100.0;
    p.cagrPct = metrics::cagrPct(startEquity, endEquity, years);

    std::vector<double> returns;
    returns.reserve(closeCurve.size());
    double prev = startEquity;
    for (double equity : closeCurve) {
        if (prev > 0.0) returns.push_back(equity / prev - 1.0);
        prev = equity;
    }
    metrics::ReturnStats rs = metrics::computeReturnStats(returns);
    if (statsOut) *statsOut = rs;

    p.sharpeAnnual  = metrics::annualizedSharpe(rs, barsPerYear);
    p.sharpeStdErr  = metrics::annualizedSharpeStdError(rs, barsPerYear);
    p.sortinoAnnual = metrics::annualizedSortino(rs, barsPerYear);

    double peak = startEquity;
    for (size_t i = 0; i < closeCurve.size(); ++i) {
        peak = std::max(peak, closeCurve[i]);
        if (peak <= 0.0) continue;
        double trough = lowCurve.empty() ? closeCurve[i]
                                          : std::min(closeCurve[i], lowCurve[i]);
        p.maxDrawdownPct = std::max(p.maxDrawdownPct, (peak - trough) / peak * 100.0);
        p.maxDrawdownClosePct =
            std::max(p.maxDrawdownClosePct, (peak - closeCurve[i]) / peak * 100.0);
    }
    return p;
}

std::string pct(double v, int precision = 2) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(precision) << v << "%";
    return os.str();
}

} // namespace

// First index that should actually be evaluated (everything before it is
// warm-up only). Shared by the strategy path and the benchmark so the two
// always cover exactly the same window.
size_t evaluationStartIndex(const CandleSeries& series, const BacktestConfig& config) {
    if (config.evaluateFromTimestamp <= 0) return 0;
    size_t i = 0;
    while (i < series.size() && series.timestamp[i] < config.evaluateFromTimestamp) ++i;
    return i;
}

PerformanceStats performanceFromCurve(const std::vector<double>& closeCurve,
                                       const std::vector<double>& lowCurve,
                                       double startEquity, double barsPerYear,
                                       double years) {
    return statsFromCurve(closeCurve, lowCurve, startEquity, barsPerYear, years);
}

PerformanceStats buyAndHoldStats(const CandleSeries& series, const BacktestConfig& config) {
    PerformanceStats p;
    if (series.size() < 2) return p;

    const size_t evalStart = evaluationStartIndex(series, config);
    if (evalStart + 2 > series.size()) return p;

    // Enter on the same bar a strategy's first possible entry would fill, and
    // pay the same friction, so the comparison isn't rigged in either
    // direction.
    size_t entryIdx = (config.fillTiming == FillTiming::NextOpen) ? evalStart + 1 : evalStart;
    double rawEntry = (config.fillTiming == FillTiming::NextOpen) ? series.open[entryIdx]
                                                                  : series.close[entryIdx];
    double fillPrice = rawEntry * (1.0 + config.slippagePct);
    if (fillPrice <= 0.0) return p;

    double gross = config.startingEquity / (1.0 + config.feePct);
    double units = gross / fillPrice;
    double cash = config.startingEquity - gross - gross * config.feePct;

    size_t n = series.size() - evalStart;
    std::vector<double> closeCurve(n, config.startingEquity);
    std::vector<double> lowCurve(n, config.startingEquity);
    for (size_t i = entryIdx; i < series.size(); ++i) {
        closeCurve[i - evalStart] = cash + units * series.close[i];
        lowCurve[i - evalStart]   = cash + units * series.low[i];
    }

    // Liquidate at the end with the same friction a forced close pays, so
    // "hold forever" isn't credited with a free, cost-less exit.
    double exitPrice = series.close.back() * (1.0 - config.slippagePct);
    double proceeds = units * exitPrice;
    proceeds -= proceeds * config.feePct;
    closeCurve.back() = cash + proceeds;
    lowCurve.back() = std::min(lowCurve.back(), closeCurve.back());

    double years = (series.timestamp.back() - series.timestamp[evalStart]) / (365.25 * 24.0 * 3600.0);
    return statsFromCurve(closeCurve, lowCurve, config.startingEquity,
                          series.barsPerYear(), years);
}

BacktestReport BacktestEngine::run(const CandleSeries& series, Strategy& strategy) {
    BacktestReport report;
    report.strategyName = strategy.name();
    report.symbol = series.symbol;
    report.startingEquity = config_.startingEquity;
    report.barsPerYear = series.barsPerYear();
    report.searchTrials = config_.searchTrials;
    if (series.empty()) return report;

    // Indicators are prepared over the WHOLE series, including any warm-up
    // prefix, but only bars from evalStart onward are traded and measured.
    const size_t evalStart = evaluationStartIndex(series, config_);
    if (evalStart + 1 >= series.size()) { report.spanYears = 0.0; return report; }
    report.spanYears =
        (series.timestamp.back() - series.timestamp[evalStart]) / (365.25 * 24.0 * 3600.0);

    strategy.prepare(series);

    // Volatility used for sizing. With the Trailing model this is the same
    // rolling standard deviation the engine has always used; with HAR it is a
    // forecast of the NEXT bar's volatility, which is the bar the position is
    // actually held through. Both are strictly causal - vol_forecast.cpp is
    // gated by a test asserting that recomputing from a truncated series
    // reproduces each value bit-for-bit.
    std::vector<double> volatility;
    if (config_.volTargetAnnual > 0.0) {
        indicators::VolForecastConfig vf = config_.volForecast;
        vf.trailingWindow = config_.volWindow;
        volatility = indicators::forecastVolatility(series.close, vf);
    }

    // ------------------------------------------------------------------
    // LONG/SHORT MODE - a separate loop, deliberately.
    //
    // The long/flat loop below is the measuring instrument every published
    // number in this repo came from; threading side-tracking through it to
    // support shorts would put the default path at risk for a feature that
    // is off by default. The cost is ~120 duplicated lines; the benefit is
    // that with allowShort=false not one instruction of the original loop
    // changes, which the standing control (tsmom vt 0.20, --end 1786795200:
    // Sharpe 1.26, 88 trades) verifies after every rebuild.
    // ------------------------------------------------------------------
    if (config_.allowShort) {
        double cash = config_.startingEquity;
        double units = 0.0;          // always >= 0; the side carries the sign
        int side = 0;                // +1 long, -1 short, 0 flat (pre-first-signal)
        PositionContext position;
        double entryBasis = 0.0;     // long: outlay incl fee; short: net proceeds
        double entryNotional = 0.0;  // units * fill at entry, P&L denominator
        double entryFee = 0.0;
        Signal pending = Signal::Hold;
        std::vector<double> closeCurve, lowCurve;
        closeCurve.reserve(series.size());
        lowCurve.reserve(series.size());
        size_t barsInMarket = 0;
        double fractionSum = 0.0;
        size_t fractionCount = 0;

        std::vector<double> volatility;
        if (config_.volTargetAnnual > 0.0) {
            indicators::VolForecastConfig vf = config_.volForecast;
            vf.trailingWindow = config_.volWindow;
            volatility = indicators::forecastVolatility(series.close, vf);
        }
        auto sizeFraction = [&](size_t signalIndex) {
            double cap = std::max(0.0, std::min(1.0, config_.maxPositionFraction));
            if (config_.volTargetAnnual <= 0.0) return cap;
            if (signalIndex >= volatility.size() || std::isnan(volatility[signalIndex]) ||
                volatility[signalIndex] <= 0.0 || report.barsPerYear <= 0.0)
                return 0.0;
            double annualVol = volatility[signalIndex] * std::sqrt(report.barsPerYear);
            if (annualVol <= 0.0) return 0.0;
            return std::max(0.0, std::min(cap, config_.volTargetAnnual / annualVol));
        };
        auto equityNow = [&](double price) {
            return side >= 0 ? cash + units * price : cash - units * price;
        };
        auto closeAt = [&](size_t barIndex, double rawPrice, bool forced) {
            if (side == 0 || units <= 0.0 || rawPrice <= 0.0) return;
            TradeRecord trade;
            trade.entryTime = position.entryTime;
            trade.exitTime = series.timestamp[barIndex];
            trade.entryPrice = position.entryPrice;
            trade.units = units;
            trade.forcedClose = forced;
            trade.shortSide = (side < 0);
            if (side > 0) {
                double fillPrice = rawPrice * (1.0 - config_.slippagePct);
                double proceeds = units * fillPrice;
                double fee = proceeds * config_.feePct;
                cash += proceeds - fee;
                report.totalFees += fee;
                trade.exitPrice = fillPrice;
                trade.fees = entryFee + fee;
                trade.pnlAbs = (proceeds - fee) - entryBasis;
            } else {
                // Covering a short: buy back at the adverse side of the spread.
                double fillPrice = rawPrice * (1.0 + config_.slippagePct);
                double cost = units * fillPrice;
                double fee = cost * config_.feePct;
                cash -= (cost + fee);
                report.totalFees += fee;
                trade.exitPrice = fillPrice;
                trade.fees = entryFee + fee;
                trade.pnlAbs = entryBasis - (cost + fee);
            }
            trade.pnlPct = entryNotional > 0.0 ? trade.pnlAbs / entryNotional * 100.0 : 0.0;
            report.trades.push_back(trade);
            units = 0.0; side = 0; entryBasis = entryFee = entryNotional = 0.0;
            position.close();
        };
        auto openAt = [&](int wantSide, size_t barIndex, double rawPrice, size_t signalIndex) {
            if (side != 0 || rawPrice <= 0.0) return;
            double fraction = sizeFraction(signalIndex);
            if (fraction <= 0.0) return;
            double equity = cash;
            double budget = std::min(cash, equity * fraction);
            if (budget <= 0.0) return;
            if (wantSide > 0) {
                double fillPrice = rawPrice * (1.0 + config_.slippagePct);
                double gross = budget / (1.0 + config_.feePct);
                double fee = gross * config_.feePct;
                units = gross / fillPrice;
                cash -= (gross + fee);
                entryBasis = gross + fee;
                entryFee = fee;
                entryNotional = gross;
                position.open(fillPrice, series.timestamp[barIndex], barIndex);
            } else {
                // Short entry: sell borrowed units at the bid. Notional is
                // capped at equity (1x), mirroring the long side's no-leverage
                // rule; the sale proceeds net of fee are the entry basis.
                double fillPrice = rawPrice * (1.0 - config_.slippagePct);
                double notional = budget;
                double fee = notional * config_.feePct;
                units = notional / fillPrice;
                cash += notional - fee;
                entryBasis = notional - fee;
                entryFee = fee;
                entryNotional = notional;
                position.open(fillPrice, series.timestamp[barIndex], barIndex);
            }
            report.totalFees += entryFee;
            side = wantSide;
            fractionSum += (equity > 0.0 ? budget / equity : 0.0);
            ++fractionCount;
        };
        auto applyTarget = [&](int wantSide, size_t barIndex, double rawPrice, size_t signalIndex) {
            if (side == wantSide) return;
            if (side != 0) closeAt(barIndex, rawPrice, false);
            openAt(wantSide, barIndex, rawPrice, signalIndex);
        };

        for (size_t i = evalStart; i < series.size(); ++i) {
            if (config_.fillTiming == FillTiming::NextOpen && pending != Signal::Hold &&
                i > evalStart && i > 0) {
                applyTarget(pending == Signal::Buy ? +1 : -1, i, series.open[i], i - 1);
                pending = Signal::Hold;
            }
            position.observe(series.close[i]);
            Signal signal = strategy.onBar(series, i, position);
            if (config_.fillTiming == FillTiming::SameClose) {
                if (signal == Signal::Buy) applyTarget(+1, i, series.close[i], i);
                else if (signal == Signal::Sell) applyTarget(-1, i, series.close[i], i);
            } else if ((signal == Signal::Buy && side <= 0) ||
                       (signal == Signal::Sell && side >= 0)) {
                pending = signal;
            }
            // Funding / borrow accrues on OPEN notional, per bar, sign as
            // configured. Charged against cash so it flows through equity,
            // drawdown and every downstream statistic.
            if (side != 0 && report.barsPerYear > 0.0) {
                double rate = (side > 0 ? config_.longCostAnnual : config_.shortCostAnnual)
                              / report.barsPerYear;
                if (rate != 0.0) cash -= units * series.close[i] * rate;
            }
            closeCurve.push_back(equityNow(series.close[i]));
            // Worst intrabar point: the LOW hurts a long, the HIGH hurts a short.
            lowCurve.push_back(side >= 0 ? equityNow(series.low[i])
                                         : equityNow(series.high[i]));
            if (side != 0) ++barsInMarket;
        }
        if (side != 0) {
            closeAt(series.size() - 1, series.close.back(), true);
            closeCurve.back() = cash;
            lowCurve.back() = std::min(lowCurve.back(), cash);
        }

        metrics::ReturnStats returnStats;
        report.strategy = statsFromCurve(closeCurve, lowCurve, config_.startingEquity,
                                         report.barsPerYear, report.spanYears, &returnStats);
        report.benchmark = buyAndHoldStats(series, config_);
        report.excessCagrPct = report.strategy.cagrPct - report.benchmark.cagrPct;
        report.excessSharpe = report.strategy.sharpeAnnual - report.benchmark.sharpeAnnual;
        report.endingEquity = closeCurve.empty() ? config_.startingEquity : closeCurve.back();
        size_t evaluatedBars = series.size() - evalStart;
        report.exposurePct = evaluatedBars > 0
            ? 100.0 * static_cast<double>(barsInMarket) / evaluatedBars : 0.0;
        report.avgPositionFraction = fractionCount > 0 ? fractionSum / fractionCount : 0.0;
        size_t wins = 0;
        for (const auto& t : report.trades) if (t.pnlAbs > 0.0) ++wins;
        report.winRatePct = report.trades.empty() ? 0.0
            : 100.0 * static_cast<double>(wins) / report.trades.size();
        report.numTrades = report.trades.size();
        if (config_.searchTrials > 1) {
            report.searchTrials = config_.searchTrials;
            report.expectedMaxSharpeUnderNull = metrics::expectedMaxSharpeUnderNull(
                config_.searchTrials, returnStats, report.barsPerYear);
            report.deflatedSharpe =
                metrics::deflatedSharpe(returnStats, report.barsPerYear, config_.searchTrials);
        }
        report.equityCurve = std::move(closeCurve);
        return report;
    }

    double cash = config_.startingEquity;
    double units = 0.0;
    PositionContext position;

    // Total cash that left the account to open the current position (price
    // plus entry fee). Trade P&L is measured against this, so the entry fee
    // is charged to the trade instead of vanishing into the equity curve the
    // way it used to - at 268 trades an uncounted 0.15% entry fee is not a
    // rounding error.
    double entryOutlay = 0.0;
    double entryFee = 0.0;

    Signal pending = Signal::Hold;

    std::vector<double> closeCurve, lowCurve;
    closeCurve.reserve(series.size());
    lowCurve.reserve(series.size());

    size_t barsInMarket = 0;
    double fractionSum = 0.0;
    size_t fractionCount = 0;

    // Volatility-targeted sizing: commit only as much of the account as keeps
    // annualized volatility near the target at the entry bar's realized vol.
    // With volTargetAnnual == 0 this collapses to all-in sizing.
    //
    // When targeting IS on and the estimate is unavailable (warm-up, or a
    // degenerate volWindow), the entry is skipped. Sizing all-in on exactly the
    // bars where risk is unmeasurable is the worst possible default, and it
    // used to fire on the first trade of every walk-forward fold.
    auto sizeFraction = [&](size_t signalIndex) {
        double cap = std::max(0.0, std::min(1.0, config_.maxPositionFraction));
        if (config_.volTargetAnnual <= 0.0) return cap;
        if (signalIndex >= volatility.size() || std::isnan(volatility[signalIndex]) ||
            volatility[signalIndex] <= 0.0 || report.barsPerYear <= 0.0) {
            return 0.0; // risk not measurable yet -> do not take the trade
        }
        double annualVol = volatility[signalIndex] * std::sqrt(report.barsPerYear);
        if (annualVol <= 0.0) return 0.0;
        return std::max(0.0, std::min(cap, config_.volTargetAnnual / annualVol));
    };

    auto executeBuy = [&](size_t barIndex, double rawPrice, size_t signalIndex) {
        if (units > 0.0 || rawPrice <= 0.0) return;
        double fillPrice = rawPrice * (1.0 + config_.slippagePct);
        double fraction = sizeFraction(signalIndex);
        if (fraction <= 0.0) return;

        double equity = cash;
        double budget = std::min(cash, equity * fraction);
        double gross = budget / (1.0 + config_.feePct);
        if (gross <= 0.0 || fillPrice <= 0.0) return;

        double bought = gross / fillPrice;
        double fee = gross * config_.feePct;
        cash -= (gross + fee);
        units += bought;
        entryOutlay = gross + fee;
        entryFee = fee;
        report.totalFees += fee;
        // Record the fraction actually DEPLOYED, not the one requested: with
        // the min() above they can differ, and reporting the request would
        // describe a position the engine never took.
        fractionSum += (equity > 0.0 ? budget / equity : 0.0);
        ++fractionCount;
        position.open(fillPrice, series.timestamp[barIndex], barIndex);
    };

    auto executeSell = [&](size_t barIndex, double rawPrice, bool forced) {
        if (units <= 0.0 || rawPrice <= 0.0) return;
        double fillPrice = rawPrice * (1.0 - config_.slippagePct);
        double proceeds = units * fillPrice;
        double fee = proceeds * config_.feePct;
        cash += proceeds - fee;
        report.totalFees += fee;

        TradeRecord trade;
        trade.entryTime = position.entryTime;
        trade.exitTime = series.timestamp[barIndex];
        trade.entryPrice = position.entryPrice;
        trade.exitPrice = fillPrice;
        trade.units = units;
        trade.fees = entryFee + fee;
        trade.pnlAbs = (proceeds - fee) - entryOutlay;
        trade.pnlPct = entryOutlay > 0.0 ? trade.pnlAbs / entryOutlay * 100.0 : 0.0;
        trade.forcedClose = forced;
        report.trades.push_back(trade);

        units = 0.0;
        entryOutlay = 0.0;
        entryFee = 0.0;
        position.close();
    };

    for (size_t i = evalStart; i < series.size(); ++i) {
        // 1. Fill the order the previous bar's signal produced. A signal
        //    derived from bar i-1's close cannot execute before bar i, so
        //    this is the earliest honest fill - and it is exactly what the
        //    live loop does.
        if (config_.fillTiming == FillTiming::NextOpen && pending != Signal::Hold &&
            i > evalStart && i > 0) {
            if (pending == Signal::Buy) executeBuy(i, series.open[i], i - 1);
            else if (pending == Signal::Sell) executeSell(i, series.open[i], false);
            pending = Signal::Hold;
        }

        // 2. Let the open position see this bar before the strategy reasons
        //    about it, so trailing stops are measured against a high that
        //    includes the current close.
        position.observe(series.close[i]);

        // 3. Ask the strategy. It sees only data up to bar i and the position
        //    the engine actually holds.
        Signal signal = strategy.onBar(series, i, position);

        // 4. Execute now (SameClose, the optimistic legacy behaviour) or
        //    queue for the next bar's open (NextOpen, the default).
        if (config_.fillTiming == FillTiming::SameClose) {
            if (signal == Signal::Buy) executeBuy(i, series.close[i], i);
            else if (signal == Signal::Sell) executeSell(i, series.close[i], false);
        } else if ((signal == Signal::Buy && units == 0.0) ||
                   (signal == Signal::Sell && units > 0.0)) {
            pending = signal;
        }

        // 5. Mark to market: at the close for the equity curve, and at the
        //    low for the drawdown measurement.
        closeCurve.push_back(cash + units * series.close[i]);
        lowCurve.push_back(cash + units * series.low[i]);
        if (units > 0.0) ++barsInMarket;
    }

    // A trend strategy is usually long when the data runs out. Closing that
    // position at cost makes the reported total return an amount you could
    // actually have withdrawn, instead of including a free phantom exit.
    if (units > 0.0) {
        executeSell(series.size() - 1, series.close.back(), true);
        closeCurve.back() = cash;
        lowCurve.back() = std::min(lowCurve.back(), cash);
    }

    metrics::ReturnStats returnStats;
    report.strategy = statsFromCurve(closeCurve, lowCurve, config_.startingEquity,
                                     report.barsPerYear, report.spanYears, &returnStats);
    report.benchmark = buyAndHoldStats(series, config_);
    report.excessCagrPct = report.strategy.cagrPct - report.benchmark.cagrPct;
    report.excessSharpe = report.strategy.sharpeAnnual - report.benchmark.sharpeAnnual;

    report.endingEquity = closeCurve.back();
    report.numTrades = report.trades.size();
    size_t wins = 0;
    for (const auto& t : report.trades) if (t.pnlAbs > 0.0) ++wins;
    report.winRatePct = report.trades.empty() ? 0.0
                                              : 100.0 * static_cast<double>(wins) / report.trades.size();
    report.exposurePct = 100.0 * static_cast<double>(barsInMarket) /
                          static_cast<double>(series.size() - evalStart);
    report.avgPositionFraction = fractionCount > 0 ? fractionSum / static_cast<double>(fractionCount) : 0.0;
    report.equityCurve = std::move(closeCurve);

    if (config_.searchTrials > 1) {
        report.expectedMaxSharpeUnderNull =
            metrics::expectedMaxSharpeUnderNull(config_.searchTrials, returnStats, report.barsPerYear);
        report.deflatedSharpe =
            metrics::deflatedSharpe(returnStats, report.barsPerYear, config_.searchTrials);
    }

    return report;
}

std::string BacktestReport::summaryLine() const {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2);
    os << symbol << ": ret " << strategy.totalReturnPct << "% (B&H " << benchmark.totalReturnPct
       << "%), CAGR " << strategy.cagrPct << "% vs " << benchmark.cagrPct
       << "%, Sharpe " << strategy.sharpeAnnual << " +/- " << strategy.sharpeStdErr
       << " (B&H " << benchmark.sharpeAnnual << "), maxDD " << strategy.maxDrawdownPct
       << "%, " << numTrades << " trades";
    return os.str();
}

void BacktestReport::print() const {
    std::cout << "===== Backtest Report =====\n";
    std::cout << "Strategy:        " << strategyName << "\n";
    std::cout << "Symbol:          " << symbol << "\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Span:            " << spanYears << " years, "
              << std::setprecision(1) << barsPerYear << " bars/year"
              << std::setprecision(2) << "\n";
    std::cout << "Starting equity: " << startingEquity << "\n";
    std::cout << "Ending equity:   " << endingEquity << "\n\n";

    std::cout << "                     strategy        buy & hold          excess\n";
    std::cout << "Total return:   " << std::setw(14) << pct(strategy.totalReturnPct)
              << std::setw(18) << pct(benchmark.totalReturnPct)
              << std::setw(16) << pct(strategy.totalReturnPct - benchmark.totalReturnPct) << "\n";
    std::cout << "CAGR:           " << std::setw(14) << pct(strategy.cagrPct)
              << std::setw(18) << pct(benchmark.cagrPct)
              << std::setw(16) << pct(excessCagrPct) << "\n";
    {
        std::ostringstream sharpeCell;
        sharpeCell << std::fixed << std::setprecision(2) << strategy.sharpeAnnual
                   << " +/- " << strategy.sharpeStdErr;
        std::ostringstream bhCell;
        bhCell << std::fixed << std::setprecision(2) << benchmark.sharpeAnnual;
        std::ostringstream exCell;
        exCell << std::fixed << std::setprecision(2) << excessSharpe;
        std::cout << "Sharpe (ann.):  " << std::setw(14) << sharpeCell.str()
                  << std::setw(18) << bhCell.str()
                  << std::setw(16) << exCell.str() << "\n";
    }
    std::cout << "Max drawdown:   " << std::setw(14) << pct(strategy.maxDrawdownPct)
              << std::setw(18) << pct(benchmark.maxDrawdownPct) << "\n";
    std::cout << "  (close-only): " << std::setw(14) << pct(strategy.maxDrawdownClosePct) << "\n";
    std::cout << "Sortino (ann.): " << std::setw(14) << std::setprecision(2) << strategy.sortinoAnnual << "\n\n";

    std::cout << "Num trades:      " << numTrades << "\n";
    std::cout << "Win rate:        " << pct(winRatePct) << "  (net of both fees)\n";
    std::cout << "Time in market:  " << pct(exposurePct) << "\n";
    std::cout << "Fees paid:       " << totalFees << "\n";
    if (avgPositionFraction > 0.0 && avgPositionFraction < 0.999)
        std::cout << "Avg position:    " << pct(avgPositionFraction * 100.0)
                  << " of equity (volatility-targeted)\n";

    if (searchTrials > 1) {
        std::cout << "\nSearch correction (" << searchTrials << " candidates evaluated):\n";
        std::cout << "  Sharpe expected from luck alone: " << std::setprecision(2)
                  << expectedMaxSharpeUnderNull << "\n";
        std::cout << "  Deflated Sharpe (P[true SR > that]): " << std::setprecision(3)
                  << deflatedSharpe << (deflatedSharpe < 0.95 ? "  <- NOT significant\n" : "\n");
    }

    if (numTrades > 0 && numTrades < 30) {
        std::cout << "\nNote: " << numTrades << " trades is few enough that the return distribution is\n"
                     "      dominated by a handful of events. The +/- above measures sampling\n"
                     "      error over the calendar span, NOT over the trade count, so it does\n"
                     "      not capture this - treat the Sharpe as weakly identified.\n";
    }
    std::cout << "============================\n";
}

} // namespace trader
