#pragma once

#include "../strategy.h"
#include "market_context.h"
#include "zoo_common.h"
#include "../../indicators/indicators.h"
#include "../../math/spiral.h"
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
        MemoryOrderAbove, MemoryOrderBelow,
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
    static bool isMemoryOrder(LeafKind k) {
        return k == LeafKind::MemoryOrderAbove || k == LeafKind::MemoryOrderBelow;
    }

    // ROLLING MEMORY ORDER. The logarithmic-spiral estimator (math/spiral.h)
    // recovers the order alpha of a decaying autocorrelation: alpha ~ -1 is an
    // integer order (exponential relaxation, ARMA-like), -1 < alpha < 0 is
    // fractional (power-law memory, ACF ~ k^-(alpha+1)).
    //
    // Three things make this leaf honest rather than a repackaging of the
    // manual (instrument, timeframe) gate in experiments/order_gate:
    //
    //   * CAUSAL. The estimate at bar i uses closes in [i-window+1, i] only.
    //     The earlier gate ranked instruments on FULL-SAMPLE spectra, which is
    //     why its own README says the shortlist is not an out-of-sample result.
    //   * NOT ONE RADIUS. spiral.h is explicit that a single radius is not a
    //     measurement, because every model looks wrong at large radius. The
    //     estimate is the median alpha over the three small radii the gate
    //     experiment's identification statistic uses.
    //   * BOUNDED COST. The estimator is far too expensive per bar, so it is
    //     refit every `window/40` bars (>= 10) and held between refits. The
    //     cadence is DERIVED, not a searchable parameter: one more knob on a
    //     statistic this noisy would be an invitation to overfit.
    //
    // NaN before the first full window, which prepareNode leaves as false.
public:
    // Test-only: the estimator is private, but its distribution has to be
    // measurable to choose thresholds that actually split. Measured on BTC 4h,
    // alpha runs about -0.8..+0.7 with a median near -0.2 (window 400-500),
    // i.e. squarely in the fractional band -1 < alpha < 0.
    static std::vector<double> memoryOrderForTest(const std::vector<double>& c, int w) { return memoryOrder(c, w); }
private:
    static std::vector<double> memoryOrder(const std::vector<double>& close, int window) {
        const size_t n = close.size();
        std::vector<double> out(n, std::numeric_limits<double>::quiet_NaN());
        const size_t w = static_cast<size_t>(window);
        if (n <= w || w < 200) return out;
        // |log return| is the volatility proxy the order-spectrum command reads
        // by default; it is the series whose memory the estimator characterises.
        std::vector<double> absRet(n, 0.0);
        for (size_t i = 1; i < n; ++i)
            absRet[i] = (close[i] > 0.0 && close[i - 1] > 0.0)
                            ? std::fabs(std::log(close[i] / close[i - 1])) : 0.0;
        const size_t stride = std::max<size_t>(10, w / 40);
        const std::vector<double> radii = {0.1, 0.03, 0.01};
        double held = std::numeric_limits<double>::quiet_NaN();
        size_t nextFit = w;
        for (size_t i = w; i < n; ++i) {
            if (i >= nextFit) {
                nextFit = i + stride;
                std::vector<double> win(absRet.begin() + static_cast<long>(i - w + 1),
                                        absRet.begin() + static_cast<long>(i + 1));
                auto acf = mathx::autocorrelation(win, w / 4);
                if (acf.size() >= 32) {
                    std::vector<double> alphas;
                    for (double r : radii) {
                        mathx::SpiralConfig cfg;
                        cfg.radius = r;
                        auto res = mathx::spiralOrder(acf, 1.0, cfg);
                        if (res.ok && std::isfinite(res.alpha)) alphas.push_back(res.alpha);
                    }
                    if (alphas.size() == radii.size()) {
                        std::sort(alphas.begin(), alphas.end());
                        held = alphas[alphas.size() / 2];
                    } else {
                        held = std::numeric_limits<double>::quiet_NaN();
                    }
                }
            }
            out[i] = held;
        }
        return out;
    }

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
            // The estimator needs a long window to see a power-law tail at all;
            // math/spiral.h and the order-spectrum command both refuse fewer
            // than 200 samples, and the ACF needs >= 32 usable lags on top.
            if (isMemoryOrder(node->leaf)) {
                if (w < 300 || w > 2000)
                    throw std::runtime_error("memory_order window must be in [300,2000]");
                return w;
            }
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
        else if (type == "memory_order_above") node->leaf = LeafKind::MemoryOrderAbove;
        else if (type == "memory_order_below") node->leaf = LeafKind::MemoryOrderBelow;
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
            // Measured range on this repo's stores is about -0.8..+1.1; the
            // model-meaningful band is alpha ~ -1 (integer order, exponential
            // relaxation) up through 0 (fractional, power-law memory).
            if (isMemoryOrder(node->leaf) && (node->threshold < -1.5 || node->threshold > 1.0))
                throw std::runtime_error("memory_order threshold is an order and must be in [-1.5,1.0]");
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
        case LeafKind::MemoryOrderAbove:
        case LeafKind::MemoryOrderBelow: {
            values = memoryOrder(s.close, node.window);
            for (size_t i = 0; i < n; ++i)
                if (zoo::ok(values[i])) node.truth[i] = node.leaf == LeafKind::MemoryOrderAbove
                    ? values[i] > node.threshold : values[i] < node.threshold;
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
