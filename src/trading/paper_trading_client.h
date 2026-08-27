#pragma once
#include "trading_client.h"
#include <map>
#include <ctime>

namespace trader {

// Simulated trading client: always available, requires no API keys.
//
// Applies the same fee AND slippage model as the backtest engine, so paper
// results are directly comparable to backtest reports. That second half used
// to be missing: paper fills happened at exactly the requested price while
// BacktestConfig::slippagePct charged 5bps per side, which made paper
// trading ~10bps per round trip *cheaper* than the simulation it exists to
// validate. A paper run that is quietly more profitable than the backtest
// cannot falsify anything, so the friction is now identical:
//     buy  fills at price * (1 + slippagePct)
//     sell fills at price * (1 - slippagePct)
//     fee  = filledPrice * amount * feePct
//
// IOC/FOK orders fill instantly (they cross the book, hence the slippage).
// GTC/PostOnly orders are simulated as resting: they never auto-fill (there's
// no real order book here to know when the price crosses them), but they
// show up via openOrders() and can be cancelled via cancelOrder() - this
// lets the rest of the trading loop (and any strategy logic that places
// resting limit orders) be exercised in paper mode exactly like live mode.
// A resting order is a maker order by definition, so it is *not* charged
// slippage if it is ever filled.
class PaperTradingClient : public TradingClient {
public:
    explicit PaperTradingClient(double feePct = 0.0015, double slippagePct = 0.0005)
        : feePct_(feePct), slippagePct_(slippagePct) {}

    std::string name() const override { return "paper"; }

    OrderResult buy(const std::string& symbol, double price, double amountBase,
                     TimeInForce tif = TimeInForce::ImmediateOrCancel,
                     const std::string& clientOrderId = "") override {
        return place(symbol, "buy", price, amountBase, tif, clientOrderId);
    }

    OrderResult sell(const std::string& symbol, double price, double amountBase,
                      TimeInForce tif = TimeInForce::ImmediateOrCancel,
                      const std::string& clientOrderId = "") override {
        return place(symbol, "sell", price, amountBase, tif, clientOrderId);
    }

    bool cancelOrder(const std::string& symbol, const std::string& orderId) override {
        (void)symbol;
        return openOrders_.erase(orderId) > 0;
    }

    std::vector<OpenOrder> openOrders(const std::string& symbol) override {
        std::vector<OpenOrder> result;
        for (auto& [id, o] : openOrders_) {
            if (o.symbol == symbol) result.push_back(o);
        }
        return result;
    }

    // Paper mode has no venue to ask, so it reports "unknown" rather than
    // zero. LiveTrader must skip reconciliation on this, not wipe its
    // simulated balances to nothing.
    AccountBalances balances(const std::string& symbol) override {
        (void)symbol;
        AccountBalances b;
        b.ok = false;
        b.raw = "paper client has no venue balances";
        return b;
    }

private:
    OrderResult place(const std::string& symbol, const std::string& side, double price,
                       double amountBase, TimeInForce tif, const std::string& clientOrderId) {
        OrderResult r;
        if (tif == TimeInForce::GoodTillCancelled || tif == TimeInForce::PostOnly) {
            std::string id = "paper-" + std::to_string(nextOrderId_++);
            OpenOrder o;
            o.orderId = id;
            o.symbol = symbol;
            o.side = side;
            o.price = price;
            o.amount = amountBase;
            o.createdAt = static_cast<int64_t>(std::time(nullptr));
            o.clientOrderId = clientOrderId;
            openOrders_[id] = o;

            r.ok = true;
            r.status = OrderStatus::Open;
            r.orderId = id;
            r.raw = "paper-resting-order";
            return r;
        }

        // ImmediateOrCancel / FillOrKill: instant fill, charged the same
        // adverse price move the backtest charges a taker.
        double fillPrice = side == "buy" ? price * (1.0 + slippagePct_)
                                          : price * (1.0 - slippagePct_);
        r.ok = true;
        r.status = OrderStatus::Filled;
        r.orderId = "paper-" + std::to_string(nextOrderId_++);
        r.filledAmount = amountBase;
        r.filledPrice = fillPrice;
        r.fee = fillPrice * amountBase * feePct_;
        r.raw = "paper-fill";
        return r;
    }

    double feePct_;
    double slippagePct_;
    std::map<std::string, OpenOrder> openOrders_;
    long nextOrderId_ = 1;
};

} // namespace trader
