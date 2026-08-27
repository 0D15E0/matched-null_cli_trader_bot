#include "poloniex_source.h"
#include "http_client.h"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <iostream>
#include <algorithm>
#include <optional>

namespace trader {

using json = nlohmann::json;

std::string PoloniexSource::intervalFromPeriod(int64_t periodSeconds) {
    switch (periodSeconds) {
        case 300:   return "MINUTE_5";
        case 900:   return "MINUTE_15";
        case 1800:  return "MINUTE_30";
        case 7200:  return "HOUR_2";
        case 14400: return "HOUR_4";
        case 86400: return "DAY_1";
        default:
            throw std::invalid_argument("PoloniexSource: unsupported period " + std::to_string(periodSeconds));
    }
}

namespace {

// Poloniex sends numeric fields as JSON strings, but that is a wire-format
// detail we should not bet the whole fetch on: accept either representation
// and signal failure by return value so one malformed row can be skipped
// instead of aborting the batch (std::stod throws, and the old code let that
// escape all the way out of fetchHistory).
bool jsonToDouble(const json& v, double& out) {
    try {
        if (v.is_number()) { out = v.get<double>(); return true; }
        if (v.is_string()) { out = std::stod(v.get<std::string>()); return true; }
    } catch (const std::exception&) {
        return false;
    }
    return false;
}

bool jsonToInt64(const json& v, int64_t& out) {
    try {
        if (v.is_number_integer()) { out = v.get<int64_t>(); return true; }
        if (v.is_number()) { out = static_cast<int64_t>(v.get<double>()); return true; }
        if (v.is_string()) { out = std::stoll(v.get<std::string>()); return true; }
    } catch (const std::exception&) {
        return false;
    }
    return false;
}

} // namespace

// NOTE: Poloniex's public candle schema below follows their documented v3
// REST API (array-of-arrays):
//   [low, high, open, close, amount, quantity, buyTakerAmount,
//    buyTakerQuantity, tradeCount, ts, weightedAverage, interval,
//    startTime, closeTime]
// That is 14 fields, indices 0..13. The parser therefore requires at least 13
// fields before touching row[12] (the old `row.size() < 10` guard was
// undefined behaviour waiting for a schema change) and 14 before reading the
// closeTime at row[13].
// If Poloniex changes this schema, only this parsing function needs updating
// - the rest of the pipeline (CandleStore, indicators, backtest) is
// completely decoupled from the wire format.
//
// IMPORTANT: Poloniex's public candles endpoint does NOT reliably page
// *forward* via `startTime` (it was observed to just return the most
// recent `limit` candles regardless of `startTime`). It DOES page
// *backward* correctly via `endTime`. So to build full history (or to
// backfill a gap), we walk backward from "now" using `endTime`, collecting
// pages until we either reach `sinceUnixSeconds` or run out of history,
// then sort ascending and filter.
//
// "Run out of history" means an EMPTY page (retried once), never a SHORT
// one. A short page looks like exhaustion but is usually a venue-side gap
// (maintenance window) with years of history behind it: treating short as
// done silently truncated the 15m walk at 469 days when the venue serves
// 8+ years. Verified against a probe at 3000 days back, which still
// returned data.
//
// A direct consequence of paging backward from now: the first page ALWAYS
// contains the candle currently being formed. See the header for why storing
// that one is unrecoverable.
std::vector<Candle> PoloniexSource::fetchHistory(const std::string& symbol,
                                                  int64_t periodSeconds,
                                                  std::optional<int64_t> sinceUnixSeconds,
                                                  bool includeForming,
                                                  int64_t nowUnixSeconds,
                                                  int maxPages) {
    const std::string interval = intervalFromPeriod(periodSeconds);
    HttpClient http;
    std::vector<Candle> all;

    const int limit = 500; // Poloniex max page size for candles
    const int64_t nowSec = nowUnixSeconds > 0
                               ? nowUnixSeconds
                               : std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
    int64_t endMs = nowSec * 1000;
    size_t malformedRows = 0;
    size_t formingDropped = 0;

    // The backward walk advances only by what the server tells us: `endMs`
    // moves to just before the oldest startTime on the page. If a page yields
    // no usable startTime at all - the documented failure being a schema
    // change that trips the row-size guard on every row - then there is
    // nothing to advance to, and the old code re-issued byte-identical
    // requests forever. An unbounded network loop in a fetch tool is worse
    // than an incomplete fetch, so an unreadable page escalates instead of
    // repeating: retry the same window once (transient failures cost no bars
    // that way), then step back a page width (which does skip bars, loudly),
    // then give up - at most kMaxUnusablePages requests per stuck window. If
    // the schema really has changed, every page is bad and the walk ends after
    // three requests. An absolute page cap backs all of it up, sized well past
    // any real history: the longest series here is ~25k 4h bars = 50 pages,
    // and even 10 years of 5m bars is ~2.1k pages.
    const int64_t pageWidthMs = static_cast<int64_t>(limit) * periodSeconds * 1000;
    constexpr int kMaxUnusablePages = 3;
    constexpr int kSafetyMaxPages = 10000;
    const int pageCap = (maxPages > 0 && maxPages < kSafetyMaxPages) ? maxPages : kSafetyMaxPages;
    int unusablePages = 0;
    int emptyPages = 0;
    int consecutiveEmptyWindows = 0;
    int skippedEmptyWindows = 0;
    int pages = 0;

    for (;;) {
        if (++pages > pageCap) {
            if (pageCap == kSafetyMaxPages)
                std::cerr << "PoloniexSource: page cap (" << pageCap << ") reached for " << symbol
                          << " at endTime=" << endMs << "; stopping. History may be incomplete.\n";
            break;
        }

        limiter_.acquire(); // respect rate limit before every request

        std::string url = "https://api.poloniex.com/markets/" + symbol + "/candles"
                           "?interval=" + interval + "&limit=" + std::to_string(limit) +
                           "&endTime=" + std::to_string(endMs);

        std::string body;
        try {
            body = http.get(url);
        } catch (const std::exception& e) {
            std::cerr << "PoloniexSource: request failed: " << e.what() << "\n";
            break;
        }

        json parsed;
        try {
            parsed = json::parse(body);
        } catch (const std::exception& e) {
            std::cerr << "PoloniexSource: JSON parse failed: " << e.what() << " body=" << body.substr(0, 200) << "\n";
            break;
        }

        if (!parsed.is_array()) break;
        if (parsed.empty()) {
            // An empty page is either the end of history or the server
            // refusing a window it actually has data for. The refusals are
            // real and NOT merely transient: measured across reruns, full-
            // history walks stopped at random depths (BTC: 8.4y, then 3.5y;
            // XRP: 1.4y, then 11.5y), so neither one retry nor several is
            // enough. Escalate in two stages: retry the same window with
            // growing backoff, then SKIP back a page width and keep walking
            // (a skipped window loses at most `limit` bars, and whole-period
            // gaps are legal downstream). Only a long consecutive run of
            // empty windows - far longer than any observed refusal streak -
            // is treated as the true end of history.
            if (++emptyPages <= 4) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500 * emptyPages));
                continue;
            }
            emptyPages = 0;
            ++skippedEmptyWindows;
            if (++consecutiveEmptyWindows >= 20) break;
            endMs -= pageWidthMs;
            if (sinceUnixSeconds.has_value() && endMs / 1000 <= *sinceUnixSeconds) break;
            continue;
        }
        emptyPages = 0;
        consecutiveEmptyWindows = 0;

        // Empty only while no row on this page has yielded a startTime; that
        // is exactly the "learned nothing" condition the brakes below test,
        // so it must not be pre-seeded with endMs.
        std::optional<int64_t> minStartTimeThisPage;
        for (const auto& row : parsed) {
            if (!row.is_array() || row.size() < 13) { ++malformedRows; continue; }

            int64_t startTimeMs = 0;
            if (!jsonToInt64(row[12], startTimeMs)) { ++malformedRows; continue; }
            // Paging must advance on every row the server sent, including the
            // ones we are about to drop - otherwise dropping the forming
            // candle would stall the backward walk.
            if (!minStartTimeThisPage.has_value() || startTimeMs < *minStartTimeThisPage) {
                minStartTimeThisPage = startTimeMs;
            }

            Candle c;
            if (!jsonToDouble(row[0], c.low) || !jsonToDouble(row[1], c.high) ||
                !jsonToDouble(row[2], c.open) || !jsonToDouble(row[3], c.close) ||
                !jsonToDouble(row[4], c.volume)) {
                ++malformedRows;
                continue;
            }
            c.timestamp = startTimeMs / 1000;

            if (!includeForming) {
                // Prefer the server's own closeTime; fall back to the nominal
                // window end when the field is absent (schema drift) so the
                // guard degrades instead of disappearing.
                int64_t closeSec = c.timestamp + periodSeconds;
                int64_t closeTimeMs = 0;
                if (row.size() >= 14 && jsonToInt64(row[13], closeTimeMs) && closeTimeMs > 0) {
                    closeSec = closeTimeMs / 1000;
                }
                if (closeSec > nowSec) { ++formingDropped; continue; }
            }

            all.push_back(c);
        }

        if (!minStartTimeThisPage.has_value()) {
            ++unusablePages;
            if (unusablePages >= kMaxUnusablePages) {
                std::cerr << "PoloniexSource: " << unusablePages
                          << " consecutive unreadable pages for " << symbol
                          << "; giving up. History may be incomplete - check the candle schema.\n";
                break;
            }
            if (unusablePages == 1) {
                // Re-request the SAME window first. A page can be unreadable
                // for reasons that do not repeat (a truncated response, an
                // error page from a proxy), and a retry is the only recovery
                // that costs no bars. This is what the old code did on every
                // iteration - the defect was never the retry, it was retrying
                // without limit.
                std::cerr << "PoloniexSource: page of " << parsed.size() << " row(s) for " << symbol
                          << " at endTime=" << endMs
                          << " yielded no usable startTime; retrying the same window once\n";
            } else {
                // Still unreadable, so the window itself is the problem: step
                // over it. Say plainly that this SKIPS bars - a gap of whole
                // periods is legal in a candle series (weekends, outages), so
                // nothing downstream will flag it, and this line is the only
                // notice anyone gets.
                std::cerr << "PoloniexSource: page at endTime=" << endMs << " for " << symbol
                          << " is still unreadable (schema change?); stepping back " << limit
                          << " x " << periodSeconds << "s. Those bars will be MISSING from this "
                             "fetch.\n";
                endMs -= pageWidthMs;
                if (sinceUnixSeconds.has_value() && endMs / 1000 <= *sinceUnixSeconds) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        unusablePages = 0;

        bool reachedSince = sinceUnixSeconds.has_value() &&
                            (*minStartTimeThisPage / 1000) <= *sinceUnixSeconds;
        if (reachedSince) break;

        endMs = *minStartTimeThisPage - 1; // page further back in time

        // Be polite even within the rate limiter's allowance.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (malformedRows > 0) {
        std::cerr << "PoloniexSource: skipped " << malformedRows
                  << " malformed candle row(s) for " << symbol << " (schema change?)\n";
    }
    if (skippedEmptyWindows > 0) {
        std::cerr << "PoloniexSource: skipped " << skippedEmptyWindows
                  << " persistently empty window(s) for " << symbol << " (up to "
                  << skippedEmptyWindows * limit << " bars may be missing mid-history; "
                     "the trailing run of empties is normal end-of-history probing)\n";
    }
    if (formingDropped > 0) {
        std::cerr << "PoloniexSource: dropped " << formingDropped
                  << " still-forming candle(s) for " << symbol << "\n";
    }

    std::sort(all.begin(), all.end(), [](const Candle& a, const Candle& b) {
        return a.timestamp < b.timestamp;
    });
    all.erase(std::unique(all.begin(), all.end(), [](const Candle& a, const Candle& b) {
                  return a.timestamp == b.timestamp;
              }),
              all.end());

    if (sinceUnixSeconds.has_value()) {
        int64_t since = *sinceUnixSeconds;
        all.erase(std::remove_if(all.begin(), all.end(),
                                  [since](const Candle& c) { return c.timestamp <= since; }),
                  all.end());
    }

    return all;
}

} // namespace trader
