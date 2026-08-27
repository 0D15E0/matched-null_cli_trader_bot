#pragma once
#include "../core/candle.h"
#include "../concurrency/rate_limiter.h"
#include <string>
#include <vector>
#include <optional>

namespace trader {

// Fetches PUBLIC candlestick (OHLCV) data from Poloniex's public REST API.
// No API key/secret required for this endpoint - this class is only used
// for backtesting data acquisition, never for trading.
//
// Endpoint: GET https://api.poloniex.com/markets/{symbol}/candles
//   ?interval=<INTERVAL>&limit=<n>&startTime=<ms>&endTime=<ms>
//
// TODO(bot-mode): the same host also exposes a WebSocket market-data feed
// which would be far more efficient than REST polling for keeping candles
// "warm" in a live bot (single persistent connection vs. repeated GETs).
// Not implemented.
class PoloniexSource {
public:
    explicit PoloniexSource(RateLimiter& limiter) : limiter_(limiter) {}

    // periodSeconds must be one of the supported Poloniex granularities:
    // 300, 900, 1800, 7200, 14400, 86400 (5m,15m,30m,2h,4h,1d).
    static std::string intervalFromPeriod(int64_t periodSeconds);

    // Fetches all candles for `symbol`/`periodSeconds` starting strictly
    // after `sinceUnixSeconds` (use std::nullopt for full history) up to
    // now. Handles pagination transparently (Poloniex caps each response
    // to a max page size).
    //
    // Pagination is bounded in both directions that matter: the backward walk
    // normally advances to the oldest startTime on each page, but a page whose
    // rows the parser cannot read at all (schema drift) yields no startTime to
    // advance to, so the walk steps back a whole page and, after a few such
    // pages in a row, gives up loudly with possibly-incomplete history rather
    // than re-issuing the same request forever. An absolute page cap backs
    // that up.
    //
    // By default the still-forming candle is DROPPED. This is not a nicety:
    // because the endpoint pages backward from `now`, the in-progress candle
    // is always in the first page, and because CandleStore is append-only and
    // later fetches filter on `timestamp <= since`, a partial bar written once
    // is frozen at its partial OHLCV forever. It happened - the shipped BTC
    // store's last 4h bar was captured 66 minutes into its window, with ~27%
    // of a normal bar's volume. Under live polling nearly every stored bar
    // would become a near-open partial.
    //
    // includeForming=true opts back in (useful only for a live view that
    // never persists what it sees). nowUnixSeconds==0 means "use the wall
    // clock"; the parameter exists so tests can pin the cut-off.
    //
    // maxPages bounds how far back the walk reaches (500 bars per page), for
    // callers that deliberately want a shallow fetch; 0 means the built-in
    // safety cap only. A SHORT page does NOT end the walk: Poloniex returns
    // short pages at internal gaps (maintenance windows) with years of older
    // history behind them - measured: the 15m walk used to stop at 469 days
    // on such a gap when the venue actually serves 8+ years. Only an EMPTY
    // page (retried once, in case it was transient) means the history is
    // exhausted.
    std::vector<Candle> fetchHistory(const std::string& symbol,
                                      int64_t periodSeconds,
                                      std::optional<int64_t> sinceUnixSeconds,
                                      bool includeForming = false,
                                      int64_t nowUnixSeconds = 0,
                                      int maxPages = 0);

private:
    RateLimiter& limiter_;
};

} // namespace trader
