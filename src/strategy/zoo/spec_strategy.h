#pragma once

#include "../strategy.h"
#include "market_context.h"
#include "zoo_common.h"
#include "../../indicators/indicators.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace trader {

// A bounded, causal rule language for generated hypotheses. The model can
// compose known primitives without writing C++ or changing the production
// strategy tree. Every leaf is precomputed over the candle series, and the
// boolean tree is evaluated at one bar, so the replay/live contract remains
// the same as for hand-written strategies.
//
// LEAVES AND THEIR THRESHOLD UNITS (the unit differs per leaf; the supervisor
// tells the model the same table):
//   close_above_sma / close_below_sma / close_above_ema / close_below_ema
//       window; threshold = PERCENT band around the average (0 = the average)
//   breakout_above / breakdown_below
//       window; threshold = PERCENT beyond the prior-window high/low, which
//       EXCLUDES the current bar (zoo::rollingMaxExclusive)
//   return_above / return_below
//       window; threshold = FRACTION (0.03 = +3% over the window)
//   zscore_return_above / zscore_return_below
//       window (lookback), vol_window (default 30); threshold in SIGMA units:
//       trailing return / (per-bar vol * sqrt(window)) - tsmom's statistic
//   rsi_above / rsi_below            window; threshold 0..100
//   relative_volume_above            window; threshold = ratio to prior mean volume
//   vol_rank_above / vol_rank_below  window (vol window), rank_window (default
//       250); threshold 0..1 = percentile of realized vol in its own history
//   market_zscore_above / market_zscore_below
//       the same z-score read on BTC_USDT (the crypto market factor) one bar
//       late, aligned by timestamp through zoo::MarketContext; needs the BTC
//       store in --data-dir, or the traded series is BTC itself
//   atr_trailing_stop                window (ATR), threshold = k; TRUE while a
//       position is open and close < highest close since entry - k * ATR.
//       Reads the engine-owned PositionContext, so it is exit-only: in an
//       entry tree it is always false.
//   weekday                          day 0..6, UTC, Sunday = 0
//   green_candle / red_candle        no fields
// Every precomputed leaf reads bars <= i only; the trailing stop reads the
// position the engine has already updated with bar i's close, exactly like
// DonchianStrategy. tests/spec_strategy_test.cpp checks truncation invariance
// for every leaf, because generated_spec is not a registry family and the
// causality_check tool never sees it.
class SpecStrategy : public Strategy {
public:
    explicit SpecStrategy(const std::string& path) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("generated spec cannot be opened: " + path);
        nlohmann::json spec;
        input >> spec;
        if (!spec.is_object()) throw std::runtime_error("generated spec root must be an object");
        if (!spec.contains("entry") || !spec.contains("exit"))
            throw std::runtime_error("generated spec requires entry and exit rules");
        entry_ = parseNode(spec.at("entry"), 0);
        exit_ = parseNode(spec.at("exit"), 0);
        if (spec.contains("max_hold_bars")) {
            maxHoldBars_ = spec.at("max_hold_bars").get<int>();
            if (maxHoldBars_ < 0 || maxHoldBars_ > 2000)
                throw std::runtime_error("generated spec max_hold_bars must be in [0,2000]");
        }
    }

    std::string name() const override { return "generated_spec"; }

    void prepare(const CandleSeries& s) override {
        prepareNode(*entry_, s);
        prepareNode(*exit_, s);
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& pos) override {
        if (i >= s.size()) return Signal::Hold;
        if (pos.inPosition) {
            if (evalNode(*exit_, i, pos, s)) return Signal::Sell;
            if (maxHoldBars_ > 0 && i >= pos.entryIndex + static_cast<size_t>(maxHoldBars_))
                return Signal::Sell;
        } else if (evalNode(*entry_, i, pos, s)) {
            return Signal::Buy;
        }
        return Signal::Hold;
    }

private:
    enum class LeafKind {
        CloseAboveSma, CloseBelowSma, CloseAboveEma, CloseBelowEma,
        ReturnAbove, ReturnBelow, BreakoutAbove, BreakdownBelow,
        RsiAbove, RsiBelow, RelativeVolumeAbove, Weekday, GreenCandle, RedCandle,
        ZscoreReturnAbove, ZscoreReturnBelow, VolRankAbove, VolRankBelow,
        MarketZscoreAbove, MarketZscoreBelow, AtrTrailingStop,
    };

    struct Node {
        enum class Kind { All, Any, Not, Leaf } kind = Kind::Leaf;
        LeafKind leaf = LeafKind::Weekday;
        int window = 0;
        int volWindow = 30;      // zscore / market_zscore
        int rankWindow = 250;    // vol_rank
        int weekday = 0;
        double threshold = 0.0;
        std::vector<std::unique_ptr<Node>> children;
        std::vector<char> truth;
        std::vector<double> level;   // atr_trailing_stop: k * ATR per bar
    };

    static bool isZscore(LeafKind k) {
        return k == LeafKind::ZscoreReturnAbove || k == LeafKind::ZscoreReturnBelow ||
               k == LeafKind::MarketZscoreAbove || k == LeafKind::MarketZscoreBelow;
    }
    static bool isVolRank(LeafKind k) { return k == LeafKind::VolRankAbove || k == LeafKind::VolRankBelow; }

    // tsmom's statistic: trailing return over `window` bars divided by its
    // expected dispersion, per-bar volatility * sqrt(window). NaN until warm
    // and where the volatility is exactly zero (stale bars).
    static std::vector<double> zscore(const std::vector<double>& close, int window, int volWindow) {
        const size_t n = close.size();
        std::vector<double> z(n, std::nan(""));
        const std::vector<double> vol = indicators::rollingVolatility(close, std::max(2, volWindow));
        const double rootL = std::sqrt(static_cast<double>(window));
        for (size_t i = static_cast<size_t>(window); i < n; ++i) {
            const double base = close[i - static_cast<size_t>(window)];
            if (base <= 0.0 || !zoo::ok(vol[i]) || vol[i] <= 0.0) continue;
            z[i] = (close[i] / base - 1.0) / (vol[i] * rootL);
        }
        return z;
    }

    static std::unique_ptr<Node> parseNode(const nlohmann::json& value, int depth) {
        if (!value.is_object() || depth > 4)
            throw std::runtime_error("generated rule must be an object with depth <= 4");
        auto node = std::make_unique<Node>();
        for (const char* logical : {"all", "any"}) {
            if (!value.contains(logical)) continue;
            if (!value.at(logical).is_array() || value.at(logical).empty() || value.at(logical).size() > 8)
                throw std::runtime_error(std::string("generated ") + logical + " must contain 1..8 rules");
            node->kind = std::string(logical) == "all" ? Node::Kind::All : Node::Kind::Any;
            for (const auto& child : value.at(logical)) node->children.push_back(parseNode(child, depth + 1));
            return node;
        }
        if (value.contains("not")) {
            node->kind = Node::Kind::Not;
            node->children.push_back(parseNode(value.at("not"), depth + 1));
            return node;
        }
        if (!value.contains("type") || !value.at("type").is_string())
            throw std::runtime_error("generated leaf requires string type");
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key() != "type" && it.key() != "window" &&
                it.key() != "threshold" && it.key() != "day" &&
                it.key() != "vol_window" && it.key() != "rank_window")
                throw std::runtime_error("generated leaf has unknown field: " + it.key());
        }
        const std::string type = value.at("type").get<std::string>();
        auto window = [&]() {
            if (!value.contains("window")) throw std::runtime_error("generated leaf requires window");
            int w = value.at("window").get<int>();
            if (w < 2 || w > 600) throw std::runtime_error("generated leaf window must be in [2,600]");
            return w;
        };
        if (type == "close_above_sma") node->leaf = LeafKind::CloseAboveSma;
        else if (type == "close_below_sma") node->leaf = LeafKind::CloseBelowSma;
        else if (type == "close_above_ema") node->leaf = LeafKind::CloseAboveEma;
        else if (type == "close_below_ema") node->leaf = LeafKind::CloseBelowEma;
        else if (type == "return_above") node->leaf = LeafKind::ReturnAbove;
        else if (type == "return_below") node->leaf = LeafKind::ReturnBelow;
        else if (type == "breakout_above") node->leaf = LeafKind::BreakoutAbove;
        else if (type == "breakdown_below") node->leaf = LeafKind::BreakdownBelow;
        else if (type == "rsi_above") node->leaf = LeafKind::RsiAbove;
        else if (type == "rsi_below") node->leaf = LeafKind::RsiBelow;
        else if (type == "relative_volume_above") node->leaf = LeafKind::RelativeVolumeAbove;
        else if (type == "weekday") node->leaf = LeafKind::Weekday;
        else if (type == "green_candle") node->leaf = LeafKind::GreenCandle;
        else if (type == "red_candle") node->leaf = LeafKind::RedCandle;
        else if (type == "zscore_return_above") node->leaf = LeafKind::ZscoreReturnAbove;
        else if (type == "zscore_return_below") node->leaf = LeafKind::ZscoreReturnBelow;
        else if (type == "vol_rank_above") node->leaf = LeafKind::VolRankAbove;
        else if (type == "vol_rank_below") node->leaf = LeafKind::VolRankBelow;
        else if (type == "market_zscore_above") node->leaf = LeafKind::MarketZscoreAbove;
        else if (type == "market_zscore_below") node->leaf = LeafKind::MarketZscoreBelow;
        else if (type == "atr_trailing_stop") node->leaf = LeafKind::AtrTrailingStop;
        else throw std::runtime_error("unknown generated leaf type: " + type);

        // Optional secondary windows belong to specific leaves only, so a
        // stray field cannot silently change a rule's meaning.
        if (value.contains("vol_window")) {
            if (!isZscore(node->leaf)) throw std::runtime_error("vol_window is only valid on zscore leaves");
            node->volWindow = value.at("vol_window").get<int>();
            if (node->volWindow < 2 || node->volWindow > 600)
                throw std::runtime_error("generated leaf vol_window must be in [2,600]");
        }
        if (value.contains("rank_window")) {
            if (!isVolRank(node->leaf)) throw std::runtime_error("rank_window is only valid on vol_rank leaves");
            node->rankWindow = value.at("rank_window").get<int>();
            if (node->rankWindow < 20 || node->rankWindow > 2000)
                throw std::runtime_error("generated leaf rank_window must be in [20,2000]");
        }

        if (node->leaf == LeafKind::Weekday) {
            if (value.size() != 2 || !value.contains("day"))
                throw std::runtime_error("weekday leaf requires only day");
            node->weekday = value.at("day").get<int>();
            if (node->weekday < 0 || node->weekday > 6)
                throw std::runtime_error("generated weekday must use UTC Sunday=0 through Saturday=6");
        } else if (node->leaf == LeafKind::GreenCandle || node->leaf == LeafKind::RedCandle) {
            if (value.size() != 1)
                throw std::runtime_error("candle leaf accepts only type");
        } else {
            node->window = window();
            if (!value.contains("threshold"))
                throw std::runtime_error("generated leaf requires threshold");
            node->threshold = value.at("threshold").get<double>();
            if (!std::isfinite(node->threshold)) throw std::runtime_error("generated threshold must be finite");
            if ((node->leaf == LeafKind::RsiAbove || node->leaf == LeafKind::RsiBelow) &&
                (node->threshold < 0.0 || node->threshold > 100.0))
                throw std::runtime_error("RSI threshold must be in [0,100]");
            if (node->leaf == LeafKind::RelativeVolumeAbove &&
                (node->threshold < 0.0 || node->threshold > 20.0))
                throw std::runtime_error("relative-volume threshold must be in [0,20]");
            if ((node->leaf == LeafKind::ReturnAbove || node->leaf == LeafKind::ReturnBelow) &&
                (node->threshold < -2.0 || node->threshold > 2.0))
                throw std::runtime_error("return threshold must be in [-2,2]");
            if ((node->leaf == LeafKind::CloseAboveSma || node->leaf == LeafKind::CloseBelowSma ||
                 node->leaf == LeafKind::CloseAboveEma || node->leaf == LeafKind::CloseBelowEma ||
                 node->leaf == LeafKind::BreakoutAbove || node->leaf == LeafKind::BreakdownBelow) &&
                (node->threshold < -100.0 || node->threshold > 100.0))
                throw std::runtime_error("price/channel threshold must be in [-100,100]");
            if (isZscore(node->leaf) && (node->threshold < -5.0 || node->threshold > 5.0))
                throw std::runtime_error("z-score threshold is in sigma units and must be in [-5,5]");
            if (isVolRank(node->leaf) && (node->threshold < 0.0 || node->threshold > 1.0))
                throw std::runtime_error("vol_rank threshold is a percentile and must be in [0,1]");
            if (node->leaf == LeafKind::AtrTrailingStop && (node->threshold < 0.5 || node->threshold > 10.0))
                throw std::runtime_error("atr_trailing_stop threshold is an ATR multiple and must be in [0.5,10]");
        }
        return node;
    }

    static void prepareNode(Node& node, const CandleSeries& s) {
        if (node.kind != Node::Kind::Leaf) {
            for (auto& child : node.children) prepareNode(*child, s);
            return;
        }
        const size_t n = s.size();
        node.truth.assign(n, false);
        std::vector<double> values;
        switch (node.leaf) {
        case LeafKind::CloseAboveSma:
        case LeafKind::CloseBelowSma: {
            values = indicators::sma(s.close, node.window);
            for (size_t i = 0; i < n; ++i)
                if (zoo::ok(values[i])) node.truth[i] = node.leaf == LeafKind::CloseAboveSma
                    ? s.close[i] > values[i] * (1.0 + node.threshold / 100.0)
                    : s.close[i] < values[i] * (1.0 - node.threshold / 100.0);
            break;
        }
        case LeafKind::CloseAboveEma:
        case LeafKind::CloseBelowEma: {
            values = indicators::ema(s.close, node.window);
            for (size_t i = 0; i < n; ++i)
                if (zoo::ok(values[i])) node.truth[i] = node.leaf == LeafKind::CloseAboveEma
                    ? s.close[i] > values[i] * (1.0 + node.threshold / 100.0)
                    : s.close[i] < values[i] * (1.0 - node.threshold / 100.0);
            break;
        }
        case LeafKind::ReturnAbove:
        case LeafKind::ReturnBelow:
            for (size_t i = static_cast<size_t>(node.window); i < n; ++i) {
                double prior = s.close[i - static_cast<size_t>(node.window)];
                if (prior > 0.0) {
                    double ret = s.close[i] / prior - 1.0;
                    node.truth[i] = node.leaf == LeafKind::ReturnAbove
                        ? ret > node.threshold : ret < node.threshold;
                }
            }
            break;
        case LeafKind::BreakoutAbove:
        case LeafKind::BreakdownBelow: {
            auto channel = node.leaf == LeafKind::BreakoutAbove
                ? zoo::rollingMaxExclusive(s.high, node.window)
                : zoo::rollingMinExclusive(s.low, node.window);
            for (size_t i = 0; i < n; ++i)
                if (zoo::ok(channel[i])) node.truth[i] = node.leaf == LeafKind::BreakoutAbove
                    ? s.close[i] > channel[i] * (1.0 + node.threshold / 100.0)
                    : s.close[i] < channel[i] * (1.0 - node.threshold / 100.0);
            break;
        }
        case LeafKind::RsiAbove:
        case LeafKind::RsiBelow:
            values = indicators::rsi(s.close, node.window);
            for (size_t i = 0; i < n; ++i)
                if (zoo::ok(values[i])) node.truth[i] = node.leaf == LeafKind::RsiAbove
                    ? values[i] > node.threshold : values[i] < node.threshold;
            break;
        case LeafKind::RelativeVolumeAbove:
            values = indicators::relativeVolume(s.volume, node.window);
            for (size_t i = 0; i < n; ++i)
                if (zoo::ok(values[i])) node.truth[i] = values[i] > node.threshold;
            break;
        case LeafKind::Weekday:
            for (size_t i = 0; i < n; ++i) {
                std::time_t raw = static_cast<std::time_t>(s.timestamp[i]);
                std::tm utc{};
#if defined(_WIN32)
                gmtime_s(&utc, &raw);
#else
                gmtime_r(&raw, &utc);
#endif
                node.truth[i] = utc.tm_wday == node.weekday;
            }
            break;
        case LeafKind::GreenCandle:
            for (size_t i = 0; i < n; ++i) node.truth[i] = s.close[i] > s.open[i];
            break;
        case LeafKind::RedCandle:
            for (size_t i = 0; i < n; ++i) node.truth[i] = s.close[i] < s.open[i];
            break;
        case LeafKind::ZscoreReturnAbove:
        case LeafKind::ZscoreReturnBelow: {
            values = zscore(s.close, node.window, node.volWindow);
            for (size_t i = 0; i < n; ++i)
                if (zoo::ok(values[i])) node.truth[i] = node.leaf == LeafKind::ZscoreReturnAbove
                    ? values[i] > node.threshold : values[i] < node.threshold;
            break;
        }
        case LeafKind::VolRankAbove:
        case LeafKind::VolRankBelow: {
            auto vol = indicators::rollingVolatility(s.close, node.window);
            values = zoo::rollingPercentileRank(vol, node.rankWindow);
            for (size_t i = 0; i < n; ++i)
                if (zoo::ok(values[i])) node.truth[i] = node.leaf == LeafKind::VolRankAbove
                    ? values[i] > node.threshold : values[i] < node.threshold;
            break;
        }
        case LeafKind::MarketZscoreAbove:
        case LeafKind::MarketZscoreBelow: {
            // Same construction and same one-bar lag as factor_trend.h; see
            // market_context.h for why the lag makes live and backtest agree.
            static constexpr const char* kFactor = "BTC_USDT";
            std::shared_ptr<const CandleSeries> ref;
            if (s.symbol == kFactor) ref = std::make_shared<const CandleSeries>(s);
            else ref = zoo::MarketContext::instance().get(kFactor, s.periodSeconds);
            if (!ref)
                throw std::runtime_error(std::string("market_zscore leaf needs the reference store ") +
                                         zoo::MarketContext::instance().dataDir() + "/" + kFactor + "_" +
                                         std::to_string(s.periodSeconds) + ".ctc");
            std::vector<double> refZ = zscore(ref->close, node.window, node.volWindow);
            std::vector<long> at = zoo::alignByTimestamp(s, *ref, 1);
            for (size_t i = 0; i < n; ++i) {
                if (at[i] < 0) continue;
                double z = refZ[static_cast<size_t>(at[i])];
                if (zoo::ok(z)) node.truth[i] = node.leaf == LeafKind::MarketZscoreAbove
                    ? z > node.threshold : z < node.threshold;
            }
            break;
        }
        case LeafKind::AtrTrailingStop: {
            // Position-dependent: only the distance k * ATR is precomputed; the
            // comparison against the high-water close happens in evalNode.
            values = indicators::atr(s.high, s.low, s.close, node.window);
            node.level.assign(n, std::nan(""));
            for (size_t i = 0; i < n; ++i)
                if (zoo::ok(values[i])) node.level[i] = node.threshold * values[i];
            break;
        }
        }
    }

    static bool evalNode(const Node& node, size_t i, const PositionContext& pos, const CandleSeries& s) {
        if (node.kind == Node::Kind::Leaf) {
            if (node.leaf == LeafKind::AtrTrailingStop) {
                if (!pos.inPosition || i >= node.level.size() || !zoo::ok(node.level[i])) return false;
                return s.close[i] < pos.highestClose - node.level[i];
            }
            return i < node.truth.size() && node.truth[i];
        }
        if (node.kind == Node::Kind::Not) return !evalNode(*node.children.front(), i, pos, s);
        if (node.kind == Node::Kind::All) {
            for (const auto& child : node.children) if (!evalNode(*child, i, pos, s)) return false;
            return true;
        }
        for (const auto& child : node.children) if (evalNode(*child, i, pos, s)) return true;
        return false;
    }

    std::unique_ptr<Node> entry_, exit_;
    int maxHoldBars_ = 0;
};

} // namespace trader