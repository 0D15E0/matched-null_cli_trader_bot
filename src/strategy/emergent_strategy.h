#pragma once
#include "strategy.h"
#include "../indicators/indicators.h"
#include <nlohmann/json.hpp>

#include <vector>
#include <string>
#include <cmath>
#include <sstream>
#include <array>

namespace trader {

// EmergentStrategy: unlike OdiseoStrategy (a fixed hand-designed formula
// whose *parameters* are tuned/evolved - see evolution/genetic_optimizer.h),
// here the trading RULES THEMSELVES are the genome. A genome is a variable-
// length list of simple "IF featureA compared-to (featureB or constant)
// THEN contribute weight" rules; at each bar every rule that fires votes
// its (possibly negative) weight into a score, and the strategy goes long
// when the score crosses above a buyThreshold, flat when it crosses below
// a sellThreshold. Which indicators get used, how many rules exist, how
// they're combined, and the entry/exit thresholds are *all* subject to
// mutation/crossover/selection in evolution/strategy_evolver.h - so the
// strategy structure itself emerges from natural selection, not just its
// parameters.
//
// Every feature below is intentionally scale-invariant (an oscillator
// bounded roughly 0-100/0-1, or a ratio centered near 1.0) rather than a
// raw price - this is what lets the SAME genome (and the same constant
// bounds) be meaningfully evaluated against wildly different instruments
// (e.g. BTC_USDT vs. ASML.AS) without one instrument's price scale making
// constants nonsensical for the other.
enum FeatureId {
    kRsi14 = 0,        // 0-100
    kAdx14,            // 0-100
    kPlusDi14,         // 0-100
    kMinusDi14,        // 0-100
    kHurst50,          // ~0-1
    kCloseOverSma10,   // ~0.8-1.2
    kCloseOverSma30,   // ~0.8-1.2
    kSma10OverSma30,   // ~0.8-1.2
    kCloseOverEma12,   // ~0.8-1.2
    kEma12OverEma26,   // ~0.8-1.2
    kCloseOverTenkan,  // ~0.8-1.2
    kCloseOverKijun,   // ~0.8-1.2
    kAtrOverClose,     // ~0-0.1 (volatility as a fraction of price)
    kBollingerPctB,    // ~-0.2-1.2 ((close-lower)/(upper-lower))
    kFeatureCount
};

inline const char* featureName(int id) {
    static const char* names[kFeatureCount] = {
        "rsi14", "adx14", "plusDI14", "minusDI14", "hurst50",
        "close/sma10", "close/sma30", "sma10/sma30",
        "close/ema12", "ema12/ema26", "close/tenkan9", "close/kijun26",
        "atr14/close", "bollinger%B"
    };
    return (id >= 0 && id < kFeatureCount) ? names[id] : "?";
}

// Constant comparison range appropriate to each feature's natural scale -
// used both when randomly generating a rule and when clamping mutations.
inline void constantRangeFor(int featureId, double& lo, double& hi) {
    switch (featureId) {
        case kRsi14: case kAdx14: case kPlusDi14: case kMinusDi14: lo = 0.0; hi = 100.0; return;
        case kHurst50: lo = 0.0; hi = 1.0; return;
        case kAtrOverClose: lo = 0.0; hi = 0.1; return;
        case kBollingerPctB: lo = -0.2; hi = 1.2; return;
        default: lo = 0.8; hi = 1.2; return; // ratio features
    }
}

// One evolvable rule: compares featureA against either featureB or a
// constant, and contributes `weight` to the bar's score if the comparison
// holds. A negative weight means the rule votes for exiting/avoiding a
// long position, not just "for" one - so bearish/regime-filter rules can
// emerge just as readily as bullish entry rules.
struct Rule {
    int featureA = kRsi14;
    bool useConstant = true;
    int featureB = kAdx14;    // ignored if useConstant
    double constant = 50.0;   // ignored if !useConstant
    bool greaterThan = true;  // true: featureA > other; false: featureA < other
    double weight = 1.0;

    std::string describe() const {
        std::ostringstream os;
        os << "IF " << featureName(featureA) << (greaterThan ? " > " : " < ")
           << (useConstant ? std::to_string(constant) : featureName(featureB))
           << " THEN weight " << (weight >= 0 ? "+" : "") << weight;
        return os.str();
    }

    nlohmann::json toJson() const {
        return {{"featureA", featureA}, {"useConstant", useConstant}, {"featureB", featureB},
                {"constant", constant}, {"greaterThan", greaterThan}, {"weight", weight}};
    }
    static Rule fromJson(const nlohmann::json& j) {
        Rule r;
        r.featureA = j.at("featureA").get<int>();
        r.useConstant = j.at("useConstant").get<bool>();
        r.featureB = j.at("featureB").get<int>();
        r.constant = j.at("constant").get<double>();
        r.greaterThan = j.at("greaterThan").get<bool>();
        r.weight = j.at("weight").get<double>();
        return r;
    }
};

// A whole emergent strategy: a rule list plus the two thresholds that
// turn the aggregate score into Buy/Sell/Hold decisions. Rule count is
// variable (bounded by the evolver, see strategy_evolver.h), so the
// evolutionary search explores strategy *structure*, not just numeric
// parameters of a fixed formula.
struct EmergentGenome {
    std::vector<Rule> rules;
    double buyThreshold = 1.0;
    double sellThreshold = -1.0;

    std::string describe() const {
        std::ostringstream os;
        os << rules.size() << " rule(s), buyThreshold=" << buyThreshold
           << " sellThreshold=" << sellThreshold << ":\n";
        for (size_t i = 0; i < rules.size(); ++i)
            os << "  [" << i << "] " << rules[i].describe() << "\n";
        return os.str();
    }

    nlohmann::json toJson() const {
        nlohmann::json j;
        j["buyThreshold"] = buyThreshold;
        j["sellThreshold"] = sellThreshold;
        j["rules"] = nlohmann::json::array();
        for (auto& r : rules) j["rules"].push_back(r.toJson());
        return j;
    }
    static EmergentGenome fromJson(const nlohmann::json& j) {
        EmergentGenome g;
        g.buyThreshold = j.at("buyThreshold").get<double>();
        g.sellThreshold = j.at("sellThreshold").get<double>();
        for (auto& rj : j.at("rules")) g.rules.push_back(Rule::fromJson(rj));
        return g;
    }
};

class EmergentStrategy : public Strategy {
public:
    explicit EmergentStrategy(EmergentGenome genome) : genome_(std::move(genome)) {}

    std::string name() const override { return "emergent"; }

    void prepare(const CandleSeries& series) override {
        size_t n = series.size();
        for (auto& f : features_) f.assign(n, std::nan(""));

        auto rsi = indicators::rsi(series.close, 14);
        auto dmiR = indicators::dmi(series.high, series.low, series.close, 14);
        auto hurst = indicators::hurstExponent(series.close, 50);
        auto sma10 = indicators::sma(series.close, 10);
        auto sma30 = indicators::sma(series.close, 30);
        auto ema12 = indicators::ema(series.close, 12);
        auto ema26 = indicators::ema(series.close, 26);
        auto ichi = indicators::ichimoku(series.high, series.low, 9, 26, 52);
        auto atrV = indicators::atr(series.high, series.low, series.close, 14);
        auto boll = indicators::bollinger(series.close, 20, 2.0);

        features_[kRsi14] = rsi;
        features_[kAdx14] = dmiR.adx;
        features_[kPlusDi14] = dmiR.plusDI;
        features_[kMinusDi14] = dmiR.minusDI;
        features_[kHurst50] = hurst;

        auto ratio = [&](const std::vector<double>& a, const std::vector<double>& b) {
            std::vector<double> out(n, std::nan(""));
            for (size_t i = 0; i < n; ++i) {
                if (i < a.size() && i < b.size() && !std::isnan(a[i]) && !std::isnan(b[i]) && b[i] != 0.0)
                    out[i] = a[i] / b[i];
            }
            return out;
        };
        features_[kCloseOverSma10] = ratio(series.close, sma10);
        features_[kCloseOverSma30] = ratio(series.close, sma30);
        features_[kSma10OverSma30] = ratio(sma10, sma30);
        features_[kCloseOverEma12] = ratio(series.close, ema12);
        features_[kEma12OverEma26] = ratio(ema12, ema26);
        features_[kCloseOverTenkan] = ratio(series.close, ichi.tenkan);
        features_[kCloseOverKijun] = ratio(series.close, ichi.kijun);
        features_[kAtrOverClose] = ratio(atrV, series.close);

        std::vector<double> pctB(n, std::nan(""));
        for (size_t i = 0; i < n; ++i) {
            double range = boll.upper[i] - boll.lower[i];
            if (!std::isnan(range) && range != 0.0)
                pctB[i] = (series.close[i] - boll.lower[i]) / range;
        }
        features_[kBollingerPctB] = pctB;
    }

    Signal onBar(const CandleSeries&, size_t i, const PositionContext&) override {
        double score = 0.0;
        for (const auto& r : genome_.rules) {
            double a = valueAt(r.featureA, i);
            if (std::isnan(a)) continue;
            double b = r.useConstant ? r.constant : valueAt(r.featureB, i);
            if (std::isnan(b)) continue;
            bool fires = r.greaterThan ? (a > b) : (a < b);
            if (fires) score += r.weight;
        }
        if (score >= genome_.buyThreshold) return Signal::Buy;
        if (score <= genome_.sellThreshold) return Signal::Sell;
        return Signal::Hold;
    }

private:
    double valueAt(int featureId, size_t i) const {
        if (featureId < 0 || featureId >= kFeatureCount) return std::nan("");
        const auto& v = features_[featureId];
        return i < v.size() ? v[i] : std::nan("");
    }

    EmergentGenome genome_;
    std::array<std::vector<double>, kFeatureCount> features_;
};

} // namespace trader
