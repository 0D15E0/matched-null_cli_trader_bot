// causality_check - does every strategy in the registry see only the past?
//
// A strategy is a pure function of (series, index, position), and its
// indicators are precomputed over the whole series in prepare() for speed.
// That optimization is exactly where look-ahead enters: a rolling statistic
// written as a loop over a window is trivial to write with the window running
// forward instead of backward, and nothing at the CLI level would notice. The
// backtest would simply return a better number.
//
// The test is direct. Run prepare() on the full series, and again on a
// truncated prefix. For every bar in the prefix, the two runs must produce
// the SAME signal, because a causal strategy at bar i cannot depend on
// anything after i - and bars after the truncation point are precisely what
// the two runs disagree about. Any mismatch is a bar whose signal changed
// because of data that had not happened yet.
//
// Each bar is probed twice, flat and in a position, because a strategy's exit
// branch is a different code path from its entry branch and only one of them
// is reachable per call. The in-position probe uses a synthetic entry a fixed
// distance back, which is enough to exercise trailing stops, vertical
// barriers and profit targets.
//
//   ./build/experiments/tools/causality_check data/BTC_USDT_14400.ctc [truncFrac]
//
// Exit status is nonzero if any family fails, so it can gate a build.
#include "core/candle_store.h"
#include "strategy/zoo/registry.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace { constexpr size_t kTargetProbes = 1200; }

using namespace trader;

namespace {

const char* signalName(Signal s) {
    switch (s) {
        case Signal::Buy: return "Buy";
        case Signal::Sell: return "Sell";
        default: return "Hold";
    }
}

struct FamilyResult {
    std::string name;
    size_t probes = 0;
    size_t mismatches = 0;
    size_t firstMismatchIndex = 0;
    std::string detail;
};

FamilyResult checkFamily(const zoo::FamilySpec& fam, const CandleSeries& full, size_t cut) {
    FamilyResult res;
    res.name = fam.name;

    std::vector<double> params;
    for (const auto& p : fam.params) params.push_back(p.def);

    CandleSeries prefix;
    prefix.symbol = full.symbol;
    prefix.periodSeconds = full.periodSeconds;
    prefix.reserve(cut);
    for (size_t i = 0; i < cut; ++i) prefix.push(full.at(i));

    auto sFull = fam.make(params);
    auto sCut = fam.make(params);
    if (!sFull || !sCut) { res.detail = "family did not construct"; return res; }
    sFull->prepare(full);
    sCut->prepare(prefix);

    // Skip the first 5% of the prefix: every indicator is NaN there, so a
    // comparison over it would pass trivially and pad the probe count.
    size_t from = cut / 20;

    // Probe on a stride rather than every bar. Look-ahead is not a rare
    // event - an indicator that reads forward reads forward at essentially
    // every bar - so a thousand spread probes catch it as surely as twenty
    // thousand, and the stride is what keeps the O(n * W^3) families (the
    // matrix pencil) from making this test too slow to run.
    size_t stride = std::max<size_t>(1, (cut - from) / kTargetProbes);

    for (size_t i = from; i < cut; i += stride) {
        for (int variant = 0; variant < 2; ++variant) {
            PositionContext pos;
            if (variant == 1) {
                size_t entryIdx = i > 40 ? i - 40 : 0;
                pos.open(full.close[entryIdx], full.timestamp[entryIdx], entryIdx);
                // Replay observe() over the holding period so highestClose /
                // lowestClose are what the engine would have accumulated.
                for (size_t j = entryIdx; j <= i; ++j) pos.observe(full.close[j]);
            }
            Signal a = sFull->onBar(full, i, pos);
            Signal b = sCut->onBar(prefix, i, pos);
            ++res.probes;
            if (a != b) {
                if (res.mismatches == 0) {
                    res.firstMismatchIndex = i;
                    res.detail = std::string(variant == 0 ? "flat" : "in-position") +
                                  " probe: full-series says " + signalName(a) +
                                  ", truncated says " + signalName(b);
                }
                ++res.mismatches;
            }
        }
    }
    return res;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: causality_check <store.ctc> [truncFrac=0.8]\n";
        return 2;
    }
    double frac = argc > 2 ? std::atof(argv[2]) : 0.8;
    if (frac <= 0.1 || frac >= 1.0) {
        std::cerr << "truncFrac must be in (0.1, 1.0)\n";
        return 2;
    }

    CandleStore store(argv[1]);
    if (!store.exists()) { std::cerr << "no such store: " << argv[1] << "\n"; return 2; }
    // Families that read a second instrument resolve it from the directory the
    // tested store lives in, exactly as `--data-dir` would point them there.
    {
        std::string path = argv[1];
        auto slash = path.find_last_of('/');
        zoo::MarketContext::instance().setDataDir(slash == std::string::npos ? "." : path.substr(0, slash));
    }
    CandleSeries full = store.load();
    if (full.size() < 500) { std::cerr << "series too short to test\n"; return 2; }
    size_t cut = static_cast<size_t>(frac * full.size());

    std::cout << "causality check: " << full.symbol << " " << full.periodSeconds << "s, "
              << full.size() << " bars, truncated at " << cut << "\n"
              << "every bar before the cut must give the same signal whether or not the\n"
              << "bars after it exist.\n\n";

    std::cout << std::left << std::setw(22) << "family" << std::right << std::setw(10) << "probes"
              << std::setw(12) << "mismatches" << "  verdict\n";
    std::cout << std::string(70, '-') << "\n";

    int failures = 0;
    for (const auto& fam : zoo::families()) {
        FamilyResult r = checkFamily(fam, full, cut);
        bool bad = r.mismatches > 0 || r.probes == 0;
        if (bad) ++failures;
        std::cout << std::left << std::setw(22) << r.name << std::right << std::setw(10) << r.probes
                  << std::setw(12) << r.mismatches << "  "
                  << (r.probes == 0 ? "NO PROBES" : (r.mismatches ? "LOOK-AHEAD" : "causal"))
                  << "\n";
        if (bad && !r.detail.empty())
            std::cout << std::setw(22) << " " << "    first at bar " << r.firstMismatchIndex
                      << ": " << r.detail << "\n";
        std::cout.flush();
    }

    std::cout << "\n" << (failures == 0
        ? "All families are causal at this truncation point."
        : std::to_string(failures) + " families depend on data after the decision bar.") << "\n";
    return failures == 0 ? 0 : 1;
}
