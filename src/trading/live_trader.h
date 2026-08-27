#pragma once
#include "../core/candle_store.h"
#include "../data/poloniex_source.h"
#include "../concurrency/rate_limiter.h"
#include "../indicators/indicators.h"
#include "../indicators/vol_forecast.h"
#include "../strategy/strategy.h"
#include "trading_client.h"
#include "trading_state.h"
#include "trade_log.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <sstream>
#include <csignal>
#include <atomic>
#include <memory>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <filesystem>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace trader {

// Global flag flipped by SIGINT/SIGTERM handlers so the polling loop can
// shut down gracefully (save state, exit) instead of being killed mid-cycle
// - the legacy code relied on start-stop-daemon + external process kill
// with no graceful shutdown hook at all.
inline std::atomic<bool>& liveTraderStopFlag() {
    static std::atomic<bool> stop{false};
    return stop;
}

struct LiveTraderConfig {
    std::string symbol;
    int64_t periodSeconds = 300;   // candle granularity to trade on
    int pollIntervalSeconds = 60;  // how often to check for a new candle
    std::string dataDir = "data";
    std::string stateDir = "state";
    double feePct = 0.0015;
    double slippagePct = 0.0005;   // must match BacktestConfig::slippagePct
    double startingQuoteBalance = 1000.0; // paper mode initial cash
    // Replay budget per cycle; 0 = unlimited, which is the default because
    // capping it only slows cold starts without buying any safety. Replaying
    // a bar is O(1) (prepare() has already computed the indicators over the
    // whole series either way), and no matter how many bars are replayed the
    // cycle still places AT MOST ONE order - the replay decides what position
    // the strategy wants, not how many trades to fire. Capping at 1000 meant
    // a fresh bot on 25k bars of stored history sat in "replaying-backlog",
    // refusing to trade, for 25 poll cycles. Set a positive value only if you
    // deliberately want to bound per-cycle work on a very slow machine.
    int maxBarsPerCycle = 0;
    bool reconcileBalances = true; // re-query venue balances every cycle

    // Volatility targeting, identical in meaning to BacktestConfig's fields of
    // the same names and applied by the same rule (see sizeFraction below).
    // These are not a nicety: without them the live bot is unconditionally
    // all-in while every validated `--vol-target` backtest is not, so the
    // report and the bot describe two different strategies - and the one with
    // the 65% drawdown is the one holding the money. 0 disables targeting and
    // restores all-in sizing, matching BacktestConfig's default.
    double volTargetAnnual = 0.0;
    int    volWindow = 30;
    double maxPositionFraction = 1.0;

    // Which volatility model sizes the position. Must match whatever the
    // backtest that justified this configuration was run with, or the live bot
    // is executing a strategy nobody validated.
    indicators::VolForecastConfig volForecast;

    // Opt-in: let the bot treat base currency it did not buy as its own
    // position. Default OFF, because the alternative is a bot that sells a
    // manual holding, another strategy's inventory, or a second bot's position
    // the first time its strategy says Sell. Turn it on only when you
    // deliberately want this bot to take over whatever the account holds.
    bool adoptVenueBasePosition = false;

    // Cap on how much of the account's quote currency THIS sleeve may treat
    // as its own. 0 = off (adopt the venue's full balance, the original
    // behaviour, correct for a single-sleeve account).
    //
    // Why this exists: reconciliation adopts the venue's quote balance
    // outright, and sizing budgets against it. Run eight sleeves on one
    // account and each believes it owns the whole cash pool - the first
    // signals to fire deploy the entire book, and the equal-weight design
    // every backtest measured exists only on paper. With a cap, each sleeve
    // sizes against min(venue quote, its share). A sleeve's own profits
    // accumulate in the shared pool rather than raising its cap - a
    // conservative drift, and the operator can raise caps deliberately.
    double quoteCap = 0.0;
};

// Per-symbol advisory lock, RAII.
//
// Two `cli_trader run` processes on the same symbol is not a theoretical
// problem: they poll the same candles, reach the same signal, and each
// places its own order (so the account buys twice), then both write the
// same state file and the loser's version is whatever landed last. The
// legacy PM_CONSOL design "solved" this by convention - one daemon per pair,
// started by hand, verified with `ps aux | grep PM`. An O_CREAT|O_EXCL file
// is the same idea made enforceable: the create either wins or it doesn't,
// with no window in between.
//
// The lock names its owner's pid and checks it, so a lock left behind by a
// killed process is reclaimed instead of blocking every future run - see
// reclaimIfStale().
class SymbolLock {
public:
    explicit SymbolLock(std::string path) : path_(std::move(path)) {
        auto parent = std::filesystem::path(path_).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);

        // Two tries at most: the second one only happens after a stale lock has
        // been reclaimed, and if it also loses the race then a live process
        // really did take the lock in between and we must refuse.
        for (int attempt = 0; attempt < 2; ++attempt) {
            int fd = ::open(path_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
            if (fd >= 0) {
                std::string pid = std::to_string(static_cast<long>(::getpid())) + "\n";
                ssize_t written = ::write(fd, pid.data(), pid.size());
                (void)written;
                ::close(fd);
                held_ = true;
                return;
            }
            if (errno != EEXIST) {
                throw std::runtime_error("cannot create lock file '" + path_ + "': " +
                                          std::string(std::strerror(errno)));
            }
            if (attempt == 0 && reclaimIfStale()) continue;
            // Reaching here means the lock could NOT be shown to be abandoned:
            // its pid is running, belongs to another user, is unreadable, or a
            // live process grabbed the lock during the reclaim.
            std::string who = readOwner();
            throw std::runtime_error(
                "lock file '" + path_ + "' already exists (pid " + who + ") and this process "
                "could not establish that its owner is gone: another cli_trader appears to be "
                "running for this symbol. Two processes trading one symbol place every order "
                "twice and overwrite each other's state file. Inspect it with 'ps -p " + who +
                "'; stop it before starting another. (A lock naming a pid that is definitely "
                "not running is reclaimed automatically, so this is not the crashed-process "
                "case.) If you are certain nothing is trading this symbol, remove the lock "
                "with 'rm " + path_ + "'.");
        }
    }

    ~SymbolLock() {
        if (!held_) return;
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    SymbolLock(const SymbolLock&) = delete;
    SymbolLock& operator=(const SymbolLock&) = delete;

private:
    std::string readOwner() const {
        std::string owner;
        std::ifstream in(path_);
        std::getline(in, owner);
        while (!owner.empty() && (owner.back() == '\n' || owner.back() == '\r' || owner.back() == ' '))
            owner.pop_back();
        return owner.empty() ? std::string("<unknown>") : owner;
    }

    // A lock file only disappears when its owner's destructor runs, so a
    // SIGKILL, an OOM kill or a power cut leaves one behind - and the bot then
    // refuses to start FOREVER, needing a human to notice and rm it, which for
    // an unattended trading process is an outage rather than a safeguard. The
    // pid was already being written into the file; this actually checks it.
    //
    // kill(pid, 0) sends no signal and only asks "may I signal this process":
    // ESRCH means it is gone, EPERM means it exists but belongs to another
    // user (still alive - refuse). The known race is pid reuse: if the
    // recorded pid has since been recycled by an unrelated process we keep
    // refusing, which is the safe direction to be wrong in.
    bool reclaimIfStale() const {
        std::string owner = readOwner();
        long pid = 0;
        try {
            size_t consumed = 0;
            pid = std::stol(owner, &consumed);
            if (consumed != owner.size()) pid = 0;
        } catch (...) {
            pid = 0;
        }
        if (pid <= 0) {
            // No usable pid (empty or corrupt file): we cannot prove the owner
            // is dead, so we do not get to assume it.
            return false;
        }
        if (pid == static_cast<long>(::getpid())) return false; // us, somehow: never reclaim
        if (::kill(static_cast<pid_t>(pid), 0) == 0) return false;   // alive
        if (errno != ESRCH) return false;                            // EPERM: alive, other user

        std::cerr << "[live_trader] lock file '" << path_ << "' is stale: it names pid " << pid
                  << ", which is not running (the previous run was killed rather than stopped). "
                     "Reclaiming it.\n";
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        return !ec;
    }

    std::string path_;
    bool held_ = false;
};

// Continuous polling + strategy evaluation + (paper or live) order
// execution loop. This is the modern equivalent of the legacy PM_CONSOL
// per-pair daemon process, but as a single long-running subcommand
// (`cli_trader run`) rather than a separately-daemonized binary per pair,
// with structured JSON state/log files instead of plain-text ones.
//
// Design notes vs. legacy:
//  - Legacy: one OS process per pair, status via `ps aux | grep PM` +
//    tailing text files via a shell script (status.sh).
//  - Here: `cli_trader status --symbol X` reads a JSON state file that this
//    loop keeps up to date every cycle (poll time, position, balances,
//    running win/loss stats), so status can be queried by anyone/anything
//    without needing to attach to the process or parse free-text logs.
//    Running multiple symbols concurrently = run multiple `cli_trader run`
//    processes (or extend to multi-symbol in one process later using
//    ThreadPool).
//
// The central design rule, and the reason cycle() looks the way it does:
// **live must be a replay of the backtest, not a different program**. The
// previous implementation sampled - it re-ran prepare() every poll and asked
// the strategy about one bar, the last one. That produced two verified bugs
// which together made backtest numbers meaningless as predictions:
//   * prepare() reset the strategy's internal position, so odiseo /
//     pure_ichimoku could buy live but their exit branch was unreachable;
//   * only the newest bar was ever evaluated, so every edge-triggered signal
//     (sma_cross) that fired while the process was down simply never existed.
// Now the position lives in the state file as a PositionContext, and every
// closed bar that has not been processed yet is fed through onBar() in
// order, exactly as BacktestEngine::run does it.
class LiveTrader {
public:
    LiveTrader(LiveTraderConfig config, std::unique_ptr<Strategy> strategy,
               std::unique_ptr<TradingClient> client, std::string profileName)
        : cfg_(std::move(config)), strategy_(std::move(strategy)),
          client_(std::move(client)), profileName_(std::move(profileName)),
          lock_(cfg_.stateDir + "/" + cfg_.symbol + ".lock"),
          store_(cfg_.dataDir + "/" + cfg_.symbol + "_" + std::to_string(cfg_.periodSeconds) + ".ctc"),
          limiter_(6, 3.0), source_(limiter_),
          statePath_(cfg_.stateDir + "/" + cfg_.symbol + ".state.json"),
          logPath_(cfg_.stateDir + "/" + cfg_.symbol + ".trades.jsonl"),
          tradeLog_(logPath_) {
        // Throws if a state file exists but cannot be parsed. That is
        // deliberate: starting fresh would forget an open position.
        auto loaded = TradingState::load(statePath_);
        if (loaded.has_value()) {
            state_ = *loaded;
        } else {
            state_.symbol = cfg_.symbol;
            state_.mode = client_->name();
            state_.quoteBalance = cfg_.startingQuoteBalance;
            state_.startedAt = nowUnix();
        }
        state_.strategy = strategy_->name();
        state_.profile = profileName_;
        state_.mode = client_->name();
    }

    // Runs until liveTraderStopFlag() is set (e.g. by SIGINT) or maxCycles
    // is reached (0 = run forever). Returns the number of cycles executed.
    int runLoop(int maxCycles = 0) {
        std::cout << "Starting live trader: symbol=" << cfg_.symbol
                  << " period=" << cfg_.periodSeconds << "s mode=" << client_->name()
                  << " strategy=" << state_.strategy << "/" << state_.profile
                  << " poll every " << cfg_.pollIntervalSeconds << "s\n";
        int cycles = 0;
        while (!liveTraderStopFlag().load()) {
            bool caughtUp = true;
            try {
                caughtUp = cycle();
            } catch (const std::exception& e) {
                state_.lastError = e.what();
                std::cerr << "[live_trader] cycle error: " << e.what() << "\n";
                try {
                    state_.save(statePath_);
                } catch (const std::exception& saveErr) {
                    std::cerr << "[live_trader] could not persist state after the error: "
                              << saveErr.what() << "\n";
                }
            }
            ++cycles;
            if (maxCycles > 0 && cycles >= maxCycles) break;
            // A cycle that is still working through a backlog has not made a
            // trading decision yet, so sleeping a full poll interval would
            // only stretch the catch-up over hours. Go straight round again.
            if (!caughtUp) continue;
            for (int i = 0; i < cfg_.pollIntervalSeconds && !liveTraderStopFlag().load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        std::cout << "Live trader stopped after " << cycles << " cycle(s).\n";
        return cycles;
    }

private:
    // A residual worth less than this in quote currency is dust: it is below
    // every venue's minimum order size, so it cannot be sold and must not be
    // treated as an open position - otherwise a partially filled exit leaves
    // the bot permanently "Bought" on a few satoshis and it never trades
    // again.
    static constexpr double kDustQuoteValue = 1.0;
    // IOC limit bound past the fresh price: wide enough to cross a thin
    // book's spread, while the fill itself happens at the book's real price.
    static constexpr double kOrderLimitAggression = 0.01;

    // One poll. Returns true when the replay reached the newest closed bar
    // (i.e. this cycle was able to make a trading decision); false means
    // "still catching up", and runLoop() then polls again immediately.
    bool cycle() {
        bool cycleClean = true;

        // 1. Pull only CLOSED candles. fetchHistory defaults to
        //    includeForming=false, which is the whole point: the old loop
        //    traded the in-progress bar (a 4h BTC bar captured 66 minutes in
        //    has ~27% of its eventual volume and a close that has not
        //    happened yet), and the append-only store then froze that
        //    partial bar forever. Appending is non-strict here because a
        //    live loop must not die over one suspicious bar - the store
        //    drops it and we say so.
        auto since = store_.lastTimestamp();
        auto fresh = source_.fetchHistory(cfg_.symbol, cfg_.periodSeconds, since);
        if (!fresh.empty()) {
            size_t added = store_.append(cfg_.symbol, cfg_.periodSeconds, fresh, /*strict=*/false);
            if (added < fresh.size()) {
                std::cerr << "[live_trader] store rejected " << (fresh.size() - added) << " of "
                          << fresh.size() << " fetched bar(s) for " << cfg_.symbol
                          << " (spacing/OHLC/outlier validation); continuing with the rest\n";
            }
        }

        // 2. Load the series we will replay over.
        CandleSeries series = store_.load();
        state_.lastPollAt = nowUnix();
        if (series.empty()) {
            state_.lastAction = "no-data";
            saveState();
            return true;
        }
        state_.lastPrice = series.close.back();

        // 3./4. Reconcile with reality before deciding anything.
        //
        // An in-flight order comes first: if the process died between
        // "order sent" and "state saved", the marker is still set and we
        // must ask the venue what happened rather than send it again. The
        // clientOrderId makes that question answerable at all.
        if (!state_.pendingClientOrderId.empty()) {
            if (pendingOrderStillOpen()) {
                state_.lastAction = "awaiting-order:" + state_.pendingClientOrderId;
                std::cerr << "[live_trader] order " << state_.pendingClientOrderId
                          << " is still open at the venue; not placing anything this cycle\n";
                saveState();
                return true;
            }
            // Gone from the book: it either filled or was cancelled, and the
            // balances are the only truthful record of which.
            std::cerr << "[live_trader] in-flight order " << state_.pendingClientOrderId
                      << " is no longer open; reconciling balances to find out what it did\n";
            clearPending();
            // ownFillLikely: whatever the balances now show is the doing of an
            // order this bot persisted before sending, so a base increase here
            // is our own fill and not foreign inventory.
            reconcileWithVenue(/*ownFillLikely=*/true);
        }

        // Balances: the venue is the authority on the CASH available and on the
        // upper bound of what can be sold, but not on which coins are this
        // bot's to trade (see reconcileWithVenue). Live mode used to trade a
        // fictional startingQuoteBalance of $1,000 regardless of what the
        // account actually held, which makes every order size wrong and every
        // P&L number a work of fiction. The first cycle always asks; after that
        // it is governed by cfg_.reconcileBalances - note that with it off, a
        // mismatch found on the first cycle is never re-examined and the bot
        // stays halted until restarted.
        if (firstCycle_ || cfg_.reconcileBalances) reconcileWithVenue();
        firstCycle_ = false;

        // 5. Replay every not-yet-processed closed bar through the strategy,
        //    oldest first, carrying the position across bars exactly the way
        //    BacktestEngine::run does.
        strategy_->prepare(series);

        PositionContext position = state_.toPositionContext();
        if (position.inPosition) {
            // entryIndex is not persisted (indices are not stable across
            // store rebuilds); recover it from the entry timestamp, which is.
            position.entryIndex = indexOfTimestamp(series, position.entryTime);
        }

        size_t first = 0;
        while (first < series.size() && series.timestamp[first] <= state_.lastProcessedTimestamp) {
            ++first;
        }
        size_t stop = series.size();
        bool caughtUp = true;
        if (cfg_.maxBarsPerCycle > 0 &&
            stop - first > static_cast<size_t>(cfg_.maxBarsPerCycle)) {
            stop = first + static_cast<size_t>(cfg_.maxBarsPerCycle);
            caughtUp = false;
        }

        for (size_t i = first; i < stop; ++i) {
            // The position sees the bar before the strategy reasons about
            // it, so a trailing stop is measured against a high that
            // includes this close (engine.cpp step 2, verbatim).
            position.observe(series.close[i]);
            Signal signal = strategy_->onBar(series, i, position);
            if (signal == Signal::Buy && !position.inPosition) {
                // The NOTIONAL position opens at this bar's close, not at
                // whatever price the real order later fills at. That keeps
                // the replay deterministic: restarting the bot and replaying
                // the same bar must rebuild the same trailing-stop
                // reference, which it cannot do if the reference depends on
                // execution history. (The backtest opens at the slipped fill
                // price of the *next* bar's open - a deliberately
                // conservative model of the same latency. The account's
                // entryPrice below does use the real fill.)
                position.open(series.close[i], series.timestamp[i], i);
            } else if (signal == Signal::Sell && position.inPosition) {
                position.close();
            }
            state_.lastProcessedTimestamp = series.timestamp[i];
        }
        if (stop > first) state_.lastSignalAt = nowUnix();
        state_.setPositionContext(position);

        if (!caughtUp) {
            state_.lastAction = "replaying-backlog";
            std::cout << "[live_trader] replayed " << (stop - first) << " bar(s), "
                      << (series.size() - stop) << " still pending; not trading until the "
                         "replay reaches the newest closed bar\n";
            saveState();
            return false;
        }

        // 6. One order, at most, to close the gap between what the strategy
        //    wants and what the account actually holds. Deliberately NOT one
        //    order per replayed signal: a bot that was down for a day would
        //    otherwise fire a day's worth of round trips into the market at
        //    today's price, paying real fees for trades whose prices are long
        //    gone. Only the final desired state is actionable.
        size_t lastIdx = series.size() - 1;
        double refPrice = series.close[lastIdx];   // 7. reference price
        int64_t refTime = series.timestamp[lastIdx];
        bool wantLong = state_.stratInPosition;
        bool haveLong = state_.state == PositionState::Bought;

        // Unreconciled inventory: keep polling, keep the strategy state
        // current, place nothing. Reported through lastError/lastAction so
        // `status` and any monitoring on top of it see a halted bot rather
        // than a quiet one.
        if (!reconcileBlock_.empty()) {
            state_.lastAction = "halted-balance-mismatch";
            state_.lastError = reconcileBlock_;
            saveState();
            return true;
        }

        if (wantLong && !haveLong) {
            cycleClean = placeBuy(series, lastIdx) && cycleClean;
        } else if (!wantLong && haveLong) {
            cycleClean = placeSell(refPrice, refTime) && cycleClean;
        } else {
            state_.lastAction = "hold";
        }

        // 8. A clean cycle clears the error, so `status` stops reporting a
        //    transient failure from three days ago as if it were current.
        if (cycleClean) state_.lastError.clear();
        saveState();
        return true;
    }

    // ---- venue reconciliation ------------------------------------------

    // Reconciles tracked balances against the venue's. Returns false when the
    // client cannot know them (paper mode), in which case the tracked
    // simulated balances are left exactly as they are - "unknown" must never
    // be read as "zero", or the bot would decide it is flat while real coins
    // sit on the exchange.
    //
    // The asymmetry between the two legs is the whole point, and it is about
    // WHOSE COINS THESE ARE. This used to be `state_.baseBalance = venue.base`
    // with the mismatch check only logging, which means an account holding BTC
    // the bot never bought - a manual holding, another strategy, a second bot
    // - was adopted on the first cycle as the bot's own position, and the next
    // Sell signal liquidated it at market. So:
    //   * quote is adopted outright. It is the sizing budget; trading a
    //     smaller or larger cash pile than expected is the operator's call to
    //     make, and cannot destroy an asset.
    //   * base is adopted DOWNWARD only. Fewer coins than our books say is a
    //     fact we must respect (we cannot sell what is not there). MORE coins
    //     than our books say is somebody else's inventory until proven
    //     otherwise, and the bot stops trading and says so rather than
    //     guessing.
    // `ownFillLikely` is the one case where an upward adoption is justified
    // without asking: an order we ourselves persisted has just left the book,
    // so a base increase is that order filling. cfg_.adoptVenueBasePosition is
    // the operator's explicit "yes, take over whatever is there".
    bool reconcileWithVenue(bool ownFillLikely = false) {
        AccountBalances venue = client_->balances(cfg_.symbol);
        if (!venue.ok) return false;

        auto [baseCcy, quoteCcy] = splitSymbol(cfg_.symbol);
        double price = state_.lastPrice;
        double trackedEquity = state_.quoteBalance + state_.baseBalance * price;
        // What counts as a real disagreement rather than noise. Two terms:
        // dust/0.1% of equity as before, and a couple of fee-widths of the
        // position, because some venues (Poloniex among them) bill the fee in
        // the BASE asset - so the account legitimately holds slightly less base
        // than the fill reported, and a threshold tighter than the fee would
        // halt a perfectly healthy bot on its first buy.
        double material = std::max(kDustQuoteValue, 0.001 * trackedEquity);
        material = std::max(material, 2.0 * cfg_.feePct * state_.baseBalance * price);

        double adoptedQuote = cfg_.quoteCap > 0.0 ? std::min(venue.quote, cfg_.quoteCap)
                                                    : venue.quote;
        double quoteGap = std::fabs(adoptedQuote - state_.quoteBalance);
        if (quoteGap > material) {
            std::cerr << "[live_trader] cash balance differs from tracked on " << cfg_.symbol
                      << ": tracked " << state_.quoteBalance << " " << quoteCcy << ", venue reports "
                      << venue.quote << " " << quoteCcy
                      << (cfg_.quoteCap > 0.0 ? " (capped to this sleeve's share " +
                                                     num(cfg_.quoteCap) + ")" : "")
                      << ". Adopting - it is the only cash that can actually be spent.\n";
        }
        state_.quoteBalance = adoptedQuote;

        double tracked = state_.baseBalance;
        double delta = venue.base - tracked;   // + = venue holds more than our books
        // Valued in quote so one threshold covers every symbol. Without a
        // price we cannot value it at all, so any difference is treated as
        // material rather than waved through.
        double gapQuote = price > 0.0 ? std::fabs(delta) * price
                                       : (delta != 0.0 ? 2.0 * material : 0.0);

        if (gapQuote <= material) {
            // The venue agrees with our books: an ordinary cycle, or a restart
            // where the bot's own recorded position is exactly what is there.
            // Adopt the rounding-level difference silently and carry on.
            state_.baseBalance = venue.base;
            clearReconcileBlock();
        } else if (delta < 0.0) {
            // Shortfall. Adopt downward regardless (an order for coins that do
            // not exist is rejected anyway, and pretending we hold them makes
            // every P&L number wrong), but stop trading: something other than
            // this bot moved base out of this account.
            scaleEntryBasis(tracked > 0.0 ? venue.base / tracked : 0.0);
            state_.baseBalance = venue.base;
            setReconcileBlock(
                "venue holds LESS " + baseCcy + " than this bot's recorded position (tracked " +
                num(tracked) + ", venue " + num(venue.base) + "). Something other than this bot "
                "moved coins out of the account: a manual sale, another strategy, or a second "
                "bot. Trading is suspended until the two agree - a bot sharing an account with "
                "an unknown seller cannot size or exit its own position correctly.");
        } else if (ownFillLikely || cfg_.adoptVenueBasePosition) {
            std::cerr << "[live_trader] adopting " << delta << " " << baseCcy
                      << " of venue balance as this bot's position ("
                      << (ownFillLikely ? "an order this bot placed appears to have filled"
                                        : "adoptVenueBasePosition is on")
                      << ")\n";
            state_.baseBalance = venue.base;
            // Cost basis for units nobody logged a fill for. Estimated at the
            // last close plus the configured fee so realized P&L stays
            // computable; it is an estimate and the log says so.
            if (price > 0.0) state_.entryOutlay += delta * price * (1.0 + cfg_.feePct);
            clearReconcileBlock();
        } else {
            // Excess. Keep our own number - the extra is not ours to sell.
            setReconcileBlock(
                "venue holds MORE " + baseCcy + " than this bot ever bought (tracked " +
                num(tracked) + ", venue " + num(venue.base) + ", unaccounted " + num(delta) +
                " ~ " + num(gapQuote) + " " + quoteCcy + "). Trading is suspended: a Sell here "
                "would liquidate inventory this bot never opened. Either move the unaccounted " +
                baseCcy + " to another account, or restart with --adopt-venue-position if you "
                "want this bot to take the whole balance over and eventually sell all of it.");
        }

        // The account's position follows the base the bot actually owns - not
        // everything the venue happens to be holding.
        PositionState ownState = holdsPosition(state_.baseBalance, price) ? PositionState::Bought
                                                                          : PositionState::Scanning;
        if (ownState != state_.state) {
            std::cerr << "[live_trader] account position corrected from " << toString(state_.state)
                      << " to " << toString(ownState) << " by the reconciled balances\n";
            state_.state = ownState;
        }
        if (state_.state == PositionState::Scanning) {
            state_.entryPrice = 0.0;
            state_.entryOutlay = 0.0;
            state_.entryFee = 0.0;
        } else if (state_.entryPrice <= 0.0) {
            state_.entryPrice = price;
            if (state_.entryOutlay <= 0.0 && price > 0.0)
                state_.entryOutlay = state_.baseBalance * price * (1.0 + cfg_.feePct);
            std::cerr << "[live_trader] holding a position with no recorded entry price; using "
                         "the last close (" << price << ") so the realized P&L of this trade "
                         "will be an estimate, not a measurement\n";
        }
        return true;
    }

    // The reconciliation block is deliberately sticky: it is recomputed on
    // every reconciliation and clears itself the moment the books and the
    // venue agree again (e.g. once the operator has moved the foreign coins
    // out), so no manual "resume" step is needed. The full explanation is
    // printed only when it changes, because a 60-second poll would otherwise
    // reprint it 1,440 times a day and train the operator to ignore it.
    void setReconcileBlock(const std::string& reason) {
        if (reason != reconcileBlock_) {
            std::cerr << "\n[live_trader] *** REFUSING TO TRADE " << cfg_.symbol << " ***\n  "
                      << reason << "\n  The bot keeps polling and keeps its strategy state up to "
                         "date; it just will not place orders.\n\n";
        }
        reconcileBlock_ = reason;
    }

    void clearReconcileBlock() {
        if (!reconcileBlock_.empty()) {
            std::cerr << "[live_trader] balances reconcile again; trading resumes\n";
            reconcileBlock_.clear();
        }
    }

    // Shrinks the open position's cost basis with the position itself, so a
    // partial exit (or a shortfall) leaves a per-unit basis that still means
    // the same thing.
    void scaleEntryBasis(double factor) {
        if (factor < 0.0) factor = 0.0;
        state_.entryOutlay *= factor;
        state_.entryFee *= factor;
    }

    // Compact number for operator messages. std::to_string always prints
    // exactly six decimals, which turns a small BTC amount into "0.000000" -
    // an unreadable number in a message whose entire job is to tell the
    // operator how much of something is unaccounted for.
    static std::string num(double v) {
        std::ostringstream os;
        os << std::setprecision(10) << v;
        return os.str();
    }

    // Is the order we were in the middle of placing still resting? Matched
    // on the clientOrderId because that is the only id we managed to
    // persist before sending it. An IOC order never rests, so "not found"
    // there simply means it is done (filled or cancelled) and the balances
    // decide which - the check exists to stop us re-sending a GTC/PostOnly
    // order that is alive and well.
    bool pendingOrderStillOpen() {
        auto orders = client_->openOrders(cfg_.symbol);
        for (const auto& o : orders) {
            if (!o.clientOrderId.empty() && o.clientOrderId == state_.pendingClientOrderId) {
                return true;
            }
        }
        return false;
    }

    // ---- order placement -----------------------------------------------

    // Returns true when nothing went wrong (used to decide whether to clear
    // lastError at the end of the cycle). `signalIndex` is the bar whose close
    // produced the decision: it prices the order, stamps the client order id,
    // and - now - is the bar whose realized volatility sizes the position,
    // which is precisely how BacktestEngine::run picks its signal index.
    // The market's price NOW: the still-forming candle's latest close, i.e.
    // the last trade. Used only to price the IOC limit on real orders (see
    // placeBuy/placeSell for why the decision bar's close is not usable for
    // that job). Falls back to the caller's reference on any fetch problem -
    // a degraded order beats no order, and the IOC still bounds the damage.
    double freshMarketPrice(double fallback) {
        try {
            int64_t now = std::time(nullptr);
            auto recent = source_.fetchHistory(cfg_.symbol, cfg_.periodSeconds,
                                                now - 2 * cfg_.periodSeconds,
                                                /*includeForming=*/true, now);
            if (!recent.empty() && recent.back().close > 0.0) return recent.back().close;
        } catch (...) {}
        return fallback;
    }

    bool placeBuy(const CandleSeries& series, size_t signalIndex) {
        double refPrice = series.close[signalIndex];
        int64_t barTime = series.timestamp[signalIndex];

        double fraction = sizeFraction(series, signalIndex);
        if (fraction <= 0.0) {
            // Vol targeting is on but this bar has no volatility estimate yet
            // (inside the warm-up window, or a flat stretch with zero
            // variance). The engine skips the entry in that case rather than
            // falling back to all-in, and so must this: maximum size exactly
            // where the risk is unmeasured is the one outcome a volatility
            // target exists to prevent. The strategy stays long in its own
            // books, so the entry happens on the first bar that can be sized.
            state_.lastAction = "buy-skipped-no-vol-estimate";
            return true;
        }

        // Size exactly the way BacktestEngine::executeBuy does: the fee
        // comes out of the budget FIRST, and the amount is priced at the
        // slipped price an IOC actually pays. The old code bought
        // quoteBalance/refPrice and then debited the fee on top, so the
        // tracked balance went negative on every single buy - and a real
        // venue would reject the order for insufficient funds. When flat, the
        // account's equity IS its quote balance, so this is the engine's
        // `min(cash, equity * fraction)` with the same meaning.
        double budget = std::min(state_.quoteBalance, state_.quoteBalance * fraction);
        double gross = budget / (1.0 + cfg_.feePct);
        // Price the ORDER off the market as it is now, not off the decision
        // bar's close. The close can be hours stale - at cold start, up to a
        // whole bar - and an IOC limit at a stale price below the current ask
        // expires unfilled (observed live: cancelReason 1001 on three of the
        // first eight entries). Worse than a delay, it is adverse selection:
        // a stale buy limit only fills when the market has FALLEN since the
        // signal, so a momentum book acquires exactly the entries that are
        // already going wrong. The limit is the fresh price plus the same
        // slippage allowance the backtest charges; the fill still happens at
        // the book's actual ask, the limit only bounds the worst case.
        // The limit bound is wider than the modeled slippage on purpose. The
        // fill happens at the book's actual ask; the limit only decides
        // whether the IOC crosses at all, and on the thinner books here
        // (DOGE, TRX) the spread alone exceeds 0.05% - observed live: IOCs at
        // fresh-price+slippage canceled unfilled (1001) while the same book
        // filled instantly inside a 1% bound. Sizing uses the limit price, so
        // the venue's hold (quantity x limit) can never exceed the budget.
        double orderRef = freshMarketPrice(refPrice);
        double limitPrice = orderRef * (1.0 + kOrderLimitAggression);
        if (gross <= 0.0 || limitPrice <= 0.0 || gross <= kDustQuoteValue) {
            state_.lastAction = "buy-skipped-insufficient-quote";
            return true;
        }
        double amount = gross / limitPrice;

        // Persist the intent BEFORE the order leaves, so a crash in the next
        // few milliseconds is recoverable instead of producing a duplicate.
        std::string coid = makeClientOrderId("buy", barTime);
        markPending("buy", coid);

        OrderResult r = client_->buy(cfg_.symbol, limitPrice, amount,
                                      TimeInForce::ImmediateOrCancel, coid);
        noteOrderAcknowledged(r);

        if (r.ok && r.status == OrderStatus::Open) {
            // Alive at the venue: keep the pending marker so the next cycle
            // asks about it instead of placing a second one.
            state_.lastAction = "buy-order-open:" + r.orderId;
            return true;
        }

        if (!r.ok || r.filledAmount <= 0.0) {
            // The pending marker survives an UNKNOWN outcome on purpose. `ok ==
            // false` does not mean "nothing happened" - the Poloniex client
            // returns it when the order was accepted but the read-back failed -
            // and the marker is what makes the next cycle ask openOrders() and
            // then attribute any resulting fill to us. Clearing it here would
            // hand that fill to reconciliation as unexplained inventory, which
            // now (correctly) halts the bot instead of adopting it. An observed
            // zero-fill (ok, nothing crossed) leaves nothing in flight.
            if (r.ok) clearPending();
            // Leave the strategy's notional position untouched: it still
            // wants to be long, so the next cycle simply tries again.
            state_.lastError = "buy order failed: " + r.raw;
            state_.lastAction = "buy-failed";
            std::cerr << "[live_trader] buy order failed (" << toString(r.status) << "): "
                      << r.raw << "\n";
            return false;
        }
        clearPending();

        double outlay = r.filledPrice * r.filledAmount + r.fee;
        state_.baseBalance += r.filledAmount;
        state_.quoteBalance -= outlay;
        if (state_.quoteBalance < 0.0) state_.quoteBalance = 0.0; // rounding only; sizing prevents this
        state_.entryPrice = r.filledPrice;
        // Cost basis, including the entry fee, accumulated rather than assigned
        // so a position built by two fills (a partial buy topped up later)
        // carries the basis of both. This is what the exit's P&L is measured
        // against, exactly as in engine.cpp's entryOutlay.
        state_.entryOutlay += outlay;
        state_.entryFee += r.fee;
        // A partially filled buy still leaves us long; the unfilled part just
        // stays as cash. What matters is that we only call ourselves Bought
        // when the coins are actually worth something tradeable.
        state_.state = holdsPosition(state_.baseBalance, refPrice) ? PositionState::Bought
                                                                    : PositionState::Scanning;
        state_.lastAction = r.status == OrderStatus::PartiallyFilled ? "buy-partial" : "buy";
        state_.totalTrades++;
        tradeLog_.append({nowUnix(), cfg_.symbol, "buy", r.filledPrice, r.filledAmount,
                          r.filledPrice * r.filledAmount, r.fee, std::nullopt, r.raw, coid});
        if (r.status == OrderStatus::PartiallyFilled) {
            std::cerr << "[live_trader] buy only partially filled: " << r.filledAmount << " of "
                      << amount << " requested\n";
        }
        return true;
    }

    bool placeSell(double refPrice, int64_t barTime) {
        if (!holdsPosition(state_.baseBalance, refPrice)) {
            // Nothing sellable: mark flat so the account stops disagreeing
            // with itself instead of retrying a dust order forever.
            state_.state = PositionState::Scanning;
            state_.lastAction = "sell-skipped-dust";
            return true;
        }
        double amount = state_.baseBalance;

        std::string coid = makeClientOrderId("sell", barTime);
        markPending("sell", coid);

        // Same stale-price reasoning as placeBuy, mirrored: a sell limit at a
        // stale close above the current bid expires unfilled exactly when the
        // market is falling - the one moment an exit must not fail.
        double orderRef = freshMarketPrice(refPrice);
        OrderResult r = client_->sell(cfg_.symbol, orderRef * (1.0 - kOrderLimitAggression),
                                       amount, TimeInForce::ImmediateOrCancel, coid);
        noteOrderAcknowledged(r);

        if (r.ok && r.status == OrderStatus::Open) {
            state_.lastAction = "sell-order-open:" + r.orderId;
            return true;
        }

        if (!r.ok || r.filledAmount <= 0.0) {
            // See placeBuy: an unknown outcome keeps its pending marker so the
            // next cycle can find out what the order did.
            if (r.ok) clearPending();
            state_.lastError = "sell order failed: " + r.raw;
            state_.lastAction = "sell-failed";
            std::cerr << "[live_trader] sell order failed (" << toString(r.status) << "): "
                      << r.raw << "\n";
            return false;
        }
        clearPending();

        double proceeds = r.filledPrice * r.filledAmount - r.fee;
        // Cost basis of the units being sold: the average TOTAL OUTLAY per
        // unit, entry fee included. The old `entryPrice * filledAmount` left
        // the entry fee out entirely, so every live round trip was reported
        // one entry fee better than it was - and near breakeven that is the
        // difference between a logged win and a logged loss, which is why the
        // backtest was fixed the same way in Phase 0. The fallback covers a
        // position whose basis was never recorded (adopted, or written by an
        // older build): estimate it from the entry price plus the configured
        // fee rather than pretend the entry was free.
        double basisBefore = state_.entryOutlay;
        double basisPerUnit = state_.baseBalance > 0.0 && basisBefore > 0.0
                                  ? basisBefore / state_.baseBalance
                                  : state_.entryPrice * (1.0 + cfg_.feePct);
        double cost = basisPerUnit * r.filledAmount;
        double pnl = proceeds - cost;
        state_.realizedPnl += pnl;
        state_.baseBalance -= r.filledAmount;
        if (state_.baseBalance < 0.0) state_.baseBalance = 0.0;
        state_.quoteBalance += proceeds;
        // Whatever was not sold keeps its share of the basis, so a retry of the
        // remainder next cycle is measured against what that remainder cost.
        scaleEntryBasis(basisBefore > 0.0 ? std::max(0.0, basisBefore - cost) / basisBefore : 0.0);
        state_.totalTrades++;
        tradeLog_.append({nowUnix(), cfg_.symbol, "sell", r.filledPrice, r.filledAmount,
                          r.filledPrice * r.filledAmount, r.fee, pnl, r.raw, coid});

        // PARTIAL FILL: the residual base balance decides the position, not
        // the fact that a sell happened. Flipping to Scanning while coins are
        // still on the exchange used to strand them - the bot believed it was
        // flat, so it would never sell them, and the next buy would size
        // itself as if that money did not exist.
        if (holdsPosition(state_.baseBalance, refPrice)) {
            state_.state = PositionState::Bought;
            state_.lastAction = "sell-partial";
            std::cerr << "[live_trader] sell only partially filled: " << r.filledAmount << " of "
                      << amount << "; " << state_.baseBalance
                      << " base still held, staying in position and retrying next cycle\n";
            return true;
        }

        state_.state = PositionState::Scanning;
        state_.entryPrice = 0.0;
        state_.entryOutlay = 0.0;
        state_.entryFee = 0.0;
        state_.lastAction = "sell";
        // Win/loss is counted per round trip, on the fill that actually
        // flattens the position, so a partially filled exit is one trade and
        // not two. (Its bucket is decided by the closing chunk's P&L; the
        // realizedPnl total above is exact either way.) roundTrips is
        // maintained here too, because it - not the fill count - is the
        // denominator of the reported win rate.
        state_.roundTrips++;
        if (pnl >= 0.0) state_.wins++; else state_.losses++;
        return true;
    }

    // ---- position sizing -------------------------------------------------

    // Fraction of the account to commit to an entry. This is
    // BacktestEngine::run's `sizeFraction` lambda, on purpose and line for
    // line: same rolling-volatility estimator, same window, same annualization
    // by the series' measured bars/year, same maxPositionFraction cap.
    //
    // It exists here because `--vol-target` used to be backtest-only, so the
    // live bot was unconditionally all-in while the validated backtest ran at
    // roughly half the capital. That is not a small parity gap - vol targeting
    // is the single change in the whole plan that moved the numbers most (BTC
    // 4h tsmom: Sharpe 1.16 -> 1.26, max drawdown 67.8% -> 39.7%), so a live
    // bot without it is running the version of the strategy that was rejected.
    //
    // Returns 0 to mean "do not enter". With targeting on and no usable
    // volatility estimate (NaN during the first volWindow bars, or a dead flat
    // window) the entry is skipped rather than sized all-in, matching the
    // engine: an unmeasurable-risk bar is the worst possible place to put the
    // whole account.
    double sizeFraction(const CandleSeries& series, size_t signalIndex) const {
        if (cfg_.volTargetAnnual <= 0.0) return std::max(0.0, cfg_.maxPositionFraction);

        // Same estimator the engine uses, chosen by the same config - so a
        // backtest validated with --vol-model fractional --vol-horizon 141 is
        // sized live by that model too, not by a trailing window. Calling
        // rollingVolatility directly here (as this used to) silently pinned the
        // live bot to the Trailing model no matter what the backtest ran.
        indicators::VolForecastConfig vf = cfg_.volForecast;
        vf.trailingWindow = cfg_.volWindow;
        auto volatility = indicators::forecastVolatility(series.close, vf);
        double barsPerYear = series.barsPerYear();
        if (signalIndex >= volatility.size() || std::isnan(volatility[signalIndex]) ||
            volatility[signalIndex] <= 0.0 || barsPerYear <= 0.0) {
            return 0.0;
        }
        double annualVol = volatility[signalIndex] * std::sqrt(barsPerYear);
        if (annualVol <= 0.0) return 0.0;
        return std::max(0.0, std::min(cfg_.maxPositionFraction, cfg_.volTargetAnnual / annualVol));
    }

    // ---- small helpers --------------------------------------------------

    // Deterministic idempotency key: the same (symbol, side, bar, attempt)
    // always produces the same id, so a retry after a crash - which regenerates
    // it from persisted state, having never learned the order's fate - is
    // either rejected as a duplicate by the venue or found in openOrders(),
    // instead of quietly doubling the position.
    //
    // The attempt counter is what makes the retry story consistent. Keyed on
    // (symbol, side, bar) alone, the documented partial-fill retry and the
    // documented venue-level idempotency could not both be true: selling the
    // remainder after a partial exit within the same bar reused the id of the
    // order that had already traded, so the venue either rejected the exit (and
    // the coins stayed stranded) or, if it does not dedupe, the bot doubled up.
    //
    // Format: "ct-<symbol>-<side>-a<attempt>-<barTime>". The bar timestamp is
    // deliberately kept as the LAST '-' separated field: the parity command
    // recovers the decision bar from the id by taking the substring after the
    // final '-', and that keeps working unchanged, for ids written before this
    // field existed as well as after.
    std::string makeClientOrderId(const std::string& side, int64_t barTime) const {
        return "ct-" + cfg_.symbol + "-" + side + "-a" +
               std::to_string(state_.orderAttempt + 1) + "-" + std::to_string(barTime);
    }

    // Advances the attempt counter once the venue has TOLD us what it did with
    // an order (filled, partially filled, rejected-with-an-id, or resting), so
    // the next order this bot sends cannot collide with one the venue has
    // already seen.
    //
    // Deliberately NOT advanced when the outcome is unknown (`ok == false`
    // AND the venue never issued an order id): in that case the next attempt
    // must reuse the same id, because a same-id resend is the only thing
    // standing between "we are not sure whether that order landed" and buying
    // twice.
    //
    // But an id the venue HAS seen is burned whether or not the order made
    // money for anyone. An IOC that was placed, canceled unfilled (a stale
    // limit that never crossed - observed live, cancelReason 1001) and
    // reported back `ok=false` still consumed its clientOrderId, and Poloniex
    // rejects every reuse with {"code":21312,"Client order id repeat"} - so
    // a bot that only advanced on `ok` retried the same dead id forever and
    // could never enter. Advance whenever the venue issued an id, and also on
    // the 21312 rejection itself, which is the venue telling us the id is
    // burned even though this response carries no order.
    void noteOrderAcknowledged(const OrderResult& r) {
        bool venueSawId = r.ok || !r.orderId.empty() ||
                          r.raw.find("21312") != std::string::npos ||
                          r.raw.find("Client order id repeat") != std::string::npos;
        if (venueSawId) ++state_.orderAttempt;
    }

    void markPending(const std::string& side, const std::string& clientOrderId) {
        state_.pendingClientOrderId = clientOrderId;
        state_.pendingSide = side;
        state_.pendingSince = nowUnix();
        saveState();
    }

    void clearPending() {
        state_.pendingClientOrderId.clear();
        state_.pendingSide.clear();
        state_.pendingSince = 0;
    }

    void saveState() { state_.save(statePath_); }

    static bool holdsPosition(double baseAmount, double price) {
        if (baseAmount <= 0.0) return false;
        if (price <= 0.0) return baseAmount > 0.0;
        return baseAmount * price > kDustQuoteValue;
    }

    static size_t indexOfTimestamp(const CandleSeries& series, int64_t timestamp) {
        auto it = std::lower_bound(series.timestamp.begin(), series.timestamp.end(), timestamp);
        if (it == series.timestamp.end()) return series.empty() ? 0 : series.size() - 1;
        return static_cast<size_t>(it - series.timestamp.begin());
    }

    LiveTraderConfig cfg_;
    std::unique_ptr<Strategy> strategy_;
    std::unique_ptr<TradingClient> client_;
    std::string profileName_;
    SymbolLock lock_;   // declared here so it is taken before any state I/O
    CandleStore store_;
    RateLimiter limiter_;
    PoloniexSource source_;
    std::string statePath_;
    std::string logPath_;
    TradeLog tradeLog_;
    TradingState state_;
    bool firstCycle_ = true;
    // Non-empty while the venue's inventory cannot be attributed to this bot;
    // see setReconcileBlock(). Not persisted on purpose: it is a statement
    // about the balances as they are right now, and it is recomputed by the
    // first reconciliation of every run.
    std::string reconcileBlock_;
};

} // namespace trader
