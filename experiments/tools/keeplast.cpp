// keeplast - copy a CTC1 store keeping only its TAIL, for the matched-window
// controls.
//
// ============================================================================
// THE ARGUMENT HAS TWO MEANINGS AND THE TOOL DECIDES WHICH FROM ITS MAGNITUDE
// ============================================================================
//
//     keeplast in.ctc out.ctc 4096          -> keep the last 4096 BARS
//     keeplast in.ctc out.ctc 1483228800    -> keep every bar at/after that
//                                              EPOCH-SECONDS TIMESTAMP
//
// The cut is at 1e9. Anything below it is a bar count, anything at or above
// it is a Unix timestamp. This is safe rather than clever: 1e9 seconds is
// 2001-09-09, so every timestamp this repo will ever see is above the cut,
// while a store of 1e9 bars is 48 GB of 4h candles - about 456,000 years of
// history. The two ranges cannot collide.
//
// READ THAT TWICE BEFORE USING THE TOOL, because both meanings are load
// bearing and a mistake between them is silent. Passing 1000000 meaning "the
// year 1970-01-12" gets you the last million bars instead, and the resulting
// store is perfectly valid - it just answers a different question than the
// one you asked. Nothing downstream can detect the substitution.
//
// WHY BOTH MODES EXIST. The experiment needs two different kinds of control,
// and they are not interchangeable:
//
//   * MATCHED SAMPLE COUNT (bar mode). The order estimator's lag budget is
//     min(2000, n/4), so a longer store gets a longer autocorrelation and a
//     better-conditioned pencil fit. Comparing a 20,000-bar crypto store
//     against a 2,000-bar equity store therefore compares estimator
//     conditioning as much as it compares markets. Truncating to a common n
//     removes that confound: same estimator, same lag budget, same everything
//     but the instrument.
//
//   * MATCHED CALENDAR WINDOW (timestamp mode). Instruments that trade over
//     different eras are exposed to different macro regimes, and a trend
//     strategy's excess Sharpe is dominated by which regimes it lived
//     through. Cutting every store at one shared start date makes the
//     cross-section a statement about instruments rather than about history.
//     A bar count cannot express this: 4096 bars is a different span of
//     wall-clock on a 24/7 crypto venue than on a session-clipped equity
//     venue, which is exactly the confound this mode removes.
//
// Bar mode clamps: N = 0 or N greater than the store's length keeps the whole
// store rather than erroring, so a sweep can pass one common N across stores
// of different lengths and the short ones simply pass through intact.
// Timestamp mode does NOT clamp - if no bar sits at or after the cut, that is
// an error, because an empty output store is never what the caller wanted and
// rewrite() would refuse it anyway.
//
// The output is written with strict validation: this tool only ever drops a
// PREFIX of an already-valid series, so the tail it keeps must still validate.
// If it does not, the input store was already corrupt and the right outcome
// is a loud failure rather than a second corrupt store next to the first.
#include "core/candle_store.h"
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace trader;

// Below this, the argument is a bar count; at or above it, an epoch second.
// See the header comment - the gap between the two ranges is ~456,000 years
// of 4h bars, so the discrimination is exact, not heuristic.
static constexpr uint64_t kTimestampCutoff = 1000000000ULL;

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: keeplast <in.ctc> <out.ctc> <N|startTimestamp>\n"
                     "  N < 1e9   : keep the last N bars\n"
                     "  N >= 1e9  : keep bars with timestamp >= N (epoch seconds)\n";
        return 1;
    }
    CandleStore in(argv[1]);
    if (!in.isValidStore()) { std::cerr << "not a valid store: " << argv[1] << "\n"; return 1; }
    CandleSeries s = in.load();
    if (s.empty()) { std::cerr << "empty store\n"; return 1; }

    uint64_t arg = std::stoull(argv[3]);
    size_t start = 0;
    if (arg >= kTimestampCutoff) {
        while (start < s.size() && s.timestamp[start] < static_cast<int64_t>(arg)) ++start;
        if (start == s.size()) { std::cerr << "no bars at or after ts " << arg << "\n"; return 1; }
    } else {
        size_t keep = static_cast<size_t>(arg);
        if (keep == 0 || keep > s.size()) keep = s.size();
        start = s.size() - keep;
    }

    std::vector<Candle> out;
    out.reserve(s.size() - start);
    for (size_t i = start; i < s.size(); ++i)
        out.push_back({s.timestamp[i], s.open[i], s.high[i], s.low[i], s.close[i], s.volume[i]});

    CandleStore dst(argv[2]);
    dst.rewrite(s.symbol, s.periodSeconds, out, /*strict=*/true);
    std::cout << "kept last " << out.size() << " of " << s.size() << " bars -> " << argv[2] << "\n";
    return 0;
}
