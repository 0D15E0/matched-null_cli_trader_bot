#pragma once
#include "../../indicators/indicators.h"
#include "../strategy.h"
#include "market_context.h"
#include "zoo_common.h"
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace trader {

// ---------------------------------------------------------------------------
// FACTOR TREND: time-series momentum read through the market factor.
//
// Every other family in this zoo decides about a coin from that coin's own
// candles. Crypto, though, is close to a one-factor market: an altcoin's
// return is roughly beta times the market's plus a large idiosyncratic term,
// with beta above one. The alt's own 15-day return is therefore a NOISY
// estimate of the thing trend following actually feeds on - the persistent,
// market-wide move - and Bitcoin, the least noisy proxy for that factor, is a
// second, partly independent reading of the same quantity. In equities the
// analogous finding is that momentum in individual stocks is largely FACTOR
// momentum (Ehsani & Linnainmaa, "Factor Momentum and the Momentum Factor",
// Journal of Finance 2022); this family is that observation as a rule.
//
// THE RULE. Let z(x) be tsmom's own statistic on series x: the trailing
// `lookback`-bar return divided by its expected dispersion, sigma_bar *
// sqrt(lookback), with sigma_bar the rolling `volWindow`-bar volatility. Then
//
//     blend[i] = (1 - w) * z(coin)[i]  +  w * z(BTC)[at or before i - lag bars]
//
// Buy while blend > +thresholdZ, sell while blend < -thresholdZ, hold in
// between - exactly tsmom's hysteresis, with the same defaults (90 / 30 /
// 0.5 sigma). At w = 0 the family IS tsmom with a sigma threshold and no
// volatility gate (`tsmom --sparams entryThresholdSigmas=0.5,maxAnnualVolatility=3.0`
// reproduces it, see tests/factor_trend_test.cpp); w = 1 trades every coin
// on Bitcoin's trend alone. w is the one new degree of freedom.
//
// WHAT WAS MEASURED BEFORE BUILDING IT (experiments/factor_trend/, signal
// level, dev data 2017-2023, 7 alts pooled, no fees): given a positive own
// trend, next-bar Sharpe is 1.45 when Bitcoin's trend agrees and 0.69 when it
// does not; the blend's Sharpe as a function of w reads 1.48 / 1.51 / 1.64 /
// 1.50 / 1.18 at w = 0 / .25 / .5 / .75 / 1 - a hump, not a spike, and not a
// substitute: the market reading helps up to the point where the coin's own
// information is thrown away. Five of seven coins improve at w = 0.5. The
// portfolio-level consequence is NOT implied by that table, for one reason
// worth stating: sleeves that share a factor signal enter and exit together,
// which raises their correlation and spends some of the diversification the
// book lives on. Whether the per-sleeve gain outruns that cost is what the
// research protocol measures, and nothing here presumes the answer.
//
// THE REFERENCE IS READ ONE BAR LATE ON PURPOSE. Live, each sleeve is its own
// process; when the ETH sleeve evaluates the bar that closed at T, the BTC
// sleeve may not have fetched BTC's bar T yet. Reading BTC at T - 1 bar needs
// the BTC store to be at most one bar stale, which a 60-120s poll loop
// guarantees, and the backtest reads the same lagged bar - so `parity` sees
// the same signal in both. For a 15-day trend statistic four hours of lag is
// immaterial; the alignment is by TIMESTAMP (market_context.h), never by
// index. If the reference store does lag further than that, prepare() says so
// on stderr, because the decision for the newest bar would then be reading an
// older factor value than the backtest did.
//
// A MISSING REFERENCE IS AN ERROR, NOT A FALLBACK. Degrading silently to
// own-only would produce a report labelled factor_trend that measured tsmom.
// The one exception is trading BTC_USDT itself, where the reference IS the
// traded series and is taken from it directly.
// ---------------------------------------------------------------------------
struct FactorTrendParams {
    int    lookback = 90;        // bars; tsmom default (15 days at 4h)
    int    volWindow = 30;       // bars; tsmom default
    double factorWeight = 0.5;   // w: 0 = own trend only, 1 = market trend only
    double thresholdZ = 0.5;     // enter above +thr, exit below -thr, in sigma units
    int    refLagBars = 1;       // read the reference this many bars behind the decision bar
    // ENTRY CONFIRMATION (exploratory, added 2026-09-05 after the blend's
    // engine-level result was seen - so anything measured with it is a SECOND
    // look at the development data, and is recorded as such). A Buy is
    // additionally required to have the reference's z above this level; Sells
    // are never touched, so Bitcoin can keep a coin OUT but never keep it IN.
    // This is the shape the 2017+ conditional table supports: given a positive
    // own trend, bars with Bitcoin's trend down earned Sharpe 0.69 against 1.45
    // with it up, while bars with own trend down were negative regardless of
    // Bitcoin - i.e. the factor's information is in the veto, not the rescue,
    // and the symmetric blend spends half its weight on the half that fails.
    // Below -8 (the default) the confirmation is off and the family is the
    // pure blend; every previously published factor_trend number is unchanged.
    double confirmZ = -9.0;
};

class FactorTrendStrategy : public Strategy {
public:
    static constexpr const char* kReferenceSymbol = "BTC_USDT";

    explicit FactorTrendStrategy(FactorTrendParams p = {}) : p_(p) {
        p_.lookback = std::max(2, p_.lookback);
        p_.volWindow = std::max(2, p_.volWindow);
        p_.factorWeight = std::min(1.0, std::max(0.0, p_.factorWeight));
        p_.refLagBars = std::max(0, p_.refLagBars);
    }

    std::string name() const override { return "factor_trend"; }

    void prepare(const CandleSeries& s) override {
        const size_t n = s.size();
        blend_.assign(n, std::nan(""));
        if (n == 0) return;

        std::vector<double> own = zscore(s.close);

        std::vector<double> mkt(n, std::nan(""));
        const bool needRef = p_.factorWeight > 0.0 || confirmOn();
        if (needRef) {
            std::shared_ptr<const CandleSeries> ref;
            if (s.symbol == kReferenceSymbol) {
                // Trading the factor itself: the reference is this series.
                ref = std::make_shared<const CandleSeries>(s);
            } else {
                ref = zoo::MarketContext::instance().get(kReferenceSymbol, s.periodSeconds);
                if (!ref) {
                    throw std::runtime_error(
                        std::string("factor_trend needs the reference store ") +
                        zoo::MarketContext::instance().dataDir() + "/" + kReferenceSymbol + "_" +
                        std::to_string(s.periodSeconds) + ".ctc for " + s.symbol +
                        " (fetch it, or point --data-dir at where it lives); it will not "
                        "silently fall back to a single-series rule");
                }
                warnIfStale(s, *ref);
            }
            std::vector<double> refZ = zscore(ref->close);
            std::vector<long> at = zoo::alignByTimestamp(s, *ref, p_.refLagBars);
            for (size_t i = 0; i < n; ++i)
                if (at[i] >= 0) mkt[i] = refZ[static_cast<size_t>(at[i])];
        }

        const double w = p_.factorWeight;
        for (size_t i = 0; i < n; ++i) {
            if (w <= 0.0)      blend_[i] = own[i];
            else if (w >= 1.0) blend_[i] = mkt[i];
            else if (zoo::ok(own[i]) && zoo::ok(mkt[i]))
                blend_[i] = (1.0 - w) * own[i] + w * mkt[i];
        }
        // Entry confirmation, precomputed as a per-bar boolean so onBar stays a
        // pure O(1) read. An unmeasured reference (warm-up) does not block: a
        // gate that fires on missing data is a gate with a hidden parameter.
        confirmed_.assign(n, 1);
        if (confirmOn())
            for (size_t i = 0; i < n; ++i)
                confirmed_[i] = (!zoo::ok(mkt[i]) || mkt[i] > p_.confirmZ) ? 1 : 0;
    }

    Signal onBar(const CandleSeries&, size_t i, const PositionContext&) override {
        if (i >= blend_.size() || !zoo::ok(blend_[i])) return Signal::Hold;
        if (blend_[i] > p_.thresholdZ) return confirmed_[i] ? Signal::Buy : Signal::Hold;
        if (blend_[i] < -p_.thresholdZ) return Signal::Sell;   // exits are never blocked
        return Signal::Hold;
    }

private:
    // tsmom's statistic, in sigma units: trailing return over `lookback` bars
    // divided by sigma_bar * sqrt(lookback). NaN until warm, and NaN where the
    // rolling volatility is exactly zero (stale, carried-forward bars) - tsmom
    // would act on any nonzero return there; this family declines to.
    std::vector<double> zscore(const std::vector<double>& close) const {
        const size_t n = close.size();
        std::vector<double> z(n, std::nan(""));
        const std::vector<double> vol = indicators::rollingVolatility(close, p_.volWindow);
        const double rootL = std::sqrt(static_cast<double>(p_.lookback));
        for (size_t i = static_cast<size_t>(p_.lookback); i < n; ++i) {
            const double base = close[i - static_cast<size_t>(p_.lookback)];
            if (base <= 0.0 || !zoo::ok(vol[i]) || vol[i] <= 0.0) continue;
            z[i] = (close[i] / base - 1.0) / (vol[i] * rootL);
        }
        return z;
    }

    void warnIfStale(const CandleSeries& s, const CandleSeries& ref) {
        if (s.empty() || ref.empty()) return;
        // The newest decision needs the reference at s.back - lag bars. Anything
        // beyond one extra bar of slack means a live reference store has not been
        // refreshed and the newest signal reads an older factor than the backtest.
        const int64_t needed = s.timestamp.back() -
                               static_cast<int64_t>(p_.refLagBars) * s.periodSeconds;
        const int64_t gap = needed - ref.timestamp.back();
        if (gap > s.periodSeconds && gap != lastWarnedGap_) {
            std::cerr << "[factor_trend] reference " << kReferenceSymbol << " store ends "
                      << (gap / std::max<int64_t>(1, s.periodSeconds))
                      << " bar(s) before the bar " << s.symbol
                      << " needs; the newest decision reads a stale factor value\n";
            lastWarnedGap_ = gap;
        }
    }

    bool confirmOn() const { return p_.confirmZ > -8.0; }

    FactorTrendParams p_;
    std::vector<double> blend_;
    std::vector<char> confirmed_;
    int64_t lastWarnedGap_ = 0;
};

} // namespace trader
