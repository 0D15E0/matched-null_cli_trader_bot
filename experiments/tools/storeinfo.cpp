// storeinfo - one line of metadata about one .ctc store:
//
//     <symbol> <periodSeconds> <nbars> <firstTimestamp> <lastTimestamp>
//
// WHY THIS EXISTS. The order-gate experiment drives ~98 (instrument,
// timeframe) stores from Python, and every stage of it needs to know what is
// actually inside a store before deciding what to do with it: which stores
// share a symbol, how many bars a resample produced, where the matched-window
// truncations have to start. The alternative is a Python reader for CTC1, and
// that is a SECOND implementation of the on-disk format. Two readers of an
// append-only binary format drift - and they drift silently, because the
// format has no per-record framing: a file short by 17 bytes still parses,
// just with every record after the tear shifted. Asking the real CandleStore
// keeps exactly one parser in the repo, and inherits its torn-write and
// header checks for free.
//
// WHAT THE OUTPUT IS FOR. Space-separated, one line, no header row, no units,
// no thousands separators - it is meant for `read`/`split()`, not for humans.
// Timestamps are raw epoch seconds for the same reason: the experiment
// compares them arithmetically to build matched windows, and any date
// formatting here would just have to be undone downstream.
//
// The store is read through isValidStore() rather than exists(), because
// "there is a file" and "there is a readable CTC1 header" are different
// questions and only the second one licenses reading records (see
// core/candle_store.h). A zero-length or headerless file is an error here,
// not an empty result: silently printing "0 bars" for a store the fetch
// stage failed to write would let a broken pull propagate into the sweep as
// a legitimately-empty instrument.
#include "core/candle_store.h"
#include <iostream>

using namespace trader;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: storeinfo <store.ctc>\n"
                     "prints: <symbol> <periodSeconds> <nbars> <firstTs> <lastTs>\n";
        return 1;
    }
    CandleStore in(argv[1]);
    if (!in.isValidStore()) { std::cerr << "invalid store: " << argv[1] << "\n"; return 1; }
    CandleSeries s = in.load();
    if (s.empty()) { std::cerr << "empty store\n"; return 1; }
    std::cout << s.symbol << " " << s.periodSeconds << " " << s.size() << " "
              << s.timestamp.front() << " " << s.timestamp.back() << "\n";
    return 0;
}
