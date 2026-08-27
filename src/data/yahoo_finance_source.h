#pragma once
#include "../core/candle.h"
#include "../concurrency/rate_limiter.h"
#include <string>
#include <vector>

namespace trader {

// Fetches PUBLIC daily (or intraday) OHLCV candles from Yahoo Finance's
// public "chart" endpoint. Used for equities/indices (e.g. "ASML.AS" for
// ASML on Euronext Amsterdam, "AAPL" for Apple on NASDAQ) where Poloniex
// (crypto-only) doesn't apply. No API key required.
//
// Endpoint: GET https://query1.finance.yahoo.com/v8/finance/chart/{ticker}
//   ?range=<range>&interval=<interval>
//
// Unlike PoloniexSource, Yahoo's chart endpoint doesn't support arbitrary
// start/end pagination for free-form incremental backfill as cleanly, so
// this source simply fetches the widest available `range` in one shot and
// lets CandleStore::append() de-duplicate against what's already stored.
//
// Because every fetch returns the ENTIRE history re-based to today's split
// and dividend adjustments, the overlap between a fresh fetch and the store
// is also the split detector: see CandleStore::compareOverlap().
class YahooFinanceSource {
public:
    explicit YahooFinanceSource(RateLimiter& limiter) : limiter_(limiter) {}

    // interval: "1d", "1wk", "1mo", or intraday ("5m","15m","30m","1h",
    // limited history availability upstream). range: "1y","5y","10y","max".
    //
    // adjustForSplitsAndDividends (default true) scales O/H/L/C by the
    // per-bar `adjclose/close` ratio, leaving volume alone. `quote[0]` is the
    // RAW series: it carries split adjustments but NOT dividends, so an
    // unadjusted dividend payer shows a mechanical gap down on every ex-div
    // date. Strategies read those gaps as selling pressure, and total return
    // comes out understated by the entire dividend stream. Pass false only if
    // you specifically want the as-traded prices.
    //
    // includeForming (default false): Yahoo returns a bar for the session in
    // progress. Storing it freezes a partial bar forever (CandleStore is
    // append-only), so by default the last bar is dropped whenever it has not
    // finished. "Not finished" is decided from the response's own
    // meta.currentTradingPeriod.regular window where available: the last bar
    // is forming only if it BELONGS to that session (timestamp at or after its
    // open) and the session has not closed yet. A bar from an earlier session
    // is complete no matter what today's clock says - checking only "is the
    // session end in the future" dropped yesterday's finished bar on every
    // pre-open fetch.
    std::vector<Candle> fetchHistory(const std::string& ticker,
                                      const std::string& interval = "1d",
                                      const std::string& range = "max",
                                      bool adjustForSplitsAndDividends = true,
                                      bool includeForming = false);

private:
    RateLimiter& limiter_;
};

} // namespace trader
