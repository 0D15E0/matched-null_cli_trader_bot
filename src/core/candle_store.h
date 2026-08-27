#pragma once
#include "candle.h"
#include <string>
#include <vector>
#include <optional>
#include <utility>
#include <cstdint>

namespace trader {

// A single problem found in a candle series.
//
// `kind` is a stable machine-readable tag so callers can apply policy per
// class of problem (CandleStore::append refuses to write on a structural
// defect but only warns about "zero-volume"/"outlier", because both occur in
// genuine market data); `detail` is the human-readable explanation.
//
// Kinds:
//   "nonmonotonic" - timestamp went backwards
//   "duplicate"    - the same timestamp appears twice
//   "spacing"      - the gap between two bars is not a multiple of the period
//   "ohlc"         - broken bar invariants (high<low, non-positive price, NaN)
//   "outlier"      - implausible one-bar close-to-close return
//   "zero-volume"  - the bar traded nothing (informational)
//   "rebased"      - stored history uses a different price basis than a fresh
//                    fetch, with one consistent ratio: the stock-split signature
//   "mismatch"     - stored and fresh candles disagree with no consistent ratio
struct DataIssue {
    std::string kind;
    int64_t timestamp = 0;
    std::string detail;
};

// Structural validation of a candle vector.
//
// Flags: non-monotonic and duplicate timestamps, OHLC invariant violations
// (high < low, high < max(open,close), low > min(open,close)), non-positive
// or non-finite prices, zero/negative volume, one-bar |close return| greater
// than `maxAbsBarReturn`, and bar spacing that is not an integer multiple of
// `periodSeconds`.
//
// NOTE on spacing: *missing* bars are not an error. Weekends, holidays and
// exchange outages are normal, so a 5-day gap in a daily equity series is
// perfectly valid - what is not valid is a gap that isn't a whole number of
// periods, because that means the series mixes timeframes or has corrupt
// timestamps. Daily-and-longer periods additionally get +/-1h of slack, since
// Yahoo stamps daily bars at the session open in local time and that shifts by
// an hour across DST (verified: the shipped equity stores contain 262800s and
// 255600s gaps, i.e. 3 days +/- 1 hour).
std::vector<DataIssue> validateCandles(const std::vector<Candle>& candles, int64_t periodSeconds,
                                       double maxAbsBarReturn = 0.5);

// Same checks, applied to an already-loaded series (uses series.periodSeconds).
std::vector<DataIssue> validateSeries(const CandleSeries& series, double maxAbsBarReturn = 0.5);

// Binary, append-only, columnar-ish flat file format for candle history.
//
// Why not the legacy format (one CSV-like line per candle in a QFile,
// re-parsed top-to-bottom with QTextStream on every read)?
//  - Legacy: O(n) text parsing on every access, per-line QString allocations,
//    fragile "last timestamp" bootstrap read from a *separate* file.
//  - New: fixed-size binary header + fixed-size binary records. Reading is a
//    single sequential read into pre-reserved vectors (or can be mmap'ed).
//    Appending new candles is O(new candles), not O(total candles).
//    Random access / binary search by timestamp is O(log n) instead of O(n).
//
// File layout (unchanged since CTC1 - existing data files keep working):
//   [Header]
//     magic       (4 bytes)  = "CTC1"
//     periodSecs  (int64)
//     symbolLen   (uint32)
//     symbol      (symbolLen bytes)
//   [Records] (repeated, fixed size = 48 bytes each)
//     int64  timestamp
//     double open, high, low, close, volume
//
// Records are guaranteed sorted ascending by timestamp (enforced on append).
//
// Append-only storage is fast, but it has three failure modes that silently
// corrupt history, so the class defends against all of them:
//  - A *forming* bar written once is frozen forever, because later fetches
//    filter on `timestamp <= lastTimestamp()` and never revisit it. The data
//    sources are responsible for not handing us one; append() additionally
//    validates what it is given.
//  - A provider that re-bases its whole history (Yahoo does this on every
//    split and dividend) leaves the store holding two incompatible price
//    bases glued together by a fake crash bar. compareOverlap() detects that
//    from the re-fetched overlap, and rewrite() repairs it atomically.
//  - A *torn write* (SIGKILL/power loss mid-append) leaves a trailing partial
//    record. Nothing in the format marks record boundaries - load() and
//    lastTimestamp() derive the record count by integer division from the end
//    of the header - so a file that is short by 17 bytes silently reinterprets
//    every following append as history. Both writers therefore refuse to
//    touch a file whose data region is not an exact multiple of the record
//    size, and write their payload with a single write()+fsync so the window
//    in which that can happen at all is as small as the OS allows.
class CandleStore {
public:
    explicit CandleStore(std::string filePath);

    // Loads full series into memory (fast sequential binary read).
    CandleSeries load() const;

    // Returns the last stored timestamp, or std::nullopt if the file
    // doesn't exist / is empty. Used to resume incremental downloads
    // instead of re-downloading full history (mirrors legacy "resume"
    // behavior, but reads it from the same file instead of a sidecar).
    std::optional<int64_t> lastTimestamp() const;

    // Header only: {symbol, periodSeconds}. Cheap (no record scan) - use it
    // to check what a store on disk actually holds before writing to it.
    std::optional<std::pair<std::string, int64_t>> header() const;

    // True when a file is present at path() and is not zero-length. Says
    // NOTHING about the contents: a truncated or junk file passes. It answers
    // "is there something here to read?" for CLI guards ("run fetch first"),
    // which is all its callers want; use isValidStore() before deciding to
    // write. The distinction is load-bearing - append() used to key header
    // creation off an exists() that returned true for a 0-byte file, and so
    // produced a headerless store.
    bool exists() const;

    // True when the file starts with a readable CTC1 header. This is the
    // question a writer needs answered ("can I append records to this, or
    // must I lay down a header first?").
    bool isValidStore() const;

    const std::string& path() const { return path_; }

    // Appends only the candles strictly newer than lastTimestamp(), after
    // validating that subset (validateCandles) plus the seam where it meets
    // stored history. Creates file + header if there is no valid store yet.
    // Safe to call repeatedly.
    //
    // Only the candles that will actually be written are validated. The
    // discarded older ones are the deliberate re-fetch overlap, and auditing
    // those is compareOverlap()'s job - validating them here meant one bad
    // bar anywhere in a re-fetched history blocked every future append
    // forever, over a bar nobody was going to store.
    //
    // If `strict` and validation reports a structural issue, throws
    // std::runtime_error naming the first few issues and writes NOTHING -
    // a half-written bad batch is worse than a failed fetch, because the
    // append-only design gives no way to take it back. "zero-volume" and
    // "outlier" are reported to stderr but allowed through: crypto genuinely
    // has zero-volume bars in thin early history (498 of them in the shipped
    // BTC_USDT store) and real markets do gap more than 50% in one bar.
    //
    // `strict` governs *candle* validation only. Defects in the file itself -
    // a non-empty file with no CTC1 header, a header whose period disagrees,
    // a data region that is not a whole number of records - always throw,
    // because there is no such thing as a tolerable way to append to a file
    // whose record boundaries are unknown.
    //
    // Returns the number of candles actually appended.
    size_t append(const std::string& symbol, int64_t periodSeconds,
                  const std::vector<Candle>& candles, bool strict = true);

    // Atomically replaces the whole file (write to path()+".tmp", fsync,
    // std::filesystem::rename). Used to repair a store whose history was
    // re-based (e.g. a stock split) or that accumulated partial bars.
    //
    // With `strict` (the default) the replacement history is validated first,
    // with exactly the same fatal/non-fatal split as append(): structural
    // issues throw and nothing is written, "zero-volume"/"outlier" only warn.
    // Repair is the one path that writes venue data the append checks never
    // saw, so unvalidated by default would mean the sloppiest write in the
    // system is the one used to fix corruption. Strict also refuses an empty
    // `candles`, which would silently erase the store.
    //
    // Pass strict=false for a deliberate surgical rewrite (truncating a
    // partial tail, dropping a known-bad bar) where the caller has already
    // decided what "repaired" means.
    void rewrite(const std::string& symbol, int64_t periodSeconds,
                 const std::vector<Candle>& candles, bool strict = true);

    // Compares `fresh` against stored candles at the SAME timestamps.
    // Returns one DataIssue per disagreement beyond tolerancePct (relative).
    //
    // kind=="rebased" is the corporate-action signature: one multiplicative
    // ratio, meaningfully different from 1.0, explains nearly every
    // disagreeing bar. kind=="mismatch" is everything else, including the
    // common case of a venue revising a handful of closes by a few basis
    // points. See the thresholds in the implementation for why "nearly" and
    // "meaningfully" are what they are. Empty result means the overlap agrees
    // and the store is safe to append to.
    std::vector<DataIssue> compareOverlap(const std::vector<Candle>& fresh,
                                          double tolerancePct = 0.001) const;

private:
    // Reads the last stored record in full (not just its timestamp), so
    // append() can validate the seam between old and new data.
    std::optional<Candle> lastCandle() const;

    std::string path_;
    static constexpr char kMagic[4] = {'C','T','C','1'};
    static constexpr size_t kRecordSize = sizeof(int64_t) + 5 * sizeof(double);
};

} // namespace trader
