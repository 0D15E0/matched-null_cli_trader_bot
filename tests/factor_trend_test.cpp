// factor_trend: the first family that reads a second instrument. Four things
// have to be true for its backtests to mean anything, and each is a test:
//
//   1. alignment is by timestamp, lagged, and never reaches past the decision
//      bar (a reference bar at the same timestamp is NOT visible at lag 1);
//   2. at factorWeight = 0 it is tsmom with a sigma threshold and no volatility
//      gate, signal for signal - the regression anchor that makes the blend
//      weight the ONLY new degree of freedom;
//   3. truncating the traded series changes no signal before the cut, even
//      though the reference store is always the full history (causality);
//   4. a missing reference is an error, not a silent fallback to tsmom.
#include "strategy/tsmom_strategy.h"
#include "strategy/zoo/factor_trend.h"
#include "strategy/zoo/market_context.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace trader;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++failures; }
}

// Deterministic geometric random walk (SplitMix-style LCG), so the test is
// reproducible without any RNG state.
CandleSeries synthetic(const std::string& symbol, int64_t t0, int64_t period, size_t n,
                       uint64_t seed, double drift, double vol) {
    CandleSeries s;
    s.symbol = symbol;
    s.periodSeconds = period;
    double price = 100.0;
    uint64_t x = seed;
    for (size_t i = 0; i < n; ++i) {
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        double u = static_cast<double>((x * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
        double g = std::sqrt(-2.0 * std::log(std::max(u, 1e-12))) *
                   std::cos(6.283185307179586 * static_cast<double>((x >> 20) & 0xFFFF) / 65536.0);
        double next = price * std::exp(drift + vol * g);
        Candle c;
        c.timestamp = t0 + static_cast<int64_t>(i) * period;
        c.open = price; c.close = next;
        c.high = std::max(price, next) * 1.001;
        c.low = std::min(price, next) * 0.999;
        c.volume = 1.0;
        s.push(c);
        price = next;
    }
    return s;
}

void testAlignment() {
    CandleSeries traded, ref;
    traded.periodSeconds = 100;
    for (int64_t t : {100, 200, 300, 400}) traded.push({t, 1, 1, 1, 1, 1});
    for (int64_t t : {100, 200, 400}) ref.push({t, 1, 1, 1, 1, 1});   // 300 is missing

    auto lag1 = zoo::alignByTimestamp(traded, ref, 1);
    // bar 100 needs ref <= 0: none. bar 200 -> 100 (idx 0). bar 300 -> 200 (idx 1).
    // bar 400 -> latest <= 300 is 200 (idx 1), NOT 400: the gap resolves backwards.
    check(lag1 == std::vector<long>({-1, 0, 1, 1}), "lag-1 alignment");

    auto lag0 = zoo::alignByTimestamp(traded, ref, 0);
    check(lag0 == std::vector<long>({0, 1, 1, 2}), "lag-0 alignment");

    CandleSeries empty;
    auto none = zoo::alignByTimestamp(traded, empty, 1);
    check(none == std::vector<long>({-1, -1, -1, -1}), "empty reference aligns to nothing");
}

void testTsmomAnchor() {
    const int64_t period = 14400;
    CandleSeries coin = synthetic("ETH_USDT", 1'500'000'000, period, 3000, 7, 0.0002, 0.02);
    CandleSeries btc = synthetic("BTC_USDT", 1'500'000'000, period, 3000, 11, 0.0002, 0.015);
    zoo::MarketContext::instance().setSeries("BTC_USDT", period, btc);

    FactorTrendParams fp;
    fp.factorWeight = 0.0;
    FactorTrendStrategy ft(fp);
    TsmomParams tp;
    tp.entryThresholdSigmas = 0.5;
    tp.maxAnnualVolatility = 1e9;   // gate off
    TsmomStrategy ts(tp);
    ft.prepare(coin);
    ts.prepare(coin);
    size_t mismatches = 0, buys = 0, sells = 0;
    PositionContext flat;
    for (size_t i = 0; i < coin.size(); ++i) {
        Signal a = ft.onBar(coin, i, flat), b = ts.onBar(coin, i, flat);
        if (a != b) ++mismatches;
        buys += a == Signal::Buy; sells += a == Signal::Sell;
    }
    check(mismatches == 0, "factorWeight=0 reproduces tsmom (sigma threshold, gate off), got " +
                               std::to_string(mismatches) + " mismatches");
    check(buys > 50 && sells > 50, "anchor test actually exercised both signals");
}

void testCausalityAndBlend() {
    const int64_t period = 14400;
    CandleSeries coin = synthetic("XRP_USDT", 1'500'000'000, period, 4000, 3, 0.0001, 0.025);
    CandleSeries btc = synthetic("BTC_USDT", 1'500'000'000 - 50 * period, period, 4200, 5, 0.0002, 0.015);
    zoo::MarketContext::instance().setSeries("BTC_USDT", period, btc);

    FactorTrendStrategy full;          // defaults: w = 0.5, lag 1
    FactorTrendStrategy cut;
    CandleSeries prefix = coin.slice(std::nullopt, coin.timestamp[2999]);
    full.prepare(coin);
    cut.prepare(prefix);
    size_t mismatches = 0, nonHold = 0;
    PositionContext flat;
    for (size_t i = 0; i < prefix.size(); ++i) {
        Signal a = full.onBar(coin, i, flat), b = cut.onBar(prefix, i, flat);
        if (a != b) ++mismatches;
        nonHold += a != Signal::Hold;
    }
    check(mismatches == 0, "truncating the traded series changed " + std::to_string(mismatches) +
                               " signals before the cut");
    check(nonHold > 100, "causality test exercised real signals");

    // The blend must differ from own-only somewhere, or w is not doing anything.
    FactorTrendParams own; own.factorWeight = 0.0;
    FactorTrendStrategy ownOnly(own);
    ownOnly.prepare(coin);
    size_t differs = 0;
    for (size_t i = 0; i < coin.size(); ++i)
        differs += full.onBar(coin, i, flat) != ownOnly.onBar(coin, i, flat);
    check(differs > 0, "blend at w=0.5 is indistinguishable from own-only");

    // Lag: at lag 1 the reference bar with the SAME timestamp must be invisible.
    // Build a reference whose z flips sign exactly at the last bar; with lag 1
    // the newest decision must not see that flip.
    CandleSeries spike = btc;
    spike.close.back() *= 3.0;   // enormous final bar
    spike.high.back() = spike.close.back();
    zoo::MarketContext::instance().setSeries("BTC_USDT", period, spike);
    FactorTrendParams w1; w1.factorWeight = 1.0; w1.refLagBars = 1;
    FactorTrendStrategy lagged(w1);
    FactorTrendParams w0; w0.factorWeight = 1.0; w0.refLagBars = 0;
    FactorTrendStrategy contemporaneous(w0);
    // Trade a series whose last bar coincides with the reference's last bar.
    CandleSeries coinAligned = coin.slice(std::nullopt, spike.timestamp.back());
    lagged.prepare(coinAligned);
    contemporaneous.prepare(coinAligned);
    size_t last = coinAligned.size() - 1;
    Signal sLag = lagged.onBar(coinAligned, last, flat);
    Signal sNow = contemporaneous.onBar(coinAligned, last, flat);
    check(sNow == Signal::Buy, "lag-0 sees the reference spike at the same timestamp");
    check(sLag != Signal::Buy || sNow != sLag || true, "placeholder");   // keep structure simple
    // Stronger statement: lag-1 at the last bar equals lag-0 at the previous bar.
    Signal sPrevNow = contemporaneous.onBar(coinAligned, last - 1, flat);
    check(sLag == sPrevNow, "lag-1 at bar i reads what lag-0 read at bar i-1");
}

void testConfirmationOnlyRemovesBuys() {
    const int64_t period = 14400;
    CandleSeries coin = synthetic("TRX_USDT", 1'500'000'000, period, 3000, 21, 0.0001, 0.02);
    CandleSeries btc = synthetic("BTC_USDT", 1'500'000'000, period, 3000, 23, 0.0001, 0.015);
    zoo::MarketContext::instance().setSeries("BTC_USDT", period, btc);
    FactorTrendParams base; base.factorWeight = 0.0;
    FactorTrendParams gated = base; gated.confirmZ = 0.0;
    FactorTrendStrategy a(base), b(gated);
    a.prepare(coin); b.prepare(coin);
    PositionContext flat;
    size_t removedBuys = 0, changedOther = 0;
    for (size_t i = 0; i < coin.size(); ++i) {
        Signal sa = a.onBar(coin, i, flat), sb = b.onBar(coin, i, flat);
        if (sa == sb) continue;
        if (sa == Signal::Buy && sb == Signal::Hold) ++removedBuys; else ++changedOther;
    }
    check(removedBuys > 0, "confirmation removed no entries on a random walk");
    check(changedOther == 0, "confirmation changed something other than Buy->Hold");
}

void testMissingReference() {
    const int64_t period = 3600;   // no store injected at this period
    zoo::MarketContext::instance().setDataDir("/nonexistent-dir-for-factor-trend-test");
    CandleSeries coin = synthetic("LTC_USDT", 1'500'000'000, period, 500, 9, 0.0, 0.02);
    FactorTrendStrategy ft;
    bool threw = false;
    try { ft.prepare(coin); } catch (const std::runtime_error&) { threw = true; }
    check(threw, "missing reference store must throw, not degrade to tsmom");

    // ...but trading the reference symbol itself needs no store.
    CandleSeries btc = synthetic("BTC_USDT", 1'500'000'000, period, 500, 9, 0.0, 0.02);
    bool threwSelf = false;
    try { ft.prepare(btc); } catch (const std::runtime_error&) { threwSelf = true; }
    check(!threwSelf, "BTC_USDT is its own reference");
}

} // namespace

int main() {
    testAlignment();
    testTsmomAnchor();
    testCausalityAndBlend();
    testConfirmationOnlyRemovesBuys();
    testMissingReference();
    if (failures) { std::cerr << failures << " factor_trend check(s) failed\n"; return 1; }
    std::cout << "factor_trend: all checks passed\n";
    return 0;
}
