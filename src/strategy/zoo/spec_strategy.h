#pragma once

#include "../strategy.h"
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
            if (evalNode(*exit_, i)) return Signal::Sell;
            if (maxHoldBars_ > 0 && i >= pos.entryIndex + static_cast<size_t>(maxHoldBars_))
                return Signal::Sell;
        } else if (evalNode(*entry_, i)) {
            return Signal::Buy;
        }
        return Signal::Hold;
    }

private:
    enum class LeafKind {
        CloseAboveSma, CloseBelowSma, CloseAboveEma, CloseBelowEma,
        ReturnAbove, ReturnBelow, BreakoutAbove, BreakdownBelow,
        RsiAbove, RsiBelow, RelativeVolumeAbove, Weekday, GreenCandle, RedCandle,
    };

    struct Node {
        enum class Kind { All, Any, Not, Leaf } kind = Kind::Leaf;
        LeafKind leaf = LeafKind::Weekday;
        int window = 0;
        int weekday = 0;
        double threshold = 0.0;
        std::vector<std::unique_ptr<Node>> children;
        std::vector<char> truth;
    };

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
                it.key() != "threshold" && it.key() != "day")
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
        else throw std::runtime_error("unknown generated leaf type: " + type);

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
                    ? s.close[i] > values[i] : s.close[i] < values[i];
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
        }
    }

    static bool evalNode(const Node& node, size_t i) {
        if (node.kind == Node::Kind::Leaf) return i < node.truth.size() && node.truth[i];
        if (node.kind == Node::Kind::Not) return !evalNode(*node.children.front(), i);
        if (node.kind == Node::Kind::All) {
            for (const auto& child : node.children) if (!evalNode(*child, i)) return false;
            return true;
        }
        for (const auto& child : node.children) if (evalNode(*child, i)) return true;
        return false;
    }

    std::unique_ptr<Node> entry_, exit_;
    int maxHoldBars_ = 0;
};

} // namespace trader