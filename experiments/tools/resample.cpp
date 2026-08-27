// resample - aggregate a CTC1 candle store to a COARSER period.
//
// Timestamps are floor-bucketed onto the target-period grid (UTC):
// open = the first bar's open, high = max, low = min, close = the last bar's
// close, volume = sum.
//
// WHY THIS EXISTS instead of pulling each timeframe from the venue. The
// experiment scores the same instrument at several timeframes and then asks
// whether the order statistic explains the differences between them. That
// question is only meaningful if the timeframes are the SAME HISTORY viewed
// at different resolutions. Venues do not guarantee that: a separate 4h pull
// and a separate 15m pull of the same market can start at different dates,
// carry different gap patterns, and disagree bar-for-bar where the venue has
// revised its own history. Deriving the coarse series from the fine one makes
// them identical by construction, so a difference between timeframes is a
// property of the timeframe and not of two independent downloads. This is
// also why derived stores live in data/derived/ and never in data/: only
// data/ holds what a venue actually served.
//
// WHY FLOOR BUCKETING ONTO AN ABSOLUTE GRID, and not "every k bars". Bucket
// starts are exact multiples of the target period since the epoch, so every
// gap between output bars is an exact multiple of the target period too, and
// the aggregate passes CandleStore's spacing validation by construction.
// Counting k bars at a time would instead carry the SOURCE series' gaps into
// the output as fractional-period spacing - one missing 15m bar and the 4h
// series built on top of it is no longer a 4h series - and it would make the
// bucket boundaries depend on where the history happens to start, so two
// stores of the same market resampled from different start dates would land
// on different grids and stop being comparable.
//
// THE SUBTLETY THIS ENCODES: session-clipped markets. An equity venue trades
// ~8.5h a day, so bucketing it to 4h yields two full buckets plus a stub that
// covers only the last half hour of the session. That is what "4h bars" means
// on such a venue everywhere, and it is the right thing to produce - but the
// stub bar's return is drawn from far less wall clock than its neighbours, so
// per-bar return variance is uneven across the day and any per-bar statistic
// computed downstream inherits that unevenness. It is not a bug to fix here;
// it is a property of the market that the consumer has to know about.
//
// THE EDGE BUCKETS ARE PARTIAL, AND THIS TOOL DOES NOT TRIM THEM. Bucketing
// is unconditional, so the first and last output bars are built from however
// many source bars happen to fall inside them - not necessarily a full
// bucket's worth. Measured on the shipped stores: BTC_USDT_900 -> 14400
// produces a final 4h bar aggregated from ONE 15m bar, and an opening 4h bar
// from four, because the 15m history neither starts nor ends on a 4h
// boundary. The pulled BTC_USDT_14400 store correspondingly ends one full
// bucket earlier (25201 derived bars vs 25199 pulled).
//
// The trailing stub is the FORMING-BAR failure mode core/candle_store.h
// warns about, reached by a different route: it is a bar whose period has not
// finished, written as if it had, and an append-only store never revisits it.
// Its close is really an intra-bar price and its volume is a fraction of a
// real bar's, so any statistic dominated by the last bar - a live signal, a
// final-bar return, a volume percentile - reads it wrong. The estimator this
// experiment runs is not one of those (it consumes an autocorrelation over
// thousands of lags, where two edge bars are irrelevant), which is why the
// behaviour is documented here rather than changed: the derived stores and
// the published numbers were produced with it. If you reuse this tool for
// anything that cares about the newest bar, drop the trailing bucket unless
// the source ends exactly at a target-period boundary.
//
// Refuses targets that are not a strict multiple of the source period:
// aggregating 5m to 7m would put source bars in two buckets at once, and
// there is no correct answer for what the resulting open and close are.
#include "core/candle_store.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace trader;

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: resample <in.ctc> <out.ctc> <targetPeriodSeconds>\n";
        return 1;
    }
    const std::string inPath = argv[1], outPath = argv[2];
    const int64_t target = std::stoll(argv[3]);

    CandleStore in(inPath);
    if (!in.isValidStore()) { std::cerr << "not a valid store: " << inPath << "\n"; return 1; }
    CandleSeries s = in.load();
    if (s.empty()) { std::cerr << "empty store\n"; return 1; }
    if (target <= s.periodSeconds || target % s.periodSeconds != 0) {
        std::cerr << "target period " << target << " must be a multiple of source period "
                  << s.periodSeconds << "\n";
        return 1;
    }

    std::vector<Candle> out;
    for (size_t i = 0; i < s.size(); ++i) {
        int64_t bucket = s.timestamp[i] - (s.timestamp[i] % target);
        if (out.empty() || out.back().timestamp != bucket) {
            Candle c;
            c.timestamp = bucket;
            c.open = s.open[i]; c.high = s.high[i]; c.low = s.low[i];
            c.close = s.close[i]; c.volume = s.volume[i];
            out.push_back(c);
        } else {
            Candle& c = out.back();
            c.high = std::max(c.high, s.high[i]);
            c.low = std::min(c.low, s.low[i]);
            c.close = s.close[i];
            c.volume += s.volume[i];
        }
    }

    CandleStore dst(outPath);
    dst.rewrite(s.symbol, target, out, /*strict=*/true);
    std::cout << "resampled " << s.size() << " x " << s.periodSeconds << "s -> "
              << out.size() << " x " << target << "s bars, span "
              << (out.back().timestamp - out.front().timestamp) / 86400.0 << " days -> "
              << outPath << "\n";
    return 0;
}
