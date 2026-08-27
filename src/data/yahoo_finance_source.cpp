#include "yahoo_finance_source.h"
#include "http_client.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace trader {

using json = nlohmann::json;

namespace {

// Nominal bar length for a Yahoo interval string. Only used to decide whether
// the final bar has finished, so the coarse calendar approximation for "1mo"
// is good enough.
int64_t barSecondsFromInterval(const std::string& interval) {
    if (interval == "5m")  return 300;
    if (interval == "15m") return 900;
    if (interval == "30m") return 1800;
    if (interval == "1h" || interval == "60m") return 3600;
    if (interval == "1d")  return 86400;
    if (interval == "1wk") return 604800;
    if (interval == "1mo") return 2592000;
    std::cerr << "YahooFinanceSource: unknown interval '" << interval
              << "', assuming daily bars for the forming-bar check\n";
    return 86400;
}

// Reads meta.currentTradingPeriod.regular.{start,end} if present. BOTH are
// needed: `end` alone only says whether *a* session is still open, and the
// question that decides whether to drop a bar is whether THAT BAR belongs to
// the open session - which takes `start`. Yahoo omits these on some responses
// (and the whole `meta` block on error payloads), so every level is checked
// before it is dereferenced - nlohmann's const operator[] does not bounds- or
// key-check in a release build.
bool readRegularSession(const json& result, int64_t& start, int64_t& end) {
    if (!result.contains("meta") || !result["meta"].is_object()) return false;
    const auto& meta = result["meta"];
    if (!meta.contains("currentTradingPeriod") || !meta["currentTradingPeriod"].is_object()) return false;
    const auto& period = meta["currentTradingPeriod"];
    if (!period.contains("regular") || !period["regular"].is_object()) return false;
    const auto& regular = period["regular"];
    if (!regular.contains("start") || !regular["start"].is_number()) return false;
    if (!regular.contains("end") || !regular["end"].is_number()) return false;
    start = regular["start"].get<int64_t>();
    end = regular["end"].get<int64_t>();
    return end > start;
}

const json& emptyArray() {
    static const json kEmpty = json::array();
    return kEmpty;
}

} // namespace

std::vector<Candle> YahooFinanceSource::fetchHistory(const std::string& ticker,
                                                       const std::string& interval,
                                                       const std::string& range,
                                                       bool adjustForSplitsAndDividends,
                                                       bool includeForming) {
    limiter_.acquire();

    HttpClient http;
    std::string url = "https://query1.finance.yahoo.com/v8/finance/chart/" + ticker +
                       "?range=" + range + "&interval=" + interval;

    std::string body;
    try {
        body = http.get(url);
    } catch (const std::exception& e) {
        std::cerr << "YahooFinanceSource: request failed: " << e.what() << "\n";
        return {};
    }

    json parsed;
    try {
        parsed = json::parse(body);
    } catch (const std::exception& e) {
        std::cerr << "YahooFinanceSource: JSON parse failed: " << e.what() << "\n";
        return {};
    }

    std::vector<Candle> out;

    if (!parsed.contains("chart") || !parsed["chart"].is_object()) {
        std::cerr << "YahooFinanceSource: unexpected response shape for ticker " << ticker << "\n";
        return out;
    }
    const json& chart = parsed["chart"];
    if (chart.contains("error") && !chart["error"].is_null()) {
        std::cerr << "YahooFinanceSource: upstream error for " << ticker << ": "
                  << chart["error"].dump() << "\n";
        return out;
    }
    if (!chart.contains("result") || !chart["result"].is_array() || chart["result"].empty()) {
        std::cerr << "YahooFinanceSource: no result for ticker " << ticker
                   << " (check symbol, e.g. 'ASML.AS' for Euronext Amsterdam)\n";
        return out;
    }
    const json& result = chart["result"][0];
    if (!result.contains("timestamp") || !result["timestamp"].is_array()) return out;
    if (!result.contains("indicators") || !result["indicators"].is_object()) return out;

    const json& indicators = result["indicators"];
    if (!indicators.contains("quote") || !indicators["quote"].is_array() || indicators["quote"].empty()) {
        std::cerr << "YahooFinanceSource: response for " << ticker << " has no quote block\n";
        return out;
    }

    const json& timestamps = result["timestamp"];
    const json& quote = indicators["quote"][0];
    auto arrayOrNull = [&quote](const char* key) -> const json& {
        if (!quote.contains(key) || !quote[key].is_array()) return emptyArray();
        return quote[key];
    };
    const json& opens   = arrayOrNull("open");
    const json& highs   = arrayOrNull("high");
    const json& lows    = arrayOrNull("low");
    const json& closes  = arrayOrNull("close");
    const json& volumes = arrayOrNull("volume");

    // The OHLC arrays are index-parallel with `timestamp`, but nothing in the
    // wire format guarantees it, and a short array read past its end is silent
    // UB in a release build. Clamp to the shortest.
    size_t n = timestamps.size();
    n = std::min(n, opens.size());
    n = std::min(n, highs.size());
    n = std::min(n, lows.size());
    n = std::min(n, closes.size());
    if (n == 0) {
        std::cerr << "YahooFinanceSource: response for " << ticker << " has an incomplete quote block\n";
        return out;
    }

    // `quote[0]` is the RAW series: it carries split adjustments but NOT
    // dividends, so an unadjusted dividend payer prints a mechanical gap down
    // on every ex-div date, which momentum/trend strategies read as selling
    // pressure - and total return comes out short by the entire dividend
    // stream. `adjclose` folds both in. Yahoo publishes no per-bar adjustment
    // factor, so recover it as adjclose/close and apply it to the whole bar:
    // that reproduces a total-return series while keeping the intrabar
    // geometry (high/low relative to open/close) intact. Volume is
    // deliberately NOT scaled - it is a share count, not a price.
    bool haveAdj = false;
    if (adjustForSplitsAndDividends && indicators.contains("adjclose") &&
        indicators["adjclose"].is_array() && !indicators["adjclose"].empty() &&
        indicators["adjclose"][0].is_object() &&
        indicators["adjclose"][0].contains("adjclose") &&
        indicators["adjclose"][0]["adjclose"].is_array()) {
        haveAdj = indicators["adjclose"][0]["adjclose"].size() >= n;
    }
    if (adjustForSplitsAndDividends && !haveAdj) {
        std::cerr << "YahooFinanceSource: no usable adjclose for " << ticker
                  << "; falling back to raw prices (dividends will NOT be reflected)\n";
    }
    const json& adjcloses = haveAdj ? indicators["adjclose"][0]["adjclose"] : emptyArray();

    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (timestamps[i].is_null() || opens[i].is_null() || highs[i].is_null() ||
            lows[i].is_null() || closes[i].is_null()) {
            continue; // Yahoo emits null rows for non-trading days within the range
        }
        Candle c;
        c.timestamp = timestamps[i].get<int64_t>();
        c.open = opens[i].get<double>();
        c.high = highs[i].get<double>();
        c.low = lows[i].get<double>();
        c.close = closes[i].get<double>();
        c.volume = (i < volumes.size() && !volumes[i].is_null()) ? volumes[i].get<double>() : 0.0;

        if (haveAdj && !adjcloses[i].is_null()) {
            double adj = adjcloses[i].get<double>();
            // Guard the division: a zero/negative/non-finite factor would
            // silently flatten a bar, which validation would then reject for
            // the wrong reason.
            if (std::isfinite(adj) && adj > 0.0 && std::isfinite(c.close) && c.close > 0.0) {
                double ratio = adj / c.close;
                c.open *= ratio;
                c.high *= ratio;
                c.low  *= ratio;
                c.close = adj;
            }
        }
        out.push_back(c);
    }

    if (!includeForming && !out.empty()) {
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        const int64_t barSeconds = barSecondsFromInterval(interval);
        const int64_t lastTs = out.back().timestamp;

        // When does this bar actually END? That is the only question, and the
        // nominal "timestamp + one period" answer is wrong in both directions
        // for equities, because a daily bar is stamped at the session OPEN and
        // finishes at the session CLOSE:
        //   - too late: it calls a finished daily bar "forming" for the ~17.5h
        //     between the close and the next stamp+24h;
        //   - too early is not the issue, but the *same* miscount silently
        //     discarded YESTERDAY's long-closed bar on every fetch made before
        //     roughly the session open, because 23h elapsed is still < 24h.
        //     That is a genuinely closed bar thrown away on every pre-open or
        //     early-session run.
        //
        // So place the bar in a session and take the earlier of the two ends.
        // A bar belonging to the session the response describes cannot outlive
        // that session, so its end is min(nominal end, session close) - which
        // fixes daily bars without breaking intraday ones, whose nominal end
        // is the binding one mid-session. A bar stamped before the current
        // session's open belongs to an earlier session, and an earlier session
        // has closed by definition of "current" - whatever the clock says
        // about today. Weekly/monthly bars span several sessions, so a session
        // window says nothing about them; those keep the nominal rule.
        int64_t barEnd = lastTs + barSeconds;
        int64_t sessionStart = 0, sessionEnd = 0;
        if (barSeconds <= 86400 && readRegularSession(result, sessionStart, sessionEnd)) {
            if (lastTs >= sessionStart && lastTs <= sessionEnd) {
                barEnd = std::min(barEnd, sessionEnd);
            } else if (lastTs < sessionStart) {
                barEnd = std::min(barEnd, now); // earlier session: already closed
            }
            // lastTs > sessionEnd: the bar sits outside the window Yahoo
            // described (stale meta, or a venue whose session we cannot place)
            // - keep the nominal end rather than guess.
        }
        const bool forming = now < barEnd;

        if (forming) {
            // Never persist this one: CandleStore is append-only and later
            // fetches filter on `timestamp <= lastTimestamp()`, so a partial
            // bar written once keeps its partial OHLCV forever.
            std::cerr << "YahooFinanceSource: dropping still-forming " << interval << " bar for "
                      << ticker << " at ts=" << lastTs << "\n";
            out.pop_back();
        }
    }

    return out;
}

} // namespace trader
