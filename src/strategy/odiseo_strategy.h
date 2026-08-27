#pragma once
#include "strategy.h"
#include "../indicators/indicators.h"
#include <cmath>
#include <algorithm>
#include <string>
#include <cctype>
#include <stdexcept>

namespace trader {

// Port of the legacy "odiseo - hosoda" strategy from strategies.cpp:
// combines Ichimoku (tenkan/kijun/kumo cross), DMI/ADX trend strength,
// and ATR-based volatility filter into a single directional signal.
//
// The legacy version operated on a hand-maintained QList<QList<QPointF>>
// "studies queue" rebuilt via string parsing ("ICHIMOKU 14400 NO_PLOT")
// and matched indices by scanning type strings on every bar. Here all
// indicators are precomputed once in prepare() over contiguous vectors,
// and onBar() is a handful of array lookups - no string parsing, no
// per-bar indicator recomputation.
//
// Entry requires price above the Ichimoku cloud, ADX-confirmed trend
// strength, +DI > -DI, AND (optionally) tenkan > kijun momentum
// confirmation. Exit on either a bearish cloud-cross/DMI flip OR an
// ATR-multiple trailing stop from the highest close since entry,
// whichever comes first.
//
// IMPORTANT: the profile of a good config is *very* different between a
// 24/7, high-volatility crypto pair sampled every 4h and a single
// exchange-hours equity sampled daily. Rather than hard-coding one set of
// numbers (which silently "overfits" whichever instrument was tuned last),
// parameters live in `Params`, with named presets per asset class and an
// auto-detect helper based on the symbol string. Pass an explicit Params
// to override the guess for a given symbol/timeframe.
struct OdiseoParams {
    int dmiWindow = 14;
    int atrWindow = 14;
    double adxThreshold = 20.0;
    double atrStopMultiple = 5.0;
    bool requireTenkanAboveKijun = true;
    int tenkanWindow = 9;
    int kijunWindow = 26;
    int spanBWindow = 52;

    // VOLUME. Off by default so every number previously measured for odiseo
    // remains exactly reproducible.
    //
    // Three distinct roles, because "add volume" is not one idea and the roles
    // fail differently. This repo has now watched four exposure-reducing
    // filters (Hurst twice, the order gate, Fibonacci retracement) improve a
    // measurement and damage trading, always the same way: less time in market
    // AND a lower win rate. Confirm is another such filter and should be
    // treated with that prior. The other two are not filters - one changes when
    // you LEAVE, the other replaces a condition rather than adding one - so
    // they can help without buying exposure back at a worse price.
    //
    //   None    : unchanged odiseo.
    //   Confirm : ENTRY additionally requires relative volume >= volConfirmMult.
    //             A cloud breakout on below-average volume is treated as
    //             unconfirmed. Reduces exposure - the suspect role.
    //   ObvExit : EXIT additionally when OBV is falling over obvWindow bars
    //             while price is not, i.e. the advance has lost participation.
    //             Touches exits only, so entry exposure is unchanged.
    //   AdxOr   : REPLACES the ADX trend-strength gate with "ADX strong OR
    //             volume expanding", on the argument that a real move announces
    //             itself either in directional persistence or in flow. Widens
    //             rather than narrows the entry condition.
    enum class VolumeMode { None, Confirm, ObvExit, AdxOr };
    VolumeMode volumeMode = VolumeMode::None;
    int    volWindow = 20;        // baseline for relative volume
    double volConfirmMult = 1.2;  // Confirm / AdxOr threshold on relative volume
    int    obvWindow = 10;        // lookback for the OBV slope in ObvExit

    // Hand-tuned on ~11y of BTC_USDT 4h candles. Crypto trends run longer
    // and more violently than equities, so this profile uses a higher ADX
    // bar (only trade strong trends) and a wide ATR stop (x5) so normal
    // crypto volatility doesn't shake the position out early.
    //
    // STALE: the return/Sharpe figures this comment used to advertise were
    // produced by the pre-audit engine, which filled at the signal bar's own
    // close, reported a t-statistic labelled "Sharpe", ignored entry fees in
    // trade P&L, and measured drawdown on closes only. They were also tuned
    // against an undisplaced Ichimoku cloud and an SMA-based ATR, both since
    // corrected. Re-run `backtest` for current numbers, and prefer
    // `evolve --wf-folds` over any hand-tuned preset.
    static OdiseoParams cryptoDefault() {
        OdiseoParams p;
        p.dmiWindow = 14; p.atrWindow = 14;
        p.adxThreshold = 20.0; p.atrStopMultiple = 5.0;
        p.requireTenkanAboveKijun = true;
        p.tenkanWindow = 9; p.kijunWindow = 26; p.spanBWindow = 52;
        return p;
    }

    // Hand-tuned on ~10y of ASML.AS daily candles. Daily equity bars are
    // far less noisy bar-to-bar than 4h crypto candles, so a lower ADX bar
    // catches real trends earlier, and a tighter ATR stop (x2) locks in
    // gains / cuts losers faster - it doesn't need x5 of headroom to avoid
    // being stopped out by normal noise the way crypto does.
    //
    // STALE: see cryptoDefault() - the figures previously quoted here came
    // from the pre-audit engine and no longer describe this code.
    static OdiseoParams equityDefault() {
        OdiseoParams p;
        p.dmiWindow = 14; p.atrWindow = 14;
        p.adxThreshold = 15.0; p.atrStopMultiple = 2.0;
        p.requireTenkanAboveKijun = true;
        p.tenkanWindow = 9; p.kijunWindow = 26; p.spanBWindow = 52;
        return p;
    }

    // Very rough heuristic: crypto pairs in this codebase are named like
    // "BTC_USDT" (base_quote, underscore-separated, all-uppercase),
    // whereas equity/index tickers fetched via Yahoo Finance commonly
    // look like "ASML.AS", "AAPL", "^GSPC" (dot-suffixed exchange code,
    // mixed/upper case, no underscore). This is intentionally simple -
    // pass an explicit OdiseoParams if a symbol doesn't fit the pattern.
    static OdiseoParams forSymbol(const std::string& symbol) {
        bool looksCrypto = symbol.find('_') != std::string::npos && symbol.find('.') == std::string::npos;
        return looksCrypto ? cryptoDefault() : equityDefault();
    }

    // Same heuristic as forSymbol(), exposed separately so callers (e.g.
    // the CLI) can report which profile was auto-selected without having
    // to reverse-engineer it from the resulting parameter values.
    static std::string profileNameForSymbol(const std::string& symbol) {
        bool looksCrypto = symbol.find('_') != std::string::npos && symbol.find('.') == std::string::npos;
        return looksCrypto ? "crypto" : "equity";
    }

    // Found by an early `cli_trader evolve` run against BTC_USDT (4h) and
    // ASML.AS (1d) with fitness = min of the two environments' scores.
    //
    // OBSOLETE - KEPT ONLY FOR REPRODUCIBILITY. This genome was selected by
    // maximizing the old fitness function, which was: a t-statistic
    // mislabelled as Sharpe (so it rewarded long samples rather than good
    // risk-adjusted returns, and was not comparable between a 25,000-bar
    // crypto environment and a 2,500-bar equity one), minus a drawdown
    // penalty an order of magnitude too weak to discourage a 75% drawdown,
    // scored on backtests that filled at the signal bar's own close - all on
    // the same data the result was then reported on, with no holdout. Its
    // advertised returns are not reproducible on the corrected engine and
    // should not be treated as evidence of anything. Use
    // `evolve --train-end` or `evolve --wf-folds` and judge the genome on
    // its out-of-sample segment.
    static OdiseoParams evolvedGeneralist() {
        OdiseoParams p;
        p.dmiWindow = 10; p.atrWindow = 27;
        p.adxThreshold = 17.320; p.atrStopMultiple = 7.798;
        p.requireTenkanAboveKijun = true;
        p.tenkanWindow = 11; p.kijunWindow = 27; p.spanBWindow = 74;
        return p;
    }
};

class OdiseoStrategy : public Strategy {
public:
    // Defaults to the crypto profile for backward compatibility with
    // existing call sites; prefer constructing with an explicit
    // OdiseoParams (e.g. via OdiseoParams::forSymbol(symbol)) so the
    // strategy adapts per-instrument instead of always using whichever
    // asset class was tuned most recently.
    explicit OdiseoStrategy(OdiseoParams params = OdiseoParams::cryptoDefault())
        : params_(params) {}

    std::string name() const override { return "odiseo"; }

    void prepare(const CandleSeries& series) override {
        ichi_ = indicators::ichimoku(series.high, series.low,
                                      params_.tenkanWindow, params_.kijunWindow, params_.spanBWindow);
        dmi_  = indicators::dmi(series.high, series.low, series.close, params_.dmiWindow);
        atr_  = indicators::atr(series.high, series.low, series.close, params_.atrWindow);
        if (params_.volumeMode != OdiseoParams::VolumeMode::None) {
            relVol_ = indicators::relativeVolume(series.volume, params_.volWindow);
            obv_ = indicators::onBalanceVolume(series.close, series.volume);
        } else {
            relVol_.clear();
            obv_.clear();
        }
    }

    // Pure function of (series, bar, position): the trailing stop reads its
    // reference high from the caller-owned PositionContext instead of a
    // member the strategy mutates. That member was why this strategy could
    // buy but never sell in live mode - the live loop re-ran prepare() every
    // poll, which reset it, so the exit branch below was unreachable.
    Signal onBar(const CandleSeries& series, size_t i, const PositionContext& pos) override {
        if (i == 0) return Signal::Hold;
        if (std::isnan(ichi_.spanA[i]) || std::isnan(ichi_.spanB[i]) ||
            std::isnan(dmi_.adx[i]) || std::isnan(atr_[i]))
            return Signal::Hold;

        double close = series.close[i];
        double kumoTop = std::max(ichi_.spanA[i], ichi_.spanB[i]);
        double kumoBot = std::min(ichi_.spanA[i], ichi_.spanB[i]);
        bool aboveCloud = close > kumoTop;
        bool belowCloud = close < kumoBot;
        bool trendStrong = dmi_.adx[i] > params_.adxThreshold;
        bool bullish = dmi_.plusDI[i] > dmi_.minusDI[i];
        bool momentumOk = !params_.requireTenkanAboveKijun || (ichi_.tenkan[i] > ichi_.kijun[i]);

        // Volume expansion: NaN (not yet warm) is treated as "no information",
        // which for Confirm means do not enter and for AdxOr means fall back to
        // ADX alone. Silently defaulting an unknown to `true` would make the
        // filter vanish exactly during warm-up.
        // A volume role with no volume series is a programming error, not a
        // "no information" case: left silent it made --volume-mode confirm
        // block every entry and the other roles no-ops, while the report
        // presented that as a measured result. Fail loudly instead.
        if (params_.volumeMode != OdiseoParams::VolumeMode::None && relVol_.empty())
            throw std::logic_error("odiseo: volume role selected but volume "
                                    "indicators were never prepared");
        const bool volKnown = !relVol_.empty() && std::isfinite(relVol_[i]);
        const bool volExpanding = volKnown && relVol_[i] >= params_.volConfirmMult;

        if (params_.volumeMode == OdiseoParams::VolumeMode::AdxOr)
            trendStrong = trendStrong || volExpanding;

        if (pos.inPosition) {
            bool trailingStopHit = params_.atrStopMultiple > 0.0 &&
                (pos.highestClose - close) > params_.atrStopMultiple * atr_[i];
            bool bearishFlip = belowCloud && trendStrong && !bullish;
            bool participationGone = false;
            if (params_.volumeMode == OdiseoParams::VolumeMode::ObvExit &&
                !obv_.empty() && i >= static_cast<size_t>(params_.obvWindow)) {
                size_t j = i - static_cast<size_t>(params_.obvWindow);
                // Price held up or advanced while cumulative signed volume fell:
                // the move is being carried by fewer and fewer participants.
                bool priceHeld = close >= series.close[j];
                bool obvFell = obv_[i] < obv_[j];
                participationGone = priceHeld && obvFell;
            }
            if (trailingStopHit || bearishFlip || participationGone) return Signal::Sell;
            return Signal::Hold;
        }

        bool volumeGateOk = params_.volumeMode != OdiseoParams::VolumeMode::Confirm || volExpanding;
        if (aboveCloud && trendStrong && bullish && momentumOk && volumeGateOk) return Signal::Buy;
        return Signal::Hold;
    }

private:
    OdiseoParams params_;
    indicators::IchimokuResult ichi_;
    indicators::DMIResult dmi_;
    std::vector<double> atr_;
    std::vector<double> relVol_, obv_;
};

} // namespace trader

