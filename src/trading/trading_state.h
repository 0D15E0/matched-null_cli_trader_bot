#pragma once
#include "../strategy/strategy.h"
#include <string>
#include <fstream>
#include <sstream>
#include <optional>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <ctime>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace trader {

// Modern, structured replacement for the legacy PM_CONSOL "trade_state<PAIR>_0"
// plain-text file (line1=state, line2=btc, line3=money) and its shell-script
// based inspection (status.sh / state_example). This is a small JSON document,
// human-readable, and queryable both by the `status` CLI subcommand and by
// external tooling (jq, monitoring dashboards, etc.) without needing to parse
// positional text lines.
enum class PositionState { Scanning, Bought };

inline std::string toString(PositionState s) {
    return s == PositionState::Bought ? "bought" : "scanning";
}
inline PositionState positionStateFromString(const std::string& s) {
    return s == "bought" ? PositionState::Bought : PositionState::Scanning;
}

struct TradingState {
    std::string symbol;
    std::string strategy;
    std::string profile;
    std::string mode = "paper"; // "paper" or "live"
    PositionState state = PositionState::Scanning;

    double baseBalance = 0.0;   // e.g. BTC held
    double quoteBalance = 0.0;  // e.g. USDT cash
    double entryPrice = 0.0;    // price at which current position was opened (if Bought)
    double lastPrice = 0.0;

    // Cost basis of the OPEN position, in quote currency. `entryOutlay` is the
    // total cash that actually left the account to open it (gross + entry
    // fee); `entryFee` is the fee part of that, kept separately so a trade can
    // report what it cost to get in.
    //
    // Both exist because realized P&L must be measured against total outlay,
    // not against price*amount. The engine charges the entry fee to the trade
    // (engine.cpp executeSell: pnlAbs = proceeds - entryOutlay); the live side
    // used entryPrice*filledAmount and so quietly reported every round trip as
    // one entry fee better than it was - the exact accounting error Phase 0
    // fixed in the backtest, reintroduced live. At a 0.15% fee that is a
    // systematic overstatement of both realized P&L and the win rate, which
    // makes live results incomparable with the backtest they exist to confirm.
    double entryOutlay = 0.0;
    double entryFee = 0.0;

    int64_t startedAt = 0;
    int64_t lastPollAt = 0;
    int64_t lastSignalAt = 0;
    // What the last completed cycle did. Beyond "buy"/"sell"/"hold"/"none" it
    // also carries the reasons a cycle did NOT trade - "halted-balance-mismatch",
    // "buy-skipped-no-vol-estimate", "buy-skipped-insufficient-quote",
    // "sell-skipped-dust", "replaying-backlog", "awaiting-order:<id>",
    // "buy-failed", "sell-partial", ... - because a bot that is quietly not
    // trading looks exactly like a bot with nothing to do.
    std::string lastAction = "none";
    std::string lastError;

    // totalTrades counts FILLS (buys and sells alike); roundTrips counts
    // completed buy->sell cycles, which is the only thing that can be
    // classified win or loss. Reporting wins/totalTrades - which is what
    // `status` used to print - caps the win rate at 50% by construction: a
    // perfect strategy that never loses would report 50%, because half of its
    // fills are the buys nobody can win or lose on. The denominator has to be
    // the number of round trips actually completed.
    int totalTrades = 0;
    int roundTrips = 0;
    int wins = 0;
    int losses = 0;
    double realizedPnl = 0.0;

    // Replay bookmark: the newest closed bar that has already been fed to
    // the strategy. Everything after it gets replayed on the next cycle, so
    // bars that closed while the process was down are still seen (the old
    // loop evaluated only the final bar, which silently dropped every
    // edge-triggered signal - an sma_cross crossover that happened during a
    // restart simply never existed as far as the bot was concerned).
    int64_t lastProcessedTimestamp = 0;

    // The position the STRATEGY believes it holds. Distinct from the account
    // balances above: it survives restarts so a trailing stop keeps its
    // reference price, and it is what makes replay deterministic. Before
    // PositionContext existed this state lived inside the strategy object
    // and was destroyed by every prepare() call, which is why stateful
    // strategies (odiseo, pure_ichimoku) could buy live but never sell.
    bool    stratInPosition = false;
    double  stratEntryPrice = 0.0;
    double  stratHighestClose = 0.0;
    double  stratLowestClose = 0.0;
    int64_t stratEntryTime = 0;

    // In-flight order guard (crash between placing an order and saving state).
    // Written and persisted BEFORE the order goes out, so a restart can ask
    // the venue "did this actually land?" instead of blindly re-sending it.
    std::string pendingClientOrderId;
    std::string pendingSide;      // "buy" | "sell" | ""
    int64_t     pendingSince = 0;

    // Monotonic count of orders the venue has confirmed receiving. It is the
    // attempt component of the client order id, and it exists because
    // (symbol, side, bar) alone cannot be both an idempotency key and a retry
    // key: after a PARTIAL fill the bot must send a second order for the same
    // bar, and reusing the first order's id gets it rejected as a duplicate
    // (or, on a venue that does not dedupe, doubles the position). Persisted
    // so the id stays deterministic across a restart.
    int orderAttempt = 0;

    // The strategy's notional position, in the form the Strategy interface
    // wants it. entryIndex is deliberately NOT persisted: it is an index
    // into whatever series happened to be loaded, and a store rebuild or a
    // sliced load would silently shift it. LiveTrader recovers it by
    // looking stratEntryTime up in the series it just loaded, which is the
    // only definition that stays true across restarts.
    PositionContext toPositionContext() const {
        PositionContext p;
        p.inPosition = stratInPosition;
        p.entryPrice = stratEntryPrice;
        p.highestClose = stratHighestClose;
        p.lowestClose = stratLowestClose;
        p.entryTime = stratEntryTime;
        p.entryIndex = 0;
        return p;
    }

    void setPositionContext(const PositionContext& p) {
        stratInPosition = p.inPosition;
        stratEntryPrice = p.entryPrice;
        stratHighestClose = p.highestClose;
        stratLowestClose = p.lowestClose;
        stratEntryTime = p.entryTime;
    }

    nlohmann::json toJson() const {
        nlohmann::json j;
        j["symbol"] = symbol;
        j["strategy"] = strategy;
        j["profile"] = profile;
        j["mode"] = mode;
        j["state"] = toString(state);
        j["base_balance"] = baseBalance;
        j["quote_balance"] = quoteBalance;
        j["entry_price"] = entryPrice;
        j["entry_outlay"] = entryOutlay;
        j["entry_fee"] = entryFee;
        j["last_price"] = lastPrice;
        j["started_at"] = startedAt;
        j["last_poll_at"] = lastPollAt;
        j["last_signal_at"] = lastSignalAt;
        j["last_action"] = lastAction;
        j["last_error"] = lastError;
        j["total_trades"] = totalTrades;   // fills, both sides
        j["round_trips"] = roundTrips;     // completed buy->sell cycles
        j["wins"] = wins;
        j["losses"] = losses;
        j["win_rate_pct"] = roundTrips > 0 ? (100.0 * wins / roundTrips) : 0.0;
        j["realized_pnl"] = realizedPnl;
        j["last_processed_timestamp"] = lastProcessedTimestamp;
        j["strat_in_position"] = stratInPosition;
        j["strat_entry_price"] = stratEntryPrice;
        j["strat_highest_close"] = stratHighestClose;
        j["strat_lowest_close"] = stratLowestClose;
        j["strat_entry_time"] = stratEntryTime;
        j["pending_client_order_id"] = pendingClientOrderId;
        j["pending_side"] = pendingSide;
        j["pending_since"] = pendingSince;
        j["order_attempt"] = orderAttempt;
        return j;
    }

    // Every field is read with a default, so a state file written by an
    // older build still loads (it simply starts with an empty replay
    // bookmark and a flat notional position, which the first cycle fixes).
    static TradingState fromJson(const nlohmann::json& j) {
        TradingState s;
        s.symbol = j.value("symbol", "");
        s.strategy = j.value("strategy", "");
        s.profile = j.value("profile", "");
        s.mode = j.value("mode", "paper");
        s.state = positionStateFromString(j.value("state", "scanning"));
        s.baseBalance = j.value("base_balance", 0.0);
        s.quoteBalance = j.value("quote_balance", 0.0);
        s.entryPrice = j.value("entry_price", 0.0);
        s.lastPrice = j.value("last_price", 0.0);
        // Old state files predate the cost basis. Reconstruct it from the
        // recorded entry price rather than leaving it at zero, so a position
        // opened by a previous build still gets a defensible (fee-free, hence
        // slightly optimistic) basis instead of an undefined one.
        s.entryOutlay = j.value("entry_outlay", s.entryPrice * s.baseBalance);
        s.entryFee = j.value("entry_fee", 0.0);
        s.startedAt = j.value("started_at", (int64_t)0);
        s.lastPollAt = j.value("last_poll_at", (int64_t)0);
        s.lastSignalAt = j.value("last_signal_at", (int64_t)0);
        s.lastAction = j.value("last_action", "none");
        s.lastError = j.value("last_error", "");
        s.totalTrades = j.value("total_trades", 0);
        s.wins = j.value("wins", 0);
        s.losses = j.value("losses", 0);
        // wins/losses were only ever incremented on the fill that flattened a
        // position, so their sum IS the historical round-trip count - an old
        // state file therefore keeps a correct win rate rather than a zero one.
        s.roundTrips = j.value("round_trips", s.wins + s.losses);
        s.realizedPnl = j.value("realized_pnl", 0.0);
        s.lastProcessedTimestamp = j.value("last_processed_timestamp", (int64_t)0);
        s.stratInPosition = j.value("strat_in_position", false);
        s.stratEntryPrice = j.value("strat_entry_price", 0.0);
        s.stratHighestClose = j.value("strat_highest_close", 0.0);
        s.stratLowestClose = j.value("strat_lowest_close", 0.0);
        s.stratEntryTime = j.value("strat_entry_time", (int64_t)0);
        s.pendingClientOrderId = j.value("pending_client_order_id", "");
        s.pendingSide = j.value("pending_side", "");
        s.pendingSince = j.value("pending_since", (int64_t)0);
        s.orderAttempt = j.value("order_attempt", 0);
        return s;
    }

    // Atomic save: write <path>.tmp, fsync it, copy the current file aside
    // as <path>.bak, then rename the temp over the real path.
    //
    // The old implementation opened the real file with ios::trunc and wrote
    // straight into it, so a crash (or a kill -9 from a supervisor) during
    // the write left a truncated, unparseable JSON document - and load()
    // then quietly handed back a fresh state, i.e. "flat, $1,000, no
    // position", while the exchange still held the coins. rename(2) is
    // atomic within a filesystem, so a reader now sees either the whole old
    // file or the whole new one and never a half-written one.
    void save(const std::string& path) const {
        namespace fs = std::filesystem;
        auto parent = fs::path(path).parent_path();
        if (!parent.empty()) fs::create_directories(parent);

        const std::string tmp = path + ".tmp";
        const std::string bak = path + ".bak";
        {
            std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
            if (!out) throw std::runtime_error("cannot open state temp file for writing: " + tmp);
            out << toJson().dump(2) << "\n";
            out.flush();
            if (!out) throw std::runtime_error("failed writing state temp file: " + tmp);
        }
        // flush() only reaches the OS page cache; fsync is what survives a
        // power loss, and a bot that trades real money is exactly the case
        // where the extra syscall is worth it.
        int fd = ::open(tmp.c_str(), O_RDONLY);
        if (fd >= 0) { ::fsync(fd); ::close(fd); }

        std::error_code ec;
        if (fs::exists(path, ec)) {
            fs::copy_file(path, bak, fs::copy_options::overwrite_existing, ec);
            // A failed backup is not fatal (the rename below is still
            // atomic); it only costs us the second-chance parse in load().
        }
        fs::rename(tmp, path);
    }

    // Returns std::nullopt ONLY when there is no state file at all (first
    // run). A file that exists but does not parse is an error, not an
    // absence: the previous code caught every exception and returned
    // nullopt, so a corrupt state file made the bot start over as
    // "Scanning with $1,000" and buy again on top of a position it already
    // held. We try the .bak written by the previous save first, and if that
    // is unusable too we throw and let the operator look at it.
    static std::optional<TradingState> load(const std::string& path) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(path, ec)) return std::nullopt;

        std::string mainErr;
        if (auto s = parseFile(path, mainErr)) return s;

        const std::string bak = path + ".bak";
        std::string bakErr = "no .bak file present";
        if (fs::exists(bak, ec)) {
            if (auto s = parseFile(bak, bakErr)) return s;
        }
        throw std::runtime_error(
            "state file '" + path + "' exists but could not be parsed (" + mainErr +
            "), and the backup '" + bak + "' did not help (" + bakErr + "). Refusing to "
            "start from a blank state, because that would forget any open position and "
            "trade against balances that do not exist. Inspect the file, repair it, or "
            "delete it deliberately once you have confirmed the account is flat.");
    }

private:
    static std::optional<TradingState> parseFile(const std::string& path, std::string& err) {
        try {
            std::ifstream in(path);
            if (!in.good()) { err = "cannot open for reading"; return std::nullopt; }
            nlohmann::json j;
            in >> j;
            return fromJson(j);
        } catch (const std::exception& e) {
            err = e.what();
            return std::nullopt;
        } catch (...) {
            err = "unknown parse error";
            return std::nullopt;
        }
    }
};

inline int64_t nowUnix() {
    return static_cast<int64_t>(std::time(nullptr));
}

} // namespace trader
