#pragma once
#include "../darvas_strategy.h"
#include "../fib_ichimoku_strategy.h"
#include "../odiseo_strategy.h"
#include "../pattern_strategy.h"
#include "../pencil_extrapolation_strategy.h"
#include "../pure_ichimoku_strategy.h"
#include "../sma_cross_strategy.h"
#include "../strategy.h"
#include "../tsmom_strategy.h"
#include "afml.h"
#include "breakout.h"
#include "controls.h"
#include "conviction_breakout.h"
#include "ensemble.h"
#include "ensemble_rule.h"
#include "ensemble_tuned.h"
#include "ensemble_asym.h"
#include "dip_reversion.h"
#include "ichimoku_full.h"
#include "ensemble_ls.h"
#include "factor_trend.h"
#include "momo_breakout.h"
#include "lppls.h"
#include "meanrev.h"
#include "regime.h"
#include "trend.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace trader::zoo {

// A uniform, searchable description of every strategy family in the repo.
//
// The point of this indirection: the tournament needs to treat a strategy as
// a NAME plus a vector of numbers with known bounds, so that mutation,
// crossover and random initialization are one implementation rather than one
// per family. Each family therefore publishes its parameters as an ordered
// list of bounded scalars, and a factory that reads that vector positionally.
//
// The bounds are part of the science, not boilerplate. A search allowed to
// set an RSI entry level to 90 or an ATR stop to 0.01 will find candidates
// that "work" by degenerating into something else entirely, and the report
// will still call them by the family's name. Bounds keep each family
// recognizably itself, so a survivor labelled `donchian` is a breakout rule
// and not an accident wearing its name.
struct ParamSpec {
    const char* name;
    double lo, hi, def;
    bool isInt;
};

struct FamilySpec {
    std::string name;
    std::string provenance;   // where the idea comes from
    std::vector<ParamSpec> params;
    std::function<std::unique_ptr<Strategy>(const std::vector<double>&)> make;
    // Rough relative cost of one prepare() call. The tournament uses it only
    // to warn; nothing is silently dropped on account of it.
    double costWeight = 1.0;
};

namespace detail {
inline int    ip(const std::vector<double>& v, size_t i) { return static_cast<int>(std::lround(v[i])); }
inline double dp(const std::vector<double>& v, size_t i) { return v[i]; }
} // namespace detail

inline const std::vector<FamilySpec>& families() {
    using detail::dp;
    using detail::ip;
    static const std::vector<FamilySpec> kFamilies = [] {
        std::vector<FamilySpec> f;

        // ---- families already in this repo -------------------------------
        f.push_back({"odiseo", "repo: Ichimoku + DMI/ADX + ATR trailing stop",
            {{"dmiWindow", 5, 40, 14, true}, {"atrWindow", 5, 40, 14, true},
             {"adxThreshold", 8, 40, 20, false}, {"atrStopMultiple", 1.0, 10.0, 5.0, false},
             {"tenkanWindow", 3, 30, 9, true}, {"kijunWindow", 10, 80, 26, true}},
            [](const std::vector<double>& v) {
                OdiseoParams p;
                p.dmiWindow = ip(v, 0); p.atrWindow = ip(v, 1);
                p.adxThreshold = dp(v, 2); p.atrStopMultiple = dp(v, 3);
                p.tenkanWindow = ip(v, 4); p.kijunWindow = ip(v, 5);
                p.spanBWindow = std::max(p.kijunWindow + 1, 2 * p.kijunWindow);
                return std::make_unique<OdiseoStrategy>(p);
            }});

        f.push_back({"sma_cross", "repo: fast/slow moving-average crossover",
            {{"fast", 3, 60, 10, true}, {"slow", 10, 300, 30, true}},
            [](const std::vector<double>& v) {
                int fast = ip(v, 0), slow = ip(v, 1);
                if (slow <= fast) slow = fast + 1;   // a crossover needs two speeds
                return std::make_unique<SmaCrossStrategy>(fast, slow);
            }});

        f.push_back({"pure_ichimoku", "repo: Ichimoku cloud breakout, unfiltered", {},
            [](const std::vector<double>&) { return std::make_unique<PureIchimokuStrategy>(); }});

        f.push_back({"tsmom", "Moskowitz, Ooi & Pedersen (2012); Lim, Zohren & Roberts (2019)",
            {{"lookbackWindow", 10, 400, 90, true}, {"volWindow", 10, 120, 30, true},
             {"entryThresholdSigmas", 0.0, 2.0, 0.5, false},
             {"maxAnnualVolatility", 0.2, 3.0, 0.6, false}},
            [](const std::vector<double>& v) {
                TsmomParams p;
                p.lookbackWindow = ip(v, 0); p.volWindow = ip(v, 1);
                p.entryThresholdSigmas = dp(v, 2); p.maxAnnualVolatility = dp(v, 3);
                return std::make_unique<TsmomStrategy>(p);
            }});

        f.push_back({"darvas", "Darvas (1960), box breakouts on volume",
            {{"pivotWindow", 3, 30, 10, true}, {"minSwingAtr", 1.0, 8.0, 3.0, false},
             {"maxBoxWidthFrac", 0.05, 0.6, 0.25, false}, {"targetLevel", 0.2, 2.0, 0.618, false}},
            [](const std::vector<double>& v) {
                DarvasParams p;
                p.pivotWindow = ip(v, 0); p.minSwingAtr = dp(v, 1);
                p.maxBoxWidthFrac = dp(v, 2); p.targetLevel = dp(v, 3);
                return std::make_unique<DarvasStrategy>(p);
            }});

        // Cost 500: a matrix-pencil (SVD) fit per bar over a 256-bar window is
        // O(n * W^3), which is ~1000x every other family here. It is left in
        // the registry so `backtest --strategy pencil_extrap` and
        // `tournament --only pencil_extrap` still work, but a default
        // tournament excludes it on cost and says so rather than quietly
        // carrying it.
        f.push_back({"pencil_extrap", "repo: matrix-pencil linear extrapolation", {},
            [](const std::vector<double>&) { return std::make_unique<PencilExtrapolationStrategy>(); },
            500.0});

        f.push_back({"patterns", "repo: Japanese candlestick signal ensemble", {},
            [](const std::vector<double>&) { return std::make_unique<PatternStrategy>(); }});

        f.push_back({"fib_ichimoku", "repo: Fibonacci retracement entries under an Ichimoku trend", {},
            [](const std::vector<double>&) { return std::make_unique<FibIchimokuStrategy>(); }});

        // ---- new families ------------------------------------------------
        f.push_back({"faber_ma", "Faber (2007), J. Wealth Management",
            {{"window", 20, 400, 200, true}, {"bandPct", 0.0, 5.0, 0.0, false}},
            [](const std::vector<double>& v) {
                FaberParams p; p.window = ip(v, 0); p.bandPct = dp(v, 1);
                return std::make_unique<FaberMaStrategy>(p);
            }});

        f.push_back({"kama_trend", "Kaufman (1995), efficiency-ratio adaptive average",
            {{"erWindow", 5, 100, 20, true}, {"fastPeriod", 2, 10, 2, true},
             {"slowPeriod", 10, 100, 30, true}, {"minEfficiency", 0.0, 0.6, 0.0, false}},
            [](const std::vector<double>& v) {
                KamaParams p;
                p.erWindow = ip(v, 0); p.fastPeriod = ip(v, 1);
                p.slowPeriod = std::max(ip(v, 2), p.fastPeriod + 1);
                p.minEfficiency = dp(v, 3);
                return std::make_unique<KamaTrendStrategy>(p);
            }});

        f.push_back({"ehlers_trend", "Ehlers (2004), two-pole SuperSmoother",
            {{"cutoffPeriod", 5, 120, 20, true}, {"slopeLag", 1, 40, 5, true},
             {"volWindow", 20, 200, 60, true}, {"entrySigmas", 0.0, 1.0, 0.15, false}},
            [](const std::vector<double>& v) {
                EhlersParams p;
                p.cutoffPeriod = ip(v, 0); p.slopeLag = ip(v, 1);
                p.volWindow = ip(v, 2); p.entrySigmas = dp(v, 3);
                return std::make_unique<EhlersTrendStrategy>(p);
            }});

        f.push_back({"macd_norm", "Baz, Granger, Harvey, Le Roux & Rattray (2015), SSRN 2695101",
            {{"shortWindow", 3, 120, 24, true}, {"longWindow", 20, 400, 96, true},
             {"priceStdWindow", 20, 250, 63, true}, {"signalStdWindow", 50, 600, 252, true},
             {"entryZ", 0.0, 2.0, 0.2, false}},
            [](const std::vector<double>& v) {
                MacdNormParams p;
                p.shortWindow = ip(v, 0);
                p.longWindow = std::max(ip(v, 1), p.shortWindow + 1);
                p.priceStdWindow = ip(v, 2); p.signalStdWindow = ip(v, 3);
                p.entryZ = dp(v, 4);
                return std::make_unique<MacdNormStrategy>(p);
            }});

        f.push_back({"donchian", "Donchian's 4-week rule; the Turtle system",
            {{"entryWindow", 5, 250, 55, true}, {"exitWindow", 3, 120, 20, true},
             {"atrWindow", 5, 60, 20, true}, {"atrStopMult", 0.0, 8.0, 2.5, false}},
            [](const std::vector<double>& v) {
                DonchianParams p;
                p.entryWindow = ip(v, 0); p.exitWindow = ip(v, 1);
                p.atrWindow = ip(v, 2); p.atrStopMult = dp(v, 3);
                return std::make_unique<DonchianStrategy>(p);
            }});

        f.push_back({"conviction_breakout", "causal range escape + rising Faber regime + momentum confirmation",
            {{"entryWindow", 20, 250, 55, true}, {"exitWindow", 5, 120, 20, true},
             {"trendWindow", 50, 400, 200, true}, {"slopeWindow", 5, 60, 20, true},
             {"momentumWindow", 20, 250, 90, true}, {"momentumThreshold", -0.05, 0.20, 0.0, false},
             {"atrWindow", 5, 60, 20, true}, {"breakoutAtr", 0.0, 2.0, 0.25, false},
             {"stopAtr", 1.0, 8.0, 3.0, false}},
            [](const std::vector<double>& v) {
                ConvictionBreakoutParams p;
                p.entryWindow = ip(v, 0); p.exitWindow = ip(v, 1);
                p.trendWindow = ip(v, 2); p.slopeWindow = ip(v, 3);
                p.momentumWindow = ip(v, 4); p.momentumThreshold = dp(v, 5);
                p.atrWindow = ip(v, 6); p.breakoutAtr = dp(v, 7); p.stopAtr = dp(v, 8);
                return std::make_unique<ConvictionBreakoutStrategy>(p);
            }});

        f.push_back({"squeeze_breakout", "Bollinger bandwidth contraction; volatility clustering (Engle 1982)",
            {{"window", 8, 100, 20, true}, {"numSigma", 1.0, 3.5, 2.0, false},
             {"rankWindow", 60, 750, 250, true}, {"squeezePct", 0.02, 0.6, 0.2, false},
             {"armBars", 1, 40, 10, true}},
            [](const std::vector<double>& v) {
                SqueezeParams p;
                p.window = ip(v, 0); p.numSigma = dp(v, 1); p.rankWindow = ip(v, 2);
                p.squeezePct = dp(v, 3); p.armBars = ip(v, 4);
                return std::make_unique<SqueezeBreakoutStrategy>(p);
            }});

        f.push_back({"rsi_reversal", "Jegadeesh (1990) / Lehmann (1990) short-term reversal; Connors & Alvarez (2008)",
            {{"rsiWindow", 2, 30, 2, true}, {"entryLevel", 2, 45, 10, false},
             {"exitLevel", 50, 95, 70, false}, {"trendWindow", 20, 400, 200, true},
             {"maxHoldBars", 2, 100, 20, true}},
            [](const std::vector<double>& v) {
                Rsi2Params p;
                p.rsiWindow = ip(v, 0); p.entryLevel = dp(v, 1); p.exitLevel = dp(v, 2);
                p.trendWindow = ip(v, 3); p.maxHoldBars = ip(v, 4);
                return std::make_unique<Rsi2Strategy>(p);
            }});

        f.push_back({"ou_score", "Avellaneda & Lee (2010), Quantitative Finance 10(7)",
            {{"window", 20, 250, 60, true}, {"detrendWindow", 10, 250, 60, true},
             {"entryS", 0.5, 3.0, 1.25, false}, {"exitS", 0.0, 2.0, 0.5, false},
             {"maxHalfLifeFrac", 0.1, 1.0, 0.5, false}},
            [](const std::vector<double>& v) {
                OuScoreParams p;
                p.window = ip(v, 0); p.detrendWindow = ip(v, 1);
                p.entryS = dp(v, 2); p.exitS = dp(v, 3); p.maxHalfLifeFrac = dp(v, 4);
                return std::make_unique<OuScoreStrategy>(p);
            }});

        f.push_back({"skew_reversal", "Amaya, Christoffersen, Jacobs & Vasquez (2015), JFE 118(1)",
            {{"window", 6, 120, 20, true}, {"entrySkew", -2.5, 0.0, -0.5, false},
             {"holdBars", 1, 60, 10, true}, {"trendWindow", 0, 300, 0, true}},
            [](const std::vector<double>& v) {
                SkewReversalParams p;
                p.window = ip(v, 0); p.entrySkew = dp(v, 1);
                p.holdBars = ip(v, 2); p.trendWindow = ip(v, 3);
                return std::make_unique<SkewReversalStrategy>(p);
            }});

        f.push_back({"fracdiff", "Lopez de Prado (2018), Advances in Financial ML, ch.5",
            {{"d", 0.05, 0.95, 0.4, false}, {"zWindow", 20, 400, 100, true},
             {"entryZ", 0.0, 2.5, 0.5, false}, {"exitZ", -2.0, 1.0, 0.0, false},
             {"momentum", 0, 1, 1, true}},
            [](const std::vector<double>& v) {
                FracDiffParams p;
                p.d = dp(v, 0); p.zWindow = ip(v, 1);
                p.entryZ = dp(v, 2); p.exitZ = dp(v, 3); p.momentum = ip(v, 4);
                return std::make_unique<FracDiffStrategy>(p);
            }});

        f.push_back({"cusum_tb", "Lopez de Prado (2018), AFML ch.2/3: CUSUM events + triple barrier",
            {{"volWindow", 10, 200, 50, true}, {"hSigmas", 0.5, 6.0, 2.0, false},
             {"ptMult", 0.5, 8.0, 2.0, false}, {"slMult", 0.3, 6.0, 1.5, false},
             {"maxHold", 3, 200, 30, true}},
            [](const std::vector<double>& v) {
                CusumParams p;
                p.volWindow = ip(v, 0); p.hSigmas = dp(v, 1);
                p.ptMult = dp(v, 2); p.slMult = dp(v, 3); p.maxHold = ip(v, 4);
                return std::make_unique<CusumTripleBarrierStrategy>(p);
            }});

        f.push_back({"vol_managed", "Moreira & Muir (2017), Journal of Finance 72(4)",
            {{"volWindow", 5, 120, 20, true}, {"rankWindow", 60, 750, 250, true},
             {"maxVolRank", 0.1, 1.0, 0.7, false}, {"trendWindow", 0, 400, 100, true}},
            [](const std::vector<double>& v) {
                VolManagedParams p;
                p.volWindow = ip(v, 0); p.rankWindow = ip(v, 1);
                p.maxVolRank = dp(v, 2); p.trendWindow = ip(v, 3);
                return std::make_unique<VolManagedStrategy>(p);
            }});

        f.push_back({"vr_switch", "Lo & MacKinlay (1988) variance ratio; Lo (2004) adaptive markets",
            {{"vrWindow", 60, 800, 250, true}, {"vrLag", 2, 40, 5, true},
             {"band", 0.02, 0.6, 0.15, false}, {"trendWindow", 5, 250, 50, true},
             {"mrWindow", 5, 100, 20, true}, {"mrEntryZ", 0.5, 3.5, 1.5, false}},
            [](const std::vector<double>& v) {
                VrSwitchParams p;
                p.vrWindow = ip(v, 0); p.vrLag = ip(v, 1); p.band = dp(v, 2);
                p.trendWindow = ip(v, 3); p.mrWindow = ip(v, 4); p.mrEntryZ = dp(v, 5);
                return std::make_unique<VarianceRatioSwitchStrategy>(p);
            }});

        f.push_back({"lppls_bubble", "Johansen, Ledoit & Sornette (2000); Filimonov & Sornette (2013)",
            {{"window", 120, 600, 250, true}, {"refitEvery", 5, 60, 10, true},
             {"confThreshold", 0.2, 1.0, 0.5, false}, {"trendWindow", 10, 300, 100, true}},
            [](const std::vector<double>& v) {
                LpplsParams p;
                p.window = ip(v, 0); p.refitEvery = ip(v, 1);
                p.confThreshold = dp(v, 2); p.trendWindow = ip(v, 3);
                return std::make_unique<LpplsBubbleStrategy>(p);
            },
            8.0});

        f.push_back({"ensemble_vote", "majority vote of tsmom + faber_ma + donchian at published defaults",
            {{"enterVotes", 1, 3, 2, true}, {"exitVotes", 0, 2, 1, true}},
            [](const std::vector<double>& v) {
                EnsembleParams p;
                p.enterVotes = ip(v, 0); p.exitVotes = ip(v, 1);
                return std::make_unique<EnsembleVoteStrategy>(p);
            }});

        f.push_back({"momo_breakout", "user-supplied spec 2026-08-25: 20-bar breakout + expanding range + green candle, -1.5%/+3% bracket (LONG HALF ONLY; close-evaluated exits)",
            {{"rangeFast", 3, 50, 10, true}, {"rangeSlow", 10, 200, 50, true},
             {"lookback", 5, 100, 20, true}, {"stopPct", 0.3, 10.0, 1.5, false},
             {"tpPct", 0.5, 20.0, 3.0, false}},
            [](const std::vector<double>& v) {
                MomoBreakoutParams p;
                p.rangeFast = ip(v, 0); p.rangeSlow = std::max(ip(v, 1), p.rangeFast + 1);
                p.lookback = ip(v, 2); p.stopPct = dp(v, 3); p.tpPct = dp(v, 4);
                return std::make_unique<MomoBreakoutStrategy>(p);
            }});

        f.push_back({"ensemble_ls", "long/short ensemble: same 3 members; short votes from the reciprocal series. Meant for --long-short (always-in; cannot express cash)",
            {{"enterVotes", 1, 3, 2, true}},
            [](const std::vector<double>& v) {
                EnsembleLsParams p; p.enterVotes = ip(v, 0);
                return std::make_unique<EnsembleLongShortStrategy>(p);
            }});

        f.push_back({"ensemble_rule", "general 3-member vote: entry/stay as 8-bit truth tables over (tsmom,faber,donchian); default = live majority rule",
            {{"entryMask", 1, 254, 232, true}, {"stayMask", 1, 254, 232, true}},
            [](const std::vector<double>& v) {
                EnsembleRuleParams p; p.entryMask = ip(v, 0); p.stayMask = ip(v, 1);
                return std::make_unique<EnsembleRuleStrategy>(p);
            }});

        f.push_back({"ensemble_tuned", "ensemble_vote with member parameters exposed (sensitivity studies); defaults = live book",
            {{"enterVotes", 1, 3, 2, true}, {"exitVotes", 0, 2, 0, true},
             {"faberWindow", 20, 600, 200, true}, {"donEntry", 5, 250, 55, true},
             {"donExit", 3, 120, 20, true}, {"donAtrMult", 0.0, 8.0, 2.5, false},
             {"tsLookback", 10, 400, 90, true}, {"tsThreshold", 0.0, 0.25, 0.05, false}},
            [](const std::vector<double>& v) {
                EnsembleTunedParams p;
                p.enterVotes = ip(v, 0); p.exitVotes = ip(v, 1); p.faberWindow = ip(v, 2);
                p.donEntry = ip(v, 3); p.donExit = ip(v, 4); p.donAtrMult = dp(v, 5);
                p.tsLookback = ip(v, 6); p.tsThreshold = dp(v, 7);
                return std::make_unique<EnsembleTunedStrategy>(p);
            }});

        f.push_back({"ensemble_asym", "asymmetric exit: winners keep the loose vote threshold, losers get a tighter one",
            {{"enterVotes", 1, 3, 2, true}, {"exitWinner", 0, 2, 0, true},
             {"exitLoser", 0, 2, 1, true}, {"lossThreshold", -0.20, 0.20, 0.0, false}},
            [](const std::vector<double>& v) {
                EnsembleAsymParams p;
                p.enterVotes = ip(v, 0); p.exitWinner = ip(v, 1);
                p.exitLoser = ip(v, 2); p.lossThreshold = dp(v, 3);
                return std::make_unique<EnsembleAsymStrategy>(p);
            }});

        f.push_back({"dip_reversion", "buy a sharp multi-sigma drop, exit on a time stop (short-horizon mean reversion, not trend)",
            {{"lookback", 1, 48, 4, true}, {"volWindow", 20, 400, 96, true},
             {"entrySigmas", 1.0, 6.0, 4.0, false}, {"holdBars", 1, 96, 8, true}},
            [](const std::vector<double>& v) {
                DipReversionParams p;
                p.lookback = ip(v, 0); p.volWindow = ip(v, 1);
                p.entrySigmas = dp(v, 2); p.holdBars = ip(v, 3);
                return std::make_unique<DipReversionStrategy>(p);
            }});

        f.push_back({"ichimoku_full", "complete classic Ichimoku: cloud + TK cross + Chikou confirmation, published 9/26/52/26",
            {{"tenkan", 2, 60, 9, true}, {"kijun", 5, 200, 26, true},
             {"spanB", 10, 400, 52, true}, {"displacement", 1, 100, 26, true},
             {"requireChikou", 0, 1, 1, true}},
            [](const std::vector<double>& v) {
                IchimokuFullParams p;
                p.tenkan = ip(v, 0); p.kijun = ip(v, 1); p.spanB = ip(v, 2);
                p.displacement = ip(v, 3); p.requireChikou = ip(v, 4);
                return std::make_unique<IchimokuFullStrategy>(p);
            }});

        f.push_back({"factor_trend", "Ehsani & Linnainmaa (2022) factor momentum: the coin's tsmom z-score blended with BTC_USDT's (the crypto market factor), reference read one bar late for live parity. First cross-series family; needs the BTC store in --data-dir",
            {{"lookback", 10, 400, 90, true}, {"volWindow", 10, 120, 30, true},
             {"factorWeight", 0.0, 1.0, 0.5, false}, {"thresholdZ", 0.0, 2.0, 0.5, false},
             {"refLagBars", 0, 6, 1, true}, {"confirmZ", -9.0, 2.0, -9.0, false}},
            [](const std::vector<double>& v) {
                FactorTrendParams p;
                p.lookback = ip(v, 0); p.volWindow = ip(v, 1);
                p.factorWeight = dp(v, 2); p.thresholdZ = dp(v, 3); p.refLagBars = ip(v, 4);
                p.confirmZ = dp(v, 5);
                return std::make_unique<FactorTrendStrategy>(p);
            }});

        // ---- controls ----------------------------------------------------
        f.push_back({"control_random", "CONTROL: coin-flip entries, fixed hold",
            {{"entryProb", 0.002, 0.2, 0.02, false}, {"holdBars", 2, 200, 20, true},
             {"seed", 1, 100000, 12345, true}},
            [](const std::vector<double>& v) {
                RandomEntryParams p;
                p.entryProb = dp(v, 0); p.holdBars = ip(v, 1); p.seed = ip(v, 2);
                return std::make_unique<RandomEntryStrategy>(p);
            }});

        f.push_back({"control_always_long", "CONTROL: buy first bar, never sell", {},
            [](const std::vector<double>&) { return std::make_unique<AlwaysLongStrategy>(); }});

        return f;
    }();
    return kFamilies;
}

inline const FamilySpec* findFamily(const std::string& name) {
    for (const auto& f : families()) if (f.name == name) return &f;
    return nullptr;
}

} // namespace trader::zoo
