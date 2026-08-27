#pragma once
#include <string>
#include <vector>
#include <utility>
#include <optional>
#include <stdexcept>

namespace trader {

// How an order should be handled by the exchange's matching engine.
//   ImmediateOrCancel - fill whatever is available immediately at/better
//                       than the given price, cancel the remainder. This
//                       was the *only* mode the legacy tool used ("agressive
//                       buy/sell all").
//   GoodTillCancelled - a normal resting limit order; stays open until
//                       filled or explicitly cancelled via cancelOrder().
//   PostOnly          - like GTC, but rejected instead of filled if it would
//                       take liquidity immediately (maker-only, avoids
//                       taker fees).
//   FillOrKill        - fill the entire amount immediately or cancel the
//                       whole order (no partial fills).
enum class TimeInForce { ImmediateOrCancel, GoodTillCancelled, PostOnly, FillOrKill };

enum class OrderStatus { Filled, PartiallyFilled, Open, Cancelled, Rejected };

inline std::string toString(OrderStatus s) {
    switch (s) {
        case OrderStatus::Filled: return "filled";
        case OrderStatus::PartiallyFilled: return "partially_filled";
        case OrderStatus::Open: return "open";
        case OrderStatus::Cancelled: return "cancelled";
        case OrderStatus::Rejected: return "rejected";
    }
    return "unknown";
}

// Result of placing an order. For ImmediateOrCancel/FillOrKill this is
// terminal (status is Filled/PartiallyFilled/Cancelled/Rejected right
// away). For GoodTillCancelled/PostOnly orders that don't fill instantly,
// `status` will be Open and `orderId` identifies it for later
// cancelOrder()/openOrders() lookups.
struct OrderResult {
    bool ok = false;
    OrderStatus status = OrderStatus::Rejected;
    std::string orderId;       // exchange order id, empty if never placed/rejected
    double filledAmount = 0.0; // base-asset amount actually filled so far
    double filledPrice = 0.0;  // average fill price of the filled portion
    double fee = 0.0;          // fee paid, in quote currency
    std::string raw;           // raw response / diagnostic message
};

// A currently-open (unfilled or partially-filled) resting order, as
// reported by openOrders().
struct OpenOrder {
    std::string orderId;
    std::string symbol;
    std::string side;   // "buy" | "sell"
    double price = 0.0;
    double amount = 0.0;        // original requested amount
    double filledAmount = 0.0;  // amount filled so far
    int64_t createdAt = 0;
    std::string clientOrderId;  // echoed back by the venue, empty if unknown
};

// What the venue says the account actually holds, split into the two legs
// of one trading symbol.
//
// `ok == false` is NOT "the account is empty" - it is "this client cannot
// know" (the paper client has no venue to ask). Callers must treat that as
// "skip reconciliation and keep the tracked balances", because adopting a
// zero would make the bot believe it is flat while real coins sit on the
// exchange - exactly the failure mode reconciliation exists to prevent.
struct AccountBalances {
    bool ok = false;
    double base = 0.0;   // e.g. BTC of "BTC_USDT"
    double quote = 0.0;  // e.g. USDT of "BTC_USDT"
    std::string raw;     // raw response / diagnostic message
};

// Splits an exchange symbol into its (base, quote) legs: "BTC_USDT" ->
// {"BTC", "USDT"}. Poloniex uses '_', other venues '-' or '/', so all three
// are accepted. A symbol with no separator at all (a bare equity ticker
// coming from the Yahoo side of the repo) is reported as {symbol, "USDT"}:
// the base leg is the only part that is knowable, and the quote name is
// only ever used for logging/diagnostics.
inline std::pair<std::string, std::string> splitSymbol(const std::string& symbol) {
    for (char sep : {'_', '-', '/'}) {
        auto pos = symbol.find(sep);
        if (pos != std::string::npos && pos > 0 && pos + 1 < symbol.size()) {
            return {symbol.substr(0, pos), symbol.substr(pos + 1)};
        }
    }
    return {symbol, "USDT"};
}

// Abstraction over "how orders actually get placed", so the live-trading
// loop (see trading/live_trader.h) is identical whether running in paper
// mode (default, no API keys required) or live mode (real Poloniex orders,
// once keys are supplied). This mirrors the legacy Trader class's buy()/
// sell() but decouples signing/transport from the trading loop, and adds
// what the legacy tool never had: real limit orders (not just aggressive
// IOC market-style fills), the ability to inspect/cancel resting orders,
// client-supplied order ids (so a retry after a crash is idempotent at the
// venue instead of double-buying), and a balance query (so the bot trades
// the account it actually has instead of a fictional $1,000).
class TradingClient {
public:
    virtual ~TradingClient() = default;
    virtual std::string name() const = 0;

    // Places a buy/sell order for `amountBase` at `price`. `tif` controls
    // fill semantics (see TimeInForce above); defaults to
    // ImmediateOrCancel to preserve the original "aggressive market-style"
    // behavior for existing callers.
    //
    // `clientOrderId` is a caller-chosen idempotency key. LiveTrader derives
    // it deterministically from (symbol, side, bar timestamp, attempt) as
    // "ct-<symbol>-<side>-a<attempt>-<barTime>", and persists it *before*
    // sending the order, so a crash between "order sent" and "state saved" is
    // recoverable: the same id on the retry is either rejected as a duplicate
    // or found among openOrders(), instead of silently placing the trade
    // twice. The attempt counter advances only once the venue has reported an
    // outcome, so a deliberate follow-up order (selling the remainder after a
    // partial fill, on the same bar) gets a fresh id while an unresolved one
    // gets the same id again. Empty means "don't send one".
    virtual OrderResult buy(const std::string& symbol, double price, double amountBase,
                             TimeInForce tif = TimeInForce::ImmediateOrCancel,
                             const std::string& clientOrderId = "") = 0;
    virtual OrderResult sell(const std::string& symbol, double price, double amountBase,
                              TimeInForce tif = TimeInForce::ImmediateOrCancel,
                              const std::string& clientOrderId = "") = 0;

    // Cancels a resting order previously returned as Open by buy()/sell().
    // Returns true if the cancel was accepted (order no longer resting).
    virtual bool cancelOrder(const std::string& symbol, const std::string& orderId) = 0;

    // Lists currently-open (unfilled/partially-filled) orders for `symbol`.
    virtual std::vector<OpenOrder> openOrders(const std::string& symbol) = 0;

    // Real balances from the venue. ok=false means "this client cannot know"
    // (paper mode), which callers must treat as "skip reconciliation", NOT
    // as "balances are zero".
    virtual AccountBalances balances(const std::string& symbol) = 0;
};

class TradingClientError : public std::runtime_error {
public:
    explicit TradingClientError(const std::string& msg) : std::runtime_error(msg) {}
};

} // namespace trader
