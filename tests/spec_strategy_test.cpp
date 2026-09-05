// generated_spec executor: leaf semantics and causality for the rule language
// the local research loop composes rules in. Not a registry family, so the
// causality_check tool never exercises it - this is where that check lives.
#include "strategy/tsmom_strategy.h"
#include "strategy/zoo/market_context.h"
#include "strategy/zoo/spec_strategy.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

using namespace trader;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) { if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++failures; } }

CandleSeries synthetic(const std::string& symbol, int64_t t0, int64_t period, size_t n, uint64_t seed, double drift, double vol) {
    CandleSeries s; s.symbol = symbol; s.periodSeconds = period;
    double price = 100.0; uint64_t x = seed;
    for (size_t i = 0; i < n; ++i) {
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        double u = static_cast<double>((x * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
        double g = std::sqrt(-2.0 * std::log(std::max(u, 1e-12))) * std::cos(6.283185307179586 * static_cast<double>((x >> 20) & 0xFFFF) / 65536.0);
        double next = price * std::exp(drift + vol * g);
        Candle c; c.timestamp = t0 + static_cast<int64_t>(i) * period; c.open = price; c.close = next;
        c.high = std::max(price, next) * 1.002; c.low = std::min(price, next) * 0.998; c.volume = 1.0 + (i % 7);
        s.push(c); price = next;
    }
    return s;
}

std::string writeSpec(const std::string& name, const std::string& json) {
    std::string path = std::string("/tmp/spec_strategy_test_") + name + ".json";
    std::ofstream out(path); out << json; return path;
}

void testUnknownFieldAndRanges() {
    bool threw = false;
    try { SpecStrategy s(writeSpec("bad1", R"({"entry":{"type":"close_above_sma","window":50,"threshold":0,"foo":1},"exit":{"type":"red_candle"}})")); }
    catch (const std::runtime_error&) { threw = true; }
    check(threw, "unknown leaf field must throw");
    threw = false;
    try { SpecStrategy s(writeSpec("bad2", R"({"entry":{"type":"close_above_sma","window":50,"threshold":0,"vol_window":30},"exit":{"type":"red_candle"}})")); }
    catch (const std::runtime_error&) { threw = true; }
    check(threw, "vol_window on a non-zscore leaf must throw");
    threw = false;
    try { SpecStrategy s(writeSpec("bad3", R"({"entry":{"type":"vol_rank_above","window":30,"threshold":1.5},"exit":{"type":"red_candle"}})")); }
    catch (const std::runtime_error&) { threw = true; }
    check(threw, "vol_rank threshold outside [0,1] must throw");
}

void testZscoreMatchesTsmom() {
    CandleSeries coin = synthetic("ETH_USDT", 1'500'000'000, 14400, 3000, 7, 0.0002, 0.02);
    SpecStrategy spec(writeSpec("z", R"({"entry":{"type":"zscore_return_above","window":90,"vol_window":30,"threshold":0.5},
                                       "exit":{"type":"zscore_return_below","window":90,"vol_window":30,"threshold":-0.5}})"));
    TsmomParams tp; tp.entryThresholdSigmas = 0.5; tp.maxAnnualVolatility = 1e9;
    TsmomStrategy ts(tp);
    spec.prepare(coin); ts.prepare(coin);
    size_t mism = 0, acted = 0;
    for (size_t i = 0; i < coin.size(); ++i) {
        PositionContext flat, held; held.open(coin.close[0], coin.timestamp[0], 0);
        for (size_t j = 0; j <= i; ++j) held.observe(coin.close[j]);
        Signal t = ts.onBar(coin, i, flat);
        Signal a = spec.onBar(coin, i, flat);   // flat: only the entry tree can fire
        Signal b = spec.onBar(coin, i, held);   // held: only the exit tree can fire
        if (t == Signal::Buy) { ++acted; if (a != Signal::Buy) ++mism; }
        if (t == Signal::Sell) { ++acted; if (b != Signal::Sell) ++mism; }
    }
    check(acted > 200, "zscore test exercised signals");
    check(mism == 0, "zscore leaves reproduce tsmom's sigma-threshold decisions, mismatches=" + std::to_string(mism));
}

void testAtrTrailingStop() {
    CandleSeries coin = synthetic("XRP_USDT", 1'500'000'000, 14400, 800, 3, 0.0, 0.02);
    SpecStrategy spec(writeSpec("atr", R"({"entry":{"type":"atr_trailing_stop","window":20,"threshold":2.5},
                                         "exit":{"type":"atr_trailing_stop","window":20,"threshold":2.5}})"));
    spec.prepare(coin);
    PositionContext flat;
    size_t buys = 0;
    for (size_t i = 0; i < coin.size(); ++i) buys += spec.onBar(coin, i, flat) == Signal::Buy;
    check(buys == 0, "a trailing stop in the entry tree can never open a position");
    // Held from bar 100: the stop must fire exactly when close < highest - 2.5 * ATR.
    auto atr = indicators::atr(coin.high, coin.low, coin.close, 20);
    PositionContext held; held.open(coin.close[100], coin.timestamp[100], 100);
    size_t fired = 0, expected = 0, mism = 0;
    for (size_t i = 100; i < coin.size(); ++i) {
        held.observe(coin.close[i]);
        bool want = zoo::ok(atr[i]) && coin.close[i] < held.highestClose - 2.5 * atr[i];
        bool got = spec.onBar(coin, i, held) == Signal::Sell;
        expected += want; fired += got; mism += want != got;
    }
    check(expected > 0 && fired == expected && mism == 0, "trailing stop fires exactly at highest - k*ATR");
}

void testCausalityAllLeaves() {
    const int64_t period = 14400;
    CandleSeries coin = synthetic("LTC_USDT", 1'500'000'000, period, 4000, 11, 0.0001, 0.025);
    CandleSeries btc = synthetic("BTC_USDT", 1'500'000'000 - 40 * period, period, 4200, 5, 0.0002, 0.015);
    zoo::MarketContext::instance().setSeries("BTC_USDT", period, btc);
    std::string path = writeSpec("all", R"({
      "entry":{"all":[{"type":"zscore_return_above","window":60,"vol_window":30,"threshold":0.3},
                      {"type":"vol_rank_below","window":30,"rank_window":250,"threshold":0.8},
                      {"type":"market_zscore_above","window":90,"threshold":0.0},
                      {"any":[{"type":"close_above_ema","window":50,"threshold":0.5},{"type":"breakout_above","window":20,"threshold":0}]},
                      {"not":{"type":"weekday","day":0}}]},
      "exit":{"any":[{"type":"atr_trailing_stop","window":20,"threshold":3.0},
                     {"type":"zscore_return_below","window":60,"threshold":-0.5},
                     {"type":"relative_volume_above","window":20,"threshold":4.0},
                     {"type":"rsi_below","window":14,"threshold":30}]},
      "max_hold_bars":0})");
    SpecStrategy full(path), cut(path);
    CandleSeries prefix = coin.slice(std::nullopt, coin.timestamp[2999]);
    full.prepare(coin); cut.prepare(prefix);
    size_t mism = 0, nonHold = 0;
    for (size_t i = 0; i < prefix.size(); ++i) {
        for (int variant = 0; variant < 2; ++variant) {
            PositionContext pos;
            if (variant == 1) {
                size_t e = i > 40 ? i - 40 : 0;
                pos.open(coin.close[e], coin.timestamp[e], e);
                for (size_t j = e; j <= i; ++j) pos.observe(coin.close[j]);
            }
            Signal a = full.onBar(coin, i, pos), b = cut.onBar(prefix, i, pos);
            if (a != b) ++mism;
            nonHold += a != Signal::Hold;
        }
    }
    check(mism == 0, "truncating the series changed " + std::to_string(mism) + " signals before the cut");
    check(nonHold > 50, "causality test exercised real signals (" + std::to_string(nonHold) + ")");
}
} // namespace

int main() {
    testUnknownFieldAndRanges();
    testZscoreMatchesTsmom();
    testAtrTrailingStop();
    testCausalityAllLeaves();
    if (failures) { std::cerr << failures << " spec_strategy check(s) failed\n"; return 1; }
    std::cout << "spec_strategy: all checks passed\n";
    return 0;
}
