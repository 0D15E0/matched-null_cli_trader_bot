#include "candle_store.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <filesystem>
#include <system_error>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace trader {

constexpr char CandleStore::kMagic[4];

namespace {

// Compact number formatting for issue details: enough digits to identify the
// offending value, without dragging <format> or printf into the codebase.
std::string num(double v) {
    std::ostringstream os;
    os << std::setprecision(10) << v;
    return os.str();
}

bool finitePrice(double v) { return std::isfinite(v); }

// Distance from `gap` to the nearest whole multiple of `period`. A gap of
// several periods is fine (missing bars are normal); a gap that is 1.5
// periods is not.
int64_t multipleResidual(int64_t gap, int64_t period) {
    if (period <= 0) return 0;
    int64_t k = (gap + period / 2) / period; // nearest, not floor
    if (k < 1) k = 1;
    int64_t residual = gap - k * period;
    return residual < 0 ? -residual : residual;
}

// DST slack. Yahoo timestamps daily bars at the local session open, so a
// daily series legitimately shifts by exactly one hour twice a year. Intraday
// and crypto periods are UTC-aligned and get no slack.
int64_t spacingSlack(int64_t period) { return period >= 86400 ? 3600 : 0; }

bool isWarningKind(const std::string& kind) {
    return kind == "zero-volume" || kind == "outlier";
}

// Thresholds for calling a set of disagreeing bars a provider re-basing
// rather than ordinary revision noise (see compareOverlap). Both of the first
// two matter, and the old code only had a loose version of the first:
//
//  - kRebaseRatioSpread: how tightly the shared ratio must hold. A real
//    re-basing multiplies the whole history by ONE exact factor, so the only
//    spread is the provider's decimal rounding of the adjusted close (~1e-5
//    relative at equity price levels). 0.5% is orders of magnitude looser
//    than that, and still tight enough to be a test rather than a formality.
//  - kMinRebaseOffset: how far that shared ratio must sit from 1.0. Without
//    it, three bars revised by 0.2% "share a ratio" of 1.002 and were
//    reported as a stock split - and worse, a ratio whose distance from 1.0
//    is no bigger than the spread band we allow around it carries no
//    information at all. 2% is 4x the band; no credible price revision is
//    that large, and every real split is an order of magnitude further out
//    (5:4, the narrowest ratio in practical use, is 20%; 2:1 is 50%). The
//    cost is that a sub-2% dividend re-basing is reported as "mismatch"
//    instead of "rebased" - accepted deliberately, because at that size it is
//    genuinely indistinguishable from a revision, and both classifications
//    lead the operator to the same place: rebuild with --repair.
//  - kMinRebaseBars: fewer than three agreeing bars is a coincidence, not a
//    pattern.
constexpr double kRebaseRatioSpread = 0.005;
constexpr double kMinRebaseOffset   = 0.02;
constexpr size_t kMinRebaseBars     = 3;

// How many disagreeing bars may dissent from the shared ratio. Demanding
// unanimity (the old rule) meant one bar that was BOTH re-based and revised
// downgraded a textbook split to an unexplained mismatch, which is the more
// dangerous error: "mismatch" reads as "we don't know", while a split has a
// known repair.
size_t allowedRatioOutliers(size_t disagreeing) {
    return std::max<size_t>(2, disagreeing / 10);
}

// Appends raw bytes to an in-memory record buffer. Both writers serialize
// fully before touching the file, so that the payload reaches the kernel in
// one write() call.
void put(std::vector<char>& buf, const void* src, size_t n) {
    const char* p = static_cast<const char*>(src);
    buf.insert(buf.end(), p, p + n);
}

void putHeader(std::vector<char>& buf, const char* magic, int64_t periodSeconds,
               const std::string& symbol) {
    put(buf, magic, 4);
    put(buf, &periodSeconds, sizeof(periodSeconds));
    uint32_t len = static_cast<uint32_t>(symbol.size());
    put(buf, &len, sizeof(len));
    if (len) put(buf, symbol.data(), len);
}

void putCandle(std::vector<char>& buf, const Candle& c) {
    put(buf, &c.timestamp, sizeof(c.timestamp));
    put(buf, &c.open, sizeof(c.open));
    put(buf, &c.high, sizeof(c.high));
    put(buf, &c.low, sizeof(c.low));
    put(buf, &c.close, sizeof(c.close));
    put(buf, &c.volume, sizeof(c.volume));
}

// Best effort: flush the directory entry so a freshly created file's *name*
// survives power loss, not just its contents. Not all filesystems need (or
// allow) this, so a failure here is not worth aborting a successful write.
void syncParentDirectory(const std::string& path) {
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) parent = ".";
    int dirFd = ::open(parent.c_str(), O_RDONLY);
    if (dirFd >= 0) {
        ::fsync(dirFd);
        ::close(dirFd);
    }
}

// Writes `buf` at `offset` (creating the file if needed) and fsyncs before
// returning.
//
// This is what makes append() durable rather than merely convenient. The old
// code wrote 6 fields per candle through a filebuf, which flushes in
// implementation-chosen chunks: kill the process during a 2,000-bar append
// and the file ends mid-record, at which point load()/lastTimestamp() - which
// derive the record count by integer division - reinterpret the tail and the
// next append cements fabricated candles into history.
//
// A single write() plus fsync cannot make an append atomic (no POSIX
// guarantee covers that), but it collapses the window to one syscall and, in
// combination with append()'s alignment check on the way in, changes the
// failure mode from "silent corruption" to "loud refusal": the only states
// reachable are the old file, the old file plus whole records, or a file the
// next writer refuses to touch. Nothing here ever reports success for bytes
// the kernel did not take.
void writeAllAndSync(const std::string& path, off_t offset, const std::vector<char>& buf) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        throw std::runtime_error("CandleStore: cannot open " + path + " for writing: " +
                                 std::strerror(errno));
    }
    auto fail = [&](const char* what) {
        const int err = errno;
        ::close(fd);
        throw std::runtime_error("CandleStore: " + std::string(what) + " on " + path + ": " +
                                 std::strerror(err));
    };

    size_t off = 0;
    while (off < buf.size()) {
        ssize_t n = ::pwrite(fd, buf.data() + off, buf.size() - off,
                             offset + static_cast<off_t>(off));
        if (n < 0) {
            if (errno == EINTR) continue;
            fail("write failed");
        }
        off += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) fail("fsync failed");
    if (::close(fd) != 0) {
        throw std::runtime_error("CandleStore: close failed on " + path + ": " +
                                 std::strerror(errno));
    }
}

// Aggregated reporting shared by both writers: a thin-history crypto series
// has hundreds of zero-volume bars and one line each would bury everything
// else. In strict mode the structural kinds are named by the throw instead of
// the warning, so they are never reported twice.
void reportIssues(const std::vector<DataIssue>& issues, const std::string& path,
                  const char* action, bool strict) {
    std::map<std::string, std::pair<size_t, std::string>> byKind; // kind -> {count, first detail}
    std::vector<const DataIssue*> fatal;
    for (const auto& is : issues) {
        auto& entry = byKind[is.kind];
        if (entry.first == 0) {
            entry.second = "ts=" + std::to_string(is.timestamp) + " " + is.detail;
        }
        ++entry.first;
        if (!isWarningKind(is.kind)) fatal.push_back(&is);
    }

    for (const auto& kv : byKind) {
        if (strict && !isWarningKind(kv.first)) continue; // reported by the throw below
        std::cerr << "CandleStore: warning: " << kv.second.first << " x " << kv.first
                  << " in " << path << " (first: " << kv.second.second << ")\n";
    }

    if (!strict || fatal.empty()) return;
    std::ostringstream os;
    os << "CandleStore: refusing to " << action << " " << path << ": " << fatal.size()
       << " data issue(s);";
    const size_t shown = std::min<size_t>(fatal.size(), 3);
    for (size_t i = 0; i < shown; ++i) {
        os << " [" << fatal[i]->kind << " @ " << fatal[i]->timestamp << "] " << fatal[i]->detail << ";";
    }
    if (fatal.size() > shown) os << " ... and " << (fatal.size() - shown) << " more";
    throw std::runtime_error(os.str());
}

} // namespace

std::vector<DataIssue> validateCandles(const std::vector<Candle>& candles, int64_t periodSeconds,
                                        double maxAbsBarReturn) {
    std::vector<DataIssue> issues;
    const int64_t slack = spacingSlack(periodSeconds);

    for (size_t i = 0; i < candles.size(); ++i) {
        const Candle& c = candles[i];

        // --- bar-local invariants -------------------------------------
        if (!finitePrice(c.open) || !finitePrice(c.high) || !finitePrice(c.low) ||
            !finitePrice(c.close) || !std::isfinite(c.volume)) {
            issues.push_back({"ohlc", c.timestamp, "non-finite value in bar (NaN/inf)"});
            continue; // every further comparison on this bar is meaningless
        }
        if (c.open <= 0.0 || c.high <= 0.0 || c.low <= 0.0 || c.close <= 0.0) {
            issues.push_back({"ohlc", c.timestamp,
                              "non-positive price o=" + num(c.open) + " h=" + num(c.high) +
                              " l=" + num(c.low) + " c=" + num(c.close)});
        }
        // The three range invariants get a RELATIVE tolerance, because an
        // exact comparison rejects arithmetically-correct data.
        //
        // Total-return adjustment multiplies each of open/high/low by
        // adjclose/close independently (see yahoo_finance_source.cpp). When a
        // bar closes exactly on its low - completely normal - the two products
        // differ in the last bits, and `low > min(open,close)` fires on values
        // that print identically: SPY was rejected for
        // "low 118.3276901 > min(open,close) 118.3276901". Dividend-paying
        // ETFs adjust often enough that this blocked an entire asset class.
        //
        // The tolerance is relative to the bar's own price so it means the same
        // thing at $3 and at $78,000, and it is orders of magnitude tighter
        // than any real OHLC defect: a genuinely crossed bar is wrong by
        // basis points at least, not by 1e-9 of its level.
        const double tol = 1e-9 * std::max(1.0, std::fabs(c.close));
        if (c.high < c.low - tol) {
            issues.push_back({"ohlc", c.timestamp,
                              "high " + num(c.high) + " < low " + num(c.low)});
        }
        if (c.high < std::max(c.open, c.close) - tol) {
            issues.push_back({"ohlc", c.timestamp,
                              "high " + num(c.high) + " < max(open,close) " +
                              num(std::max(c.open, c.close))});
        }
        if (c.low > std::min(c.open, c.close) + tol) {
            issues.push_back({"ohlc", c.timestamp,
                              "low " + num(c.low) + " > min(open,close) " +
                              num(std::min(c.open, c.close))});
        }
        if (c.volume <= 0.0) {
            // Informational: real venues do print bars with no trades
            // (Poloniex's thin 2015 BTC history is full of them).
            issues.push_back({"zero-volume", c.timestamp, "volume " + num(c.volume)});
        }

        if (i == 0) continue;
        const Candle& prev = candles[i - 1];

        // --- ordering and spacing -------------------------------------
        if (c.timestamp == prev.timestamp) {
            issues.push_back({"duplicate", c.timestamp, "timestamp repeated at index " + std::to_string(i)});
        } else if (c.timestamp < prev.timestamp) {
            issues.push_back({"nonmonotonic", c.timestamp,
                              "timestamp goes backwards from " + std::to_string(prev.timestamp)});
        } else if (periodSeconds > 0) {
            int64_t gap = c.timestamp - prev.timestamp;
            int64_t residual = multipleResidual(gap, periodSeconds);
            if (residual > slack) {
                issues.push_back({"spacing", c.timestamp,
                                  "gap " + std::to_string(gap) + "s is not a multiple of " +
                                  std::to_string(periodSeconds) + "s (off by " +
                                  std::to_string(residual) + "s)"});
            }
        }

        // --- continuity ------------------------------------------------
        if (finitePrice(prev.close) && prev.close > 0.0) {
            double ret = c.close / prev.close - 1.0;
            if (std::fabs(ret) > maxAbsBarReturn) {
                issues.push_back({"outlier", c.timestamp,
                                  "one-bar close return " + num(ret * 100.0) + "% (" +
                                  num(prev.close) + " -> " + num(c.close) + ")"});
            }
        }
    }
    return issues;
}

std::vector<DataIssue> validateSeries(const CandleSeries& series, double maxAbsBarReturn) {
    std::vector<Candle> candles;
    candles.reserve(series.size());
    for (size_t i = 0; i < series.size(); ++i) candles.push_back(series.at(i));
    return validateCandles(candles, series.periodSeconds, maxAbsBarReturn);
}

CandleStore::CandleStore(std::string filePath) : path_(std::move(filePath)) {}

bool CandleStore::exists() const {
    // Deliberately stricter than the old `ifstream(path).good()`: that also
    // returned true for a 0-byte file, and append() used it to decide whether
    // a header was needed - so an interrupted create produced a store with
    // records and no header. Callers asking "is there data to read?" get a
    // useful answer either way; callers about to WRITE must use isValidStore().
    std::error_code ec;
    auto st = std::filesystem::status(path_, ec);
    if (ec || !std::filesystem::is_regular_file(st)) return false;
    auto size = std::filesystem::file_size(path_, ec);
    return !ec && size > 0;
}

bool CandleStore::isValidStore() const { return header().has_value(); }

static bool readHeader(std::ifstream& f, int64_t& periodSeconds, std::string& symbol) {
    char magic[4];
    f.read(magic, 4);
    if (!f || std::memcmp(magic, "CTC1", 4) != 0) return false;
    f.read(reinterpret_cast<char*>(&periodSeconds), sizeof(periodSeconds));
    uint32_t len = 0;
    f.read(reinterpret_cast<char*>(&len), sizeof(len));
    // Sanity cap: without it a corrupt/truncated header makes us resize() a
    // string to whatever garbage the length field happened to hold.
    if (!f || len > 1024) return false;
    symbol.resize(len);
    if (len) f.read(symbol.data(), len);
    // Strictly `f`, not `f || f.eof()`: read() sets eofbit precisely when it
    // could NOT extract all `len` bytes, so the old fallback accepted a header
    // truncated inside the symbol and handed back a half-read string (and a
    // tellg() of -1). Reading a symbol that ends exactly at EOF leaves the
    // stream good, so a legitimate header-only file still passes.
    return static_cast<bool>(f);
}

namespace {

// Everything a writer needs to know about the file before it writes anything:
// whether a header must be laid down, and whether the existing data region is
// a whole number of records. Gathered in one place because getting either of
// those two questions wrong is silent history corruption.
struct StoreLayout {
    bool        present = false;      // a regular file exists at the path
    uintmax_t   size = 0;             // its length in bytes
    bool        validHeader = false;  // it starts with a readable CTC1 header
    int64_t     periodSeconds = 0;
    std::string symbol;
    uintmax_t   dataStart = 0;        // first byte after the header
    uintmax_t   dataBytes = 0;        // size - dataStart
    uintmax_t   wholeRecords = 0;
    uintmax_t   partialBytes = 0;     // dataBytes % recordSize; 0 == intact
};

StoreLayout inspectStore(const std::string& path, size_t recordSize) {
    StoreLayout l;
    std::error_code ec;
    auto st = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::is_regular_file(st)) return l;
    l.present = true;
    l.size = std::filesystem::file_size(path, ec);
    if (ec) l.size = 0;
    if (l.size == 0) return l; // zero-length: a fresh store, nothing to check

    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return l;
    if (!readHeader(f, l.periodSeconds, l.symbol)) return l;
    const std::streampos pos = f.tellg();
    if (pos < 0) return l;

    l.dataStart = static_cast<uintmax_t>(pos);
    if (l.size < l.dataStart) return l; // header longer than the file: unusable
    l.validHeader = true;
    l.dataBytes = l.size - l.dataStart;
    l.wholeRecords = l.dataBytes / recordSize;
    l.partialBytes = l.dataBytes % recordSize;
    return l;
}

} // namespace

CandleSeries CandleStore::load() const {
    CandleSeries series;
    std::ifstream f(path_, std::ios::binary);
    if (!f.good()) return series;

    int64_t periodSeconds = 0;
    std::string symbol;
    if (!readHeader(f, periodSeconds, symbol)) return series;

    series.symbol = symbol;
    series.periodSeconds = periodSeconds;

    // Determine remaining record count to reserve capacity up-front.
    auto dataStart = f.tellg();
    f.seekg(0, std::ios::end);
    auto end = f.tellg();
    f.seekg(dataStart);
    size_t bytesLeft = static_cast<size_t>(end - dataStart);
    size_t count = bytesLeft / kRecordSize;
    if (bytesLeft % kRecordSize != 0) {
        // The division below hides this, and hiding it is how a torn write
        // becomes permanent: the reader silently drops the partial tail while
        // the next writer appends onto it. Say so out loud on every read.
        std::cerr << "CandleStore: warning: " << path_ << " has " << (bytesLeft % kRecordSize)
                  << " trailing byte(s) after " << count << " whole records - an append was "
                  << "interrupted. Ignoring the partial record; rebuild the store (fetch --repair).\n";
    }
    series.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        Candle c;
        f.read(reinterpret_cast<char*>(&c.timestamp), sizeof(c.timestamp));
        f.read(reinterpret_cast<char*>(&c.open), sizeof(c.open));
        f.read(reinterpret_cast<char*>(&c.high), sizeof(c.high));
        f.read(reinterpret_cast<char*>(&c.low), sizeof(c.low));
        f.read(reinterpret_cast<char*>(&c.close), sizeof(c.close));
        f.read(reinterpret_cast<char*>(&c.volume), sizeof(c.volume));
        if (!f) break;
        series.push(c);
    }
    return series;
}

std::optional<std::pair<std::string, int64_t>> CandleStore::header() const {
    std::ifstream f(path_, std::ios::binary);
    if (!f.good()) return std::nullopt;
    int64_t periodSeconds = 0;
    std::string symbol;
    if (!readHeader(f, periodSeconds, symbol)) return std::nullopt;
    return std::make_pair(symbol, periodSeconds);
}

std::optional<int64_t> CandleStore::lastTimestamp() const {
    auto c = lastCandle();
    if (!c.has_value()) return std::nullopt;
    return c->timestamp;
}

std::optional<Candle> CandleStore::lastCandle() const {
    std::ifstream f(path_, std::ios::binary);
    if (!f.good()) return std::nullopt;

    int64_t periodSeconds = 0;
    std::string symbol;
    if (!readHeader(f, periodSeconds, symbol)) return std::nullopt;

    auto dataStart = f.tellg();
    f.seekg(0, std::ios::end);
    auto end = f.tellg();
    size_t bytesLeft = static_cast<size_t>(end - dataStart);
    if (bytesLeft < kRecordSize) return std::nullopt;

    // Seek directly to the last record instead of scanning the whole file.
    size_t count = bytesLeft / kRecordSize;
    std::streamoff lastRecordOffset =
        static_cast<std::streamoff>(dataStart) + static_cast<std::streamoff>((count - 1) * kRecordSize);
    f.seekg(lastRecordOffset);
    Candle c;
    f.read(reinterpret_cast<char*>(&c.timestamp), sizeof(c.timestamp));
    f.read(reinterpret_cast<char*>(&c.open), sizeof(c.open));
    f.read(reinterpret_cast<char*>(&c.high), sizeof(c.high));
    f.read(reinterpret_cast<char*>(&c.low), sizeof(c.low));
    f.read(reinterpret_cast<char*>(&c.close), sizeof(c.close));
    f.read(reinterpret_cast<char*>(&c.volume), sizeof(c.volume));
    if (!f) return std::nullopt;
    return c;
}

size_t CandleStore::append(const std::string& symbol, int64_t periodSeconds,
                            const std::vector<Candle>& candles, bool strict) {
    if (candles.empty()) return 0;

    const StoreLayout layout = inspectStore(path_, kRecordSize);

    // Header decision, made from what can be READ rather than from mere
    // existence. `!exists()` was the old test, and it returned false for a
    // 0-byte or junk file - so the records went in under no header at all and
    // every later reader saw an empty store, or worse, read the first record
    // as a header. Only "no file" and "zero-length file" mean fresh store.
    if (layout.present && layout.size > 0 && !layout.validHeader) {
        throw std::runtime_error(
            "CandleStore: " + path_ + " is " + std::to_string(layout.size) +
            " bytes but does not start with a readable CTC1 header, so its record boundaries are "
            "unknown; refusing to append. Move the file aside or rebuild it (fetch --repair).");
    }
    const bool needHeader = !layout.validHeader;

    // Refuse to mix timeframes in one file. The store's period lives in the
    // header and every consumer (indicators, annualization, live polling)
    // trusts it, so writing 1d bars into a 4h store is unrecoverable
    // corruption rather than a merge.
    if (layout.validHeader && layout.periodSeconds != periodSeconds) {
        throw std::runtime_error("CandleStore: " + path_ + " holds period " +
                                 std::to_string(layout.periodSeconds) + "s, refusing to append " +
                                 std::to_string(periodSeconds) + "s candles");
    }
    if (layout.validHeader && layout.symbol != symbol) {
        std::cerr << "CandleStore: warning: " << path_ << " holds symbol '" << layout.symbol
                  << "' but appending '" << symbol << "'\n";
    }

    // The write cursor must be a RECORD boundary, not merely the end of the
    // file. Nothing in the format marks where records begin, so a file left
    // short by a torn write silently re-frames everything appended after it:
    // load() and lastTimestamp() both derive the record count by integer
    // division, so the partial tail plus the first bytes of the next append
    // become a fabricated candle with a garbage timestamp - and once that is
    // in, `timestamp <= lastTimestamp()` filtering means it can never be
    // fetched over. Refusing is the only safe move; truncation is offered as
    // an explicit repair rather than done silently, because the operator may
    // prefer to keep the evidence.
    if (layout.validHeader && layout.partialBytes != 0) {
        const uintmax_t goodSize = layout.dataStart + layout.wholeRecords * kRecordSize;
        throw std::runtime_error(
            "CandleStore: " + path_ + " is CORRUPT and will not be appended to: its data region is " +
            std::to_string(layout.dataBytes) + " bytes, which is " + std::to_string(layout.wholeRecords) +
            " complete " + std::to_string(kRecordSize) + "-byte record(s) plus " +
            std::to_string(layout.partialBytes) + " leftover byte(s). A previous append was "
            "interrupted mid-record. Appending now would turn that partial record into a "
            "fabricated candle that no future fetch can correct. Rebuild the store "
            "(fetch --repair), or truncate the file to " + std::to_string(goodSize) +
            " bytes to drop the partial record and keep the " + std::to_string(layout.wholeRecords) +
            " good ones.");
    }

    // Only candles strictly newer than what we already hold get written; the
    // caller is expected to re-fetch with an overlap (see compareOverlap) so
    // the batch normally starts inside stored history.
    std::optional<Candle> lastStored;
    if (layout.wholeRecords > 0) lastStored = lastCandle();

    std::vector<Candle> fresh;
    fresh.reserve(candles.size());
    for (const auto& c : candles) {
        if (lastStored.has_value() && c.timestamp <= lastStored->timestamp) continue;
        fresh.push_back(c);
    }
    if (fresh.empty()) return 0;

    // Validate the bars we are about to WRITE, not the whole batch. The
    // difference matters: the discarded head of the batch is the re-fetch
    // overlap, so validating it made one bad bar anywhere in a venue's history
    // a permanent block on all future appends - a refusal to store good new
    // data because of an old bar we were going to throw away anyway. Whether
    // the overlap still agrees with the store is compareOverlap()'s question.
    std::vector<DataIssue> issues = validateCandles(fresh, periodSeconds);

    // The seam needs the last stored bar as context: a batch can be internally
    // perfect and still be wrong where it meets the store - a half-period
    // offset, or the fake crash bar you get when a provider re-bases history
    // after a split.
    if (lastStored.has_value()) {
        std::vector<Candle> seam{*lastStored, fresh.front()};
        for (auto& is : validateCandles(seam, periodSeconds)) {
            // Keep only the bar-to-bar findings; the bar-local ones were
            // already reported by the pass over `fresh` above.
            if (is.kind != "spacing" && is.kind != "outlier") continue;
            is.detail = "at the seam with stored history: " + is.detail;
            issues.push_back(std::move(is));
        }
    }

    reportIssues(issues, path_, "append to", strict); // throws in strict mode

    // Serialize header (if needed) and records together, then hand the kernel
    // one write() at the end of the last whole record and fsync it. Two
    // properties come out of doing it this way: the header can no longer be
    // written without its records (they are the same write), and no partially
    // accepted write is ever reported as success.
    std::vector<char> buf;
    buf.reserve((needHeader ? 16 + symbol.size() : 0) + fresh.size() * kRecordSize);
    if (needHeader) putHeader(buf, kMagic, periodSeconds, symbol);
    for (const auto& c : fresh) putCandle(buf, c);

    const uintmax_t offset = needHeader ? 0 : layout.dataStart + layout.dataBytes;
    writeAllAndSync(path_, static_cast<off_t>(offset), buf);
    if (needHeader) syncParentDirectory(path_);
    return fresh.size();
}

void CandleStore::rewrite(const std::string& symbol, int64_t periodSeconds,
                           const std::vector<Candle>& candles, bool strict) {
    // Validate BEFORE writing anything. `--repair` is the one path that puts
    // raw venue data on disk without ever passing through append(), so an
    // unvalidated rewrite made the command used to FIX corruption the only
    // command that could introduce it unchecked. Same fatal/non-fatal split as
    // append(): structural issues throw, zero-volume/outlier only warn.
    if (strict && candles.empty()) {
        throw std::runtime_error("CandleStore: refusing to rewrite " + path_ +
                                 " with an empty series - that would erase the store. Pass "
                                 "strict=false if the erasure is intended.");
    }
    reportIssues(validateCandles(candles, periodSeconds), path_, "rewrite", strict);

    // Serialize into memory first: publishing must be one atomic rename, and
    // even the longest store here (25k 4h BTC bars) is barely a megabyte.
    std::vector<char> buf;
    buf.reserve(16 + symbol.size() + candles.size() * kRecordSize);
    putHeader(buf, kMagic, periodSeconds, symbol);
    for (const auto& c : candles) putCandle(buf, c);

    const std::string tmp = path_ + ".tmp";
    int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) throw std::runtime_error("CandleStore: cannot create " + tmp);

    auto fail = [&](const std::string& what) {
        ::close(fd);
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        throw std::runtime_error("CandleStore: " + what + " for " + tmp);
    };

    size_t off = 0;
    while (off < buf.size()) {
        ssize_t n = ::write(fd, buf.data() + off, buf.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            fail("write failed");
        }
        off += static_cast<size_t>(n);
    }

    // fsync BEFORE the rename. rename() is atomic with respect to readers,
    // but without the fsync a crash can publish a name that points at blocks
    // the kernel never flushed - i.e. exactly the truncated store this
    // function exists to prevent.
    if (::fsync(fd) != 0) fail("fsync failed");
    ::close(fd);

    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);
    if (ec) {
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        throw std::runtime_error("CandleStore: rename " + tmp + " -> " + path_ + " failed: " + ec.message());
    }

    syncParentDirectory(path_); // so the rename itself survives power loss
}

std::vector<DataIssue> CandleStore::compareOverlap(const std::vector<Candle>& fresh,
                                                    double tolerancePct) const {
    std::vector<DataIssue> issues;
    if (fresh.empty()) return issues;

    CandleSeries stored = load();
    if (stored.empty()) return issues;

    std::unordered_map<int64_t, double> storedClose;
    storedClose.reserve(stored.size() * 2);
    for (size_t i = 0; i < stored.size(); ++i) storedClose[stored.timestamp[i]] = stored.close[i];

    // Ratios of the bars that actually disagree. A split shows up as one
    // ratio repeated across every pre-split bar (Yahoo re-bases the whole
    // history), while a genuine data error is ratio noise.
    struct Diff { int64_t ts; double storedC; double freshC; double ratio; };
    std::vector<Diff> diffs;
    size_t overlap = 0;

    for (const auto& c : fresh) {
        auto it = storedClose.find(c.timestamp);
        if (it == storedClose.end()) continue;
        ++overlap;
        double s = it->second;
        if (!std::isfinite(s) || !std::isfinite(c.close) || s <= 0.0) {
            issues.push_back({"mismatch", c.timestamp,
                              "unusable close (stored " + num(s) + ", fresh " + num(c.close) + ")"});
            continue;
        }
        double rel = std::fabs(c.close - s) / s;
        if (rel > tolerancePct) diffs.push_back({c.timestamp, s, c.close, c.close / s});
    }

    if (diffs.empty()) return issues;

    // Is there one multiplicative factor behind these disagreements?
    //
    // Three conditions, and the old code only had a weak form of the second:
    //   1. enough disagreeing bars to be a pattern (kMinRebaseBars);
    //   2. they cluster tightly around the median ratio (kRebaseRatioSpread),
    //      with a couple of dissenters tolerated (allowedRatioOutliers) - a
    //      bar can be re-based AND revised, and demanding unanimity turned a
    //      textbook split into an unexplained "mismatch";
    //   3. that shared ratio is meaningfully far from 1.0 (kMinRebaseOffset).
    //      Missing this was the misclassification that mattered: a few closes
    //      revised by 0.2% trivially "share" a ratio of 1.002, and the store
    //      was told it had witnessed a stock split.
    // Median rather than mean, so dissenting bars cannot drag the reference.
    bool rebased = false;
    double refRatio = 1.0;
    size_t agreeing = 0;
    if (overlap >= kMinRebaseBars && diffs.size() >= kMinRebaseBars) {
        std::vector<double> ratios;
        ratios.reserve(diffs.size());
        for (const auto& d : diffs) ratios.push_back(d.ratio);
        std::sort(ratios.begin(), ratios.end());
        refRatio = ratios[ratios.size() / 2];
        if (refRatio > 0.0 && std::fabs(refRatio - 1.0) >= kMinRebaseOffset) {
            for (double r : ratios) {
                if (std::fabs(r / refRatio - 1.0) <= kRebaseRatioSpread) ++agreeing;
            }
            rebased = agreeing >= kMinRebaseBars &&
                      (diffs.size() - agreeing) <= allowedRatioOutliers(diffs.size());
        }
    }

    for (const auto& d : diffs) {
        // Classify per bar: when a re-basing is established, the bars the
        // ratio actually explains are "rebased" and any dissenter stays an
        // unexplained "mismatch". Lumping the dissenters in with the split
        // would claim the repair accounts for them, and it does not.
        const bool followsRatio =
            rebased && std::fabs(d.ratio / refRatio - 1.0) <= kRebaseRatioSpread;
        std::string detail = "stored close " + num(d.storedC) + " vs fresh " + num(d.freshC) +
                             " (ratio " + num(d.ratio) + ")";
        if (followsRatio) {
            detail += "; ratio " + num(refRatio) + " shared by " + std::to_string(agreeing) +
                      " of " + std::to_string(diffs.size()) + " disagreeing bars (" +
                      std::to_string(overlap) + " overlapping)" +
                      " - provider re-based history (split/dividend adjustment), rebuild the store";
        } else if (rebased) {
            detail += "; does NOT follow the " + num(refRatio) +
                      " re-basing ratio the other bars share";
        }
        issues.push_back({followsRatio ? "rebased" : "mismatch", d.ts, detail});
    }
    return issues;
}

} // namespace trader
