#include "core/candle_store.h"
#include "data/poloniex_source.h"
#include "data/yahoo_finance_source.h"
#include "concurrency/rate_limiter.h"
#include "concurrency/thread_pool.h"
#include "strategy/odiseo_strategy.h"
#include "strategy/sma_cross_strategy.h"
#include "strategy/pure_ichimoku_strategy.h"
#include "strategy/tsmom_strategy.h"
#include "strategy/pencil_extrapolation_strategy.h"
#include "strategy/fib_ichimoku_strategy.h"
#include "strategy/darvas_strategy.h"
#include "strategy/pattern_strategy.h"
#include "strategy/hurst_regime_filter.h"
#include "backtest/engine.h"
#include "backtest/portfolio_benchmark.h"
#include "math/spiral.h"
#include "trading/live_trader.h"
#include "trading/paper_trading_client.h"
#include "trading/poloniex_trading_client.h"
#include "trading/trading_state.h"
#include "trading/trade_log.h"
#include "evolution/fitness.h"
#include "evolution/genetic_optimizer.h"
#include "strategy/emergent_strategy.h"
#include "evolution/strategy_evolver.h"
#include "evolution/tournament.h"
#include "strategy/zoo/registry.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace trader;
namespace fs = std::filesystem;

namespace {

void printUsage() {
    std::cout <<
        "cli_trader - backtesting-focused trading CLI\n\n"
        "Usage:\n"
        "  cli_trader fetch --symbol BTC_USDT --period 14400 [--data-dir data]\n"
        "                    [--source poloniex|yahoo] [--range 10y] [--overlap 5]\n"
        "                    [--max-pages N] [--repair] [--force] [--raw-prices]\n"
        "                    [--allow-forming]\n"
        "  cli_trader validate --symbol BTC_USDT --period 14400 [--data-dir data]\n"
        "  cli_trader backtest --symbol BTC_USDT --period 14400 --strategy odiseo\n"
        "                       [--data-dir data] [--equity 1000] [--fee 0.0015]\n"
        "                       [--slippage 0.0005] [--fill-timing next-open|same-close]\n"
        "                       [--vol-target 0.20] [--vol-window 30] [--max-position 1.0]\n"
        "                       [--retarget-band 0.25] [--retarget-down-only]\n"
        "                       [--vol-model trailing|har|pencil-har|fractional]\n"
        "                       [--long-short] [--short-cost 0.10] [--long-cost 0.1095]\n"
        "                       [--vol-horizon 141] [--vol-d 0.38]\n"
        "                       [--profile crypto|equity] [--start DATE] [--end DATE]\n"
        "  cli_trader list-strategies\n"
        "  cli_trader run --symbol BTC_USDT --period 300 --strategy odiseo\n"
        "                  [--mode paper|live] [--poll-interval 60] [--data-dir data]\n"
        "                  [--state-dir state] [--profile crypto|equity] [--equity 1000]\n"
        "                  [--fee 0.0015] [--slippage 0.0005] [--vol-target 0.20]\n"
        "                  [--adopt-venue-position] [--quote-cap USDT]\n"
        "  cli_trader status --symbol BTC_USDT [--state-dir state] [--trades 10]\n"
        "  cli_trader parity --symbol BTC_USDT --period 300 --strategy odiseo\n"
        "                     [--state-dir state] [--data-dir data]\n"
        "  cli_trader order-spectrum --symbol BTC_USDT --period 14400\n"
        "                     [--input volatility|returns] [--lags 2000] [--sigma 0.3]\n"
        "                     [--radii 1.0,0.3,0.1,0.03,0.01] [--arc 1.35] [--points 64]\n"
        "                     [--start DATE] [--end DATE]\n"
        "  cli_trader portfolio --envs BTC_USDT:14400,ETH_USDT:14400,XRP_USDT:14400\n"
        "                     [--strategy tsmom] [--vol-target 0.20]\n"
        "                     [--weights equal|invvol] [--vol-lookback 120]\n"
        "                     [--rebalance 30] [--start DATE] [--end DATE]\n"
        "                     [--warmup-bars 80]\n"
        "                  [--fib-pivot 10] [--fib-min-swing-atr 4] [--fib-target -0.272]\n"
        "                  [--fib-stop 1.05] [--fib-ichimoku 0|1|2] [--fib-no-trend-exit]\n"
        "  cli_trader xsmom --envs BTC_USDT:14400,ETH_USDT:14400,XRP_USDT:14400\n"
        "                     [--lookback 90] [--hold 2] [--rebalance 30]\n"
        "                     [--trend-filter] [--start DATE] [--end DATE]\n"
        "  cli_trader evolve --envs BTC_USDT:14400,ASML.AS:86400 [--population 40]\n"
        "                     [--generations 30] [--train-end DATE] [--wf-folds 4]\n"
        "  cli_trader evolve-strategy --envs BTC_USDT:14400,ASML.AS:86400,TSLA:86400\n"
        "                     [--population 60] [--generations 40] [--min-rules 2]\n"
        "                     [--max-rules 10] [--save emergent_genome.json]\n"
        "                     [--train-end DATE] [--wf-folds 4]\n"
        "                     [--start DATE] [--end DATE]\n"
        "                     [--regimes 2022-01-01:2022-12-31,2018-01-01:2018-12-31]\n"
        "  cli_trader tournament --envs BTC_USDT:14400,ETH_USDT:14400\n"
        "                     [--population 260] [--generations 20]\n"
        "                     [--tune-generations 12] [--tune-keep 4]\n"
        "                     [--survive-threshold 0.0] [--train-frac 0.70]\n"
        "                     [--only fam,fam] [--exclude fam,fam] [--cost-limit 50]\n"
        "                     [--no-gates] [--no-vol-target] [--no-speciate]\n"
        "                     [--seed N] [--jobs N] [--save results.json]\n"
        "  cli_trader balances [--data-dir data]   # read real account holdings\n"
        "                 (keys from POLONIEX_API_KEY/SECRET or a local .env file\n"
        "                  with API-KEY=... / SECRET-KEY=...; read-only)\n"
        "  cli_trader --help\n\n"
        "Notes:\n"
        "  * 'fetch' pulls PUBLIC candle data into an append-only binary store.\n"
        "    It never stores the candle that is still forming (its high/low/close\n"
        "    would be frozen at a partial value forever), and it re-fetches a few\n"
        "    already-stored bars to check they still agree with the venue. A\n"
        "    disagreement usually means the history was re-based - a stock split\n"
        "    re-scales the whole Yahoo series - which would otherwise glue two\n"
        "    price bases together with a fake crash bar. Pass --repair to rewrite\n"
        "    the store from the fresh data when that happens; the replacement is\n"
        "    validated first and written atomically. --repair refuses to shorten\n"
        "    your history unless you also pass --force.\n"
        "      --source poloniex (default): crypto pairs, e.g. --symbol BTC_USDT\n"
        "                    --period one of 300,900,1800,7200,14400,86400\n"
        "      --source yahoo: equities/indices, e.g. --symbol ASML.AS --period 86400.\n"
        "                    Prices are split- AND dividend-adjusted by default so the\n"
        "                    series is total return and free of ex-dividend gaps;\n"
        "                    --raw-prices opts out.\n"
        "  * 'backtest' fills orders at the NEXT bar's open by default, because a\n"
        "    signal computed from a bar's close cannot be acted on before that close\n"
        "    has happened. --fill-timing same-close reproduces the older, optimistic\n"
        "    behaviour if you want to measure how much it flattered a strategy.\n"
        "    Every report is printed next to a buy-and-hold benchmark charged the\n"
        "    same fees: on a decade of a rising asset, the excess column is the only\n"
        "    one that says anything about the strategy.\n"
        "      --vol-target: size positions for a target annualized volatility\n"
        "                 instead of going all-in, which is what makes trend\n"
        "                 following survivable. 0 (default) = all-in.\n"
        "      --vol-model: how that volatility is estimated. 'trailing'\n"
        "                 (default) is a rolling stddev of PAST returns; 'har'\n"
        "                 forecasts the volatility of the bar about to be held\n"
        "                 (Corsi 2009), which is what the sizer actually needs;\n"
        "                 'pencil-har' measures the horizons instead of assuming\n"
        "                 1/5/22, at ~30x the cost; 'fractional' uses power-law\n"
        "                 memory weights j^(d-1) with d estimated causally.\n"
        "      --vol-horizon: forecast the AVERAGE volatility over the next N bars\n"
        "                 instead of the next one. A position held 141 bars is not\n"
        "                 exposed to next-bar volatility, and next-bar is the least\n"
        "                 predictable horizon there is. Set it to the strategy's\n"
        "                 typical holding period (see order-spectrum for whether the\n"
        "                 instrument has the long memory that makes this work).\n"
        "      --start/--end: restrict to a date range (YYYY-MM-DD or epoch seconds).\n"
        "      --hurst-filter [threshold]: gate entries behind a rolling Hurst\n"
        "                 regime check (see list-strategies).\n"
        "  * 'run' polls fresh CLOSED candles, replays every bar the strategy has\n"
        "    not seen yet (so a restart or outage cannot silently skip signals),\n"
        "    and places at most one order per cycle to move the account to the\n"
        "    position the strategy wants.\n"
        "      --mode paper (default): simulated fills charged the same fee AND\n"
        "                 slippage the backtest charges.\n"
        "      --mode live: real orders - requires POLONIEX_API_KEY and\n"
        "                 POLONIEX_API_SECRET; fails fast if unset.\n"
        "      --vol-target: same risk sizing the backtest uses. Pass the value\n"
        "                 you validated with, or the bot runs a riskier strategy\n"
        "                 than the one you tested.\n"
        "      --adopt-venue-position: let the bot treat base currency it did not\n"
        "                 buy as its own position. OFF by default - otherwise a\n"
        "                 manual holding or another bot's inventory gets sold the\n"
        "                 first time this strategy says Sell.\n"
        "  * 'parity' replays the stored candles through the backtest engine and\n"
        "    compares the trades it would have made against the trades actually in\n"
        "    the live trade log. The match rate, not paper P&L, is what tells you\n"
        "    the bot is executing the strategy you tested.\n"
        "  * 'order-spectrum' asks what ORDER a series' memory has, using the\n"
        "    logarithmic-spiral estimator (math/spiral.h): integer order means\n"
        "    exponential relaxation (ordinary ARMA memory), a fractional order\n"
        "    means power-law long memory. It sweeps the contour radius and prints\n"
        "    synthetic power-law and exponential references at identical settings,\n"
        "    because a single radius is not a measurement and a bare order is not\n"
        "    interpretable. The SPREAD across radii is the statistic that matters.\n"
        "  * 'evolve' / 'evolve-strategy' search parameters / rules respectively.\n"
        "    Fitness is the MINIMUM across environments of (excess annualized Sharpe\n"
        "    over buy-and-hold, minus a quadratic drawdown penalty), so a genome must\n"
        "    generalize and must not get there through a 75% drawdown.\n"
        "      --train-end DATE: evolve on data up to DATE, report on what follows.\n"
        "      --wf-folds N: anchored walk-forward - N successive train/test splits,\n"
        "                 each validated only on data the search never saw. This is\n"
        "                 the only output here with a predictive claim; without it,\n"
        "                 a search over thousands of genomes reports its own\n"
        "                 selection criterion back to you.\n";
}

std::map<std::string, std::string> parseFlags(int argc, char* argv[], int startAt) {
    std::map<std::string, std::string> flags;
    for (int i = startAt; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--", 0) == 0) {
            std::string key = arg.substr(2);
            std::string value = (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
                                    ? argv[i + 1] : "";
            flags[key] = value;
            if (!value.empty()) ++i;
        }
    }
    return flags;
}

std::string storePath(const std::string& dataDir, const std::string& symbol, int64_t period) {
    return dataDir + "/" + symbol + "_" + std::to_string(period) + ".ctc";
}

std::vector<std::pair<std::string, int64_t>> parseEnvList(const std::string& envs) {
    std::vector<std::pair<std::string, int64_t>> out;
    std::stringstream ss(envs);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) continue;
        auto pos = token.find(':');
        if (pos == std::string::npos || pos == 0 || pos + 1 >= token.size()) continue;
        out.push_back({token.substr(0, pos), std::stoll(token.substr(pos + 1))});
    }
    return out;
}

// Parses either raw unix-epoch seconds or "YYYY-MM-DD" (00:00:00 UTC).
std::optional<int64_t> parseTimestampFlag(const std::string& s) {
    if (s.empty()) return std::nullopt;
    if (s.find('-') == std::string::npos) return std::stoll(s);
    std::tm tm{};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%d");
    if (ss.fail()) {
        std::cerr << "Warning: could not parse date '" << s << "', expected YYYY-MM-DD or epoch seconds\n";
        return std::nullopt;
    }
    return static_cast<int64_t>(timegm(&tm));
}

std::vector<std::pair<std::optional<int64_t>, std::optional<int64_t>>>
parseRegimeList(const std::string& regimes) {
    std::vector<std::pair<std::optional<int64_t>, std::optional<int64_t>>> out;
    std::stringstream ss(regimes);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) continue;
        auto pos = token.find(':');
        if (pos == std::string::npos) continue;
        out.push_back({parseTimestampFlag(token.substr(0, pos)),
                       parseTimestampFlag(token.substr(pos + 1))});
    }
    return out;
}

double flagDouble(const std::map<std::string, std::string>& flags, const std::string& key, double fallback) {
    auto it = flags.find(key);
    if (it == flags.end() || it->second.empty()) return fallback;
    try { return std::stod(it->second); } catch (...) { return fallback; }
}

int flagInt(const std::map<std::string, std::string>& flags, const std::string& key, int fallback) {
    auto it = flags.find(key);
    if (it == flags.end() || it->second.empty()) return fallback;
    try { return std::stoi(it->second); } catch (...) { return fallback; }
}

// Every command that simulates fills shares one friction model, so paper
// trading, backtesting and evolution cannot silently disagree about costs.
BacktestConfig buildBacktestConfig(const std::map<std::string, std::string>& flags) {
    BacktestConfig config;
    config.startingEquity = flagDouble(flags, "equity", config.startingEquity);
    config.feePct = flagDouble(flags, "fee", config.feePct);
    config.slippagePct = flagDouble(flags, "slippage", config.slippagePct);
    config.volTargetAnnual = flagDouble(flags, "vol-target", config.volTargetAnnual);
    config.volWindow = flagInt(flags, "vol-window", config.volWindow);
    config.maxPositionFraction = flagDouble(flags, "max-position", config.maxPositionFraction);
    config.retargetBand = flagDouble(flags, "retarget-band", config.retargetBand);
    if (flags.count("retarget-down-only")) config.retargetDownOnly = true;
    auto it = flags.find("fill-timing");
    if (it != flags.end()) {
        // A typo used to select the default silently, so the report described a
        // fill model the user had not asked for.
        if (it->second == "same-close") {
            config.fillTiming = FillTiming::SameClose;
            std::cout << "WARNING: --fill-timing same-close fills every order at the very price\n"
                         "         that triggered it. This is not achievable live; it exists to\n"
                         "         measure how much the old behaviour flattered results.\n";
        } else if (it->second != "next-open") {
            throw std::runtime_error("--fill-timing must be 'next-open' or 'same-close', got '" +
                                      it->second + "'");
        }
    }

    // The engine does not borrow, so a fraction above 1 would be silently
    // clamped and the report would describe a position never taken.
    if (config.maxPositionFraction > 1.0) {
        throw std::runtime_error("--max-position above 1.0 would require leverage, which this "
                                 "engine does not implement");
    }
    if (config.maxPositionFraction < 0.0)
        throw std::runtime_error("--max-position cannot be negative");
    // Long/short mode. Off by default; the guard below makes the dependent
    // cost flags an error when the mode is off, so a cost the user believes
    // is being modelled is never silently ignored (house rule, see
    // --hurst-estimator for the precedent).
    if (flags.count("long-short")) config.allowShort = true;
    config.shortCostAnnual = flagDouble(flags, "short-cost", config.shortCostAnnual);
    config.longCostAnnual = flagDouble(flags, "long-cost", config.longCostAnnual);
    if (!config.allowShort) {
        for (const char* dependent : {"short-cost", "long-cost"}) {
            if (flags.count(dependent))
                throw std::runtime_error(std::string("--") + dependent +
                                          " has no effect without --long-short");
        }
    }

    // Volatility model for sizing.
    auto vm = flags.find("vol-model");
    if (vm != flags.end()) {
        if (vm->second == "trailing") config.volForecast.model = indicators::VolModel::Trailing;
        else if (vm->second == "har") config.volForecast.model = indicators::VolModel::HAR;
        else if (vm->second == "pencil-har") config.volForecast.model = indicators::VolModel::PencilHAR;
        else if (vm->second == "fractional") config.volForecast.model = indicators::VolModel::FractionalHAR;
        else throw std::runtime_error("--vol-model must be 'trailing', 'har', 'pencil-har' or "
                                       "'fractional', got '" + vm->second + "'");
    }
    // Forecast horizon. The sizer holds a position for many bars (measured:
    // tsmom averages 141 bars on BTC 4h, odiseo 68), but has always sized from
    // a NEXT-BAR volatility estimate - and next-bar is the least predictable
    // horizon there is (R^2 0.13 at h=1 against 0.39 at h=141 on BTC). Setting
    // this to the strategy's typical holding period asks the forecaster for the
    // quantity the position is actually exposed to.
    //
    // It must be given ex ante. Deriving it from the strategy's realized
    // holding period over the backtest would be look-ahead: you do not know how
    // long you will hold when you size the entry.
    config.volForecast.horizonBars = flagInt(flags, "vol-horizon", config.volForecast.horizonBars);
    if (config.volForecast.horizonBars < 1)
        throw std::runtime_error("--vol-horizon must be >= 1 bar");
    config.volForecast.fractionalD = flagDouble(flags, "vol-d", config.volForecast.fractionalD);
    if (config.volForecast.fractionalD >= 0.5)
        throw std::runtime_error("--vol-d must be < 0.5 (d >= 0.5 is non-stationary); "
                                  "omit it to estimate d causally from the data");
    config.volForecast.fitWindow = flagInt(flags, "vol-fit-window", config.volForecast.fitWindow);
    config.volForecast.refitEvery = flagInt(flags, "vol-refit-every", config.volForecast.refitEvery);

    if (config.volTargetAnnual > 0.0 && config.volWindow < 2) {
        throw std::runtime_error("--vol-window must be >= 2 for volatility targeting to produce "
                                 "an estimate (got " + std::to_string(config.volWindow) + ")");
    }
    return config;
}

// Odiseo volume-role overrides, parsed from flags. Kept separate from
// OdiseoParams so the per-asset-class presets stay untouched and the volume
// role composes with any of them.
struct OdiseoVolumeOverride {
    OdiseoParams::VolumeMode mode = OdiseoParams::VolumeMode::None;
    int    volWindow = 20;
    double confirmMult = 1.2;
    int    obvWindow = 10;
};

OdiseoVolumeOverride buildVolumeOverride(const std::map<std::string, std::string>& flags) {
    OdiseoVolumeOverride v;
    if (flags.count("volume-mode")) {
        const std::string vm = flags.at("volume-mode");
        if (vm == "none") v.mode = OdiseoParams::VolumeMode::None;
        else if (vm == "confirm") v.mode = OdiseoParams::VolumeMode::Confirm;
        else if (vm == "obv-exit") v.mode = OdiseoParams::VolumeMode::ObvExit;
        else if (vm == "adx-or") v.mode = OdiseoParams::VolumeMode::AdxOr;
        else throw std::runtime_error("--volume-mode must be none, confirm, obv-exit or adx-or");
    }
    v.volWindow = flagInt(flags, "vol-window-bars", v.volWindow);
    v.confirmMult = flagDouble(flags, "vol-confirm-mult", v.confirmMult);
    v.obvWindow = flagInt(flags, "obv-window", v.obvWindow);
    if (v.volWindow < 2) throw std::runtime_error("--vol-window-bars must be >= 2");
    if (v.confirmMult <= 0.0) throw std::runtime_error("--vol-confirm-mult must be positive");
    if (v.obvWindow < 2) throw std::runtime_error("--obv-window must be >= 2");
    // Accepting these while the role is off would let a user believe they had
    // configured something that is not running - the same class of silent
    // no-op the audit removed elsewhere.
    if (v.mode == OdiseoParams::VolumeMode::None) {
        for (const char* dep : {"vol-window-bars", "vol-confirm-mult", "obv-window"})
            if (flags.count(dep))
                throw std::runtime_error(std::string("--") + dep +
                                          " has no effect without --volume-mode");
    }
    return v;
}

// tsmom knobs. Only the scale-invariance fix is exposed; the rest of the
// preset is left alone so published numbers stay reproducible.
TsmomParams buildTsmomParams(const std::map<std::string, std::string>& flags) {
    TsmomParams p;
    p.entryThresholdSigmas = flagDouble(flags, "entry-sigmas", p.entryThresholdSigmas);
    if (p.entryThresholdSigmas < 0.0)
        throw std::runtime_error("--entry-sigmas must be >= 0 (0 = use the absolute threshold)");
    return p;
}

// Chart-pattern knobs. Patterns are individually switchable so a result can be
// attributed to a SHAPE rather than to "patterns" as an undifferentiated blob.
PatternParams buildPatternParams(const std::map<std::string, std::string>& flags) {
    PatternParams p;
    p.pivotWindow = flagInt(flags, "pat-pivot", p.pivotWindow);
    p.minPatternAtr = flagDouble(flags, "pat-min-atr", p.minPatternAtr);
    p.maxPatternBars = flagInt(flags, "pat-max-bars", p.maxPatternBars);
    p.levelTolFrac = flagDouble(flags, "pat-level-tol", p.levelTolFrac);
    p.targetHeightMult = flagDouble(flags, "pat-target-mult", p.targetHeightMult);
    p.stopBelowLowFrac = flagDouble(flags, "pat-stop-frac", p.stopBelowLowFrac);
    p.volMult = flagDouble(flags, "pat-vol-mult", p.volMult);
    p.harmonicTol = flagDouble(flags, "pat-harmonic-tol", p.harmonicTol);
    if (flags.count("pat-candle")) p.requireCandle = true;
    if (flags.count("pat-volume")) p.requireVolume = true;
    if (flags.count("pat-exit-bear")) p.exitOnBearCandle = true;
    if (flags.count("pat-only")) {
        const std::string w = flags.at("pat-only");
        p.useDoubleBottom = p.useInverseHS = p.useCupHandle = p.useBullFlag = false;
        p.useAscTriangle = p.useSymTriangle = p.useButterfly = p.useCypher = false;
        if (w == "double-bottom") p.useDoubleBottom = true;
        else if (w == "inverse-hs") p.useInverseHS = true;
        else if (w == "cup-handle") p.useCupHandle = true;
        else if (w == "bull-flag") p.useBullFlag = true;
        else if (w == "asc-triangle") p.useAscTriangle = true;
        else if (w == "sym-triangle") p.useSymTriangle = true;
        else if (w == "butterfly") p.useButterfly = true;
        else if (w == "cypher") p.useCypher = true;
        else if (w == "all") { p.useDoubleBottom = p.useInverseHS = p.useCupHandle =
                               p.useBullFlag = p.useAscTriangle = p.useSymTriangle =
                               p.useButterfly = p.useCypher = true; }
        else throw std::runtime_error("--pat-only must be one of double-bottom, inverse-hs, "
                                       "cup-handle, bull-flag, asc-triangle, sym-triangle, "
                                       "butterfly, cypher, all");
    }
    return p;
}

// Darvas-box knobs. The Fibonacci ratios are fixed constants (0.272/0.414/
// 0.618) exactly as in fib_ichimoku: only structural choices are tunable.
DarvasParams buildDarvasParams(const std::map<std::string, std::string>& flags) {
    DarvasParams p;
    p.pivotWindow = flagInt(flags, "box-pivot", p.pivotWindow);
    p.minBoxBars = flagInt(flags, "box-min-bars", p.minBoxBars);
    p.maxBoxBars = flagInt(flags, "box-max-bars", p.maxBoxBars);
    p.maxBoxWidthFrac = flagDouble(flags, "box-max-width", p.maxBoxWidthFrac);
    p.minSwingAtr = flagDouble(flags, "box-min-swing-atr", p.minSwingAtr);
    p.volMult = flagDouble(flags, "box-vol-mult", p.volMult);
    p.volWindow = flagInt(flags, "box-vol-window", p.volWindow);
    p.targetLevel = flagDouble(flags, "box-target", p.targetLevel);
    p.stopBelowBoxFrac = flagDouble(flags, "box-stop-cushion", p.stopBelowBoxFrac);
    if (flags.count("box-no-volume")) p.requireVolume = false;
    if (flags.count("box-no-trail")) p.trailToNewBox = false;
    if (p.targetLevel <= 0.0)
        throw std::runtime_error("--box-target must be POSITIVE here: it is the extension "
                                  "above the impulse high, so 0.272 / 0.414 / 0.618");
    if (p.minBoxBars < 1 || p.maxBoxBars <= p.minBoxBars)
        throw std::runtime_error("--box-max-bars must exceed --box-min-bars");
    if (p.maxBoxWidthFrac <= 0.0 || p.maxBoxWidthFrac >= 1.0)
        throw std::runtime_error("--box-max-width must be in (0,1)");
    return p;
}

// Fibonacci+Ichimoku knobs. The RATIOS are deliberately not exposed: 0.5/0.618
// for the entry zone and -0.272/-0.414/-0.618 for the targets are the
// specification, and letting a sweep tune them would turn "do Fibonacci levels
// work" into "can 20 free parameters fit 9 years of BTC". Only the structural
// choices - what counts as a clear swing, which of the three specified targets
// to use, how strict the regime filter is - are tunable.
FibIchimokuParams buildFibParams(const std::map<std::string, std::string>& flags) {
    FibIchimokuParams p;
    p.pivotWindow = flagInt(flags, "fib-pivot", p.pivotWindow);
    p.minSwingAtr = flagDouble(flags, "fib-min-swing-atr", p.minSwingAtr);
    p.maxSwingAgeBars = flagInt(flags, "fib-max-age", p.maxSwingAgeBars);
    p.entryZoneNear = flagDouble(flags, "fib-entry-near", p.entryZoneNear);
    p.entryZoneFar = flagDouble(flags, "fib-entry-far", p.entryZoneFar);
    p.targetLevel = flagDouble(flags, "fib-target", p.targetLevel);
    p.stopLevel = flagDouble(flags, "fib-stop", p.stopLevel);
    p.ichimokuStrictness = flagInt(flags, "fib-ichimoku", p.ichimokuStrictness);
    if (flags.count("fib-no-trend-exit")) p.exitOnTrendBreak = false;
    if (p.targetLevel >= 0.0)
        throw std::runtime_error("--fib-target must be negative (an extension ABOVE the "
                                  "swing top); the specified targets are -0.272, -0.414, -0.618");
    if (p.entryZoneNear > p.entryZoneFar)
        throw std::runtime_error("--fib-entry-near must be <= --fib-entry-far "
                                  "(near is the shallower retracement, e.g. 0.5; "
                                  "far is the deeper one, e.g. 0.618)");
    if (p.stopLevel <= p.entryZoneFar)
        throw std::runtime_error("--fib-stop must be below the entry zone (> 0.618)");
    if (p.ichimokuStrictness < 0 || p.ichimokuStrictness > 2)
        throw std::runtime_error("--fib-ichimoku must be 0 (off), 1 (trend) or 2 (full)");
    return p;
}


// Parameter overrides for any registry family: --sparams "window=120,entryZ=0.8".
//
// Every zoo family publishes its parameters as named, bounded scalars (see
// strategy/zoo/registry.h) precisely so the tournament can search them
// generically. Exposing the same vector on the command line means a
// tournament survivor can be re-run and inspected by hand with one flag,
// instead of only existing inside a search's output. An unknown name is an
// error rather than a silent no-op: a typo'd parameter that quietly does
// nothing produces a report that looks like a measurement of something it is
// not.
std::vector<double> zooParamsFor(const zoo::FamilySpec& fam, const std::string& overrides) {
    std::vector<double> values;
    for (const auto& p : fam.params) values.push_back(p.def);
    if (overrides.empty()) return values;
    std::stringstream ss(overrides);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        auto eq = tok.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("--sparams entries look like name=value, got '" + tok + "'");
        std::string key = tok.substr(0, eq);
        double val = std::stod(tok.substr(eq + 1));
        bool found = false;
        for (size_t i = 0; i < fam.params.size(); ++i) {
            if (key != fam.params[i].name) continue;
            if (val < fam.params[i].lo || val > fam.params[i].hi)
                throw std::runtime_error("--sparams " + key + "=" + tok.substr(eq + 1) +
                                          " is outside the searchable range [" +
                                          std::to_string(fam.params[i].lo) + ", " +
                                          std::to_string(fam.params[i].hi) + "]");
            values[i] = fam.params[i].isInt ? std::lround(val) : val;
            found = true;
            break;
        }
        if (!found) {
            std::string known;
            for (const auto& p : fam.params) known += std::string(known.empty() ? "" : ", ") + p.name;
            throw std::runtime_error("'" + fam.name + "' has no parameter '" + key +
                                      "'. Known: " + (known.empty() ? "(none)" : known));
        }
    }
    return values;
}

std::unique_ptr<Strategy> makeStrategy(const std::string& name, const std::string& symbol,
                                        const std::string& profileOverride,
                                        const std::string& genomeFile = "",
                                        bool genomeFileGiven = false,
                                        FibIchimokuParams fibParams = {},
                                        OdiseoVolumeOverride volOverride = {},
                                        DarvasParams darvasParams = {},
                                        TsmomParams tsmomParams = {},
                                        PatternParams patParams = {},
                                        const std::string& zooOverrides = "") {
    std::unique_ptr<Strategy> strategy;
    // Registry families first: they are the ones the tournament searches, so
    // "what the tournament found" and "what backtest runs" must be the same
    // object. The hand-wired branches below stay for the families that take
    // richer configuration than a flat parameter vector (odiseo profiles,
    // fib_ichimoku's cross-parameter validation, emergent's genome file).
    if (const auto* fam = zoo::findFamily(name)) {
        bool handWired = (name == "odiseo" || name == "sma_cross" || name == "pure_ichimoku" ||
                          name == "tsmom" || name == "fib_ichimoku" || name == "patterns" ||
                          name == "darvas" || name == "pencil_extrap");
        if (!handWired || !zooOverrides.empty())
            return fam->make(zooParamsFor(*fam, zooOverrides));
    } else if (!zooOverrides.empty()) {
        throw std::runtime_error("--sparams only applies to registry strategies; '" + name +
                                  "' is not one. See list-strategies.");
    }
    if (name == "odiseo") {
        OdiseoParams params;
        if (profileOverride == "crypto") params = OdiseoParams::cryptoDefault();
        else if (profileOverride == "equity") params = OdiseoParams::equityDefault();
        else if (profileOverride == "generalist") params = OdiseoParams::evolvedGeneralist();
        else if (profileOverride.empty()) params = OdiseoParams::forSymbol(symbol);
        else {
            // Silently ignoring this used to leave the report and the live
            // state file both asserting a profile that was never applied.
            throw std::runtime_error("unknown --profile '" + profileOverride +
                                      "'; use crypto, equity or generalist");
        }
        // Volume role, applied on top of whichever profile was selected. Off
        // by default so previously published odiseo numbers stay exactly
        // reproducible.
        params.volumeMode = volOverride.mode;
        params.volWindow = volOverride.volWindow;
        params.volConfirmMult = volOverride.confirmMult;
        params.obvWindow = volOverride.obvWindow;
        strategy = std::make_unique<OdiseoStrategy>(params);
    } else if (name == "sma_cross") {
        strategy = std::make_unique<SmaCrossStrategy>();
    } else if (name == "pure_ichimoku") {
        strategy = std::make_unique<PureIchimokuStrategy>();
    } else if (name == "tsmom") {
        strategy = std::make_unique<TsmomStrategy>(tsmomParams);
    } else if (name == "fib_ichimoku") {
        strategy = std::make_unique<FibIchimokuStrategy>(fibParams);
    } else if (name == "patterns") {
        strategy = std::make_unique<PatternStrategy>(patParams);
    } else if (name == "darvas") {
        strategy = std::make_unique<DarvasStrategy>(darvasParams);
    } else if (name == "pencil_extrap") {
        strategy = std::make_unique<PencilExtrapolationStrategy>();
    } else if (name == "emergent") {
        // An empty value here means the user wrote "--genome-file" with the
        // next token being another flag. Falling back to the default file
        // would silently backtest a different genome than the one requested.
        if (genomeFile.empty() && genomeFileGiven)
            throw std::runtime_error("--genome-file requires a path");
        std::string path = genomeFile.empty() ? "emergent_genome.json" : genomeFile;
        std::ifstream in(path);
        if (!in) {
            std::cerr << "emergent strategy requires --genome-file pointing at a genome saved by "
                         "'evolve-strategy' (tried '" << path << "')\n";
            return nullptr;
        }
        nlohmann::json j;
        in >> j;
        strategy = std::make_unique<EmergentStrategy>(EmergentGenome::fromJson(j));
    }
    return strategy;
}

std::unique_ptr<Strategy> applyHurstFilter(std::unique_ptr<Strategy> strategy,
                                            const std::map<std::string, std::string>& flags) {
    if (!flags.count("hurst-filter")) {
        // Accepting these silently while the filter is off would let a user
        // believe they had configured a gate that is not running at all.
        for (const char* dependent : {"hurst-estimator", "hurst-window"}) {
            if (flags.count(dependent))
                throw std::runtime_error(std::string("--") + dependent +
                                          " has no effect without --hurst-filter");
        }
        return strategy;
    }
    double threshold = 0.55;
    const std::string& val = flags.at("hurst-filter");
    if (!val.empty()) {
        try { threshold = std::stod(val); } catch (...) {}
    }
    int window = flagInt(flags, "hurst-window", 100);

    HurstEstimator estimator = HurstEstimator::StructureFunction;
    auto it = flags.find("hurst-estimator");
    if (it != flags.end()) {
        if (it->second == "rs") estimator = HurstEstimator::RescaledRange;
        else if (it->second != "sf")
            throw std::runtime_error("--hurst-estimator must be 'sf' (structure function, "
                                      "default) or 'rs' (rescaled range), got '" + it->second + "'");
    }
    return std::make_unique<HurstRegimeFilter>(std::move(strategy), window, threshold, estimator);
}

void printDataIssues(const std::vector<DataIssue>& issues, size_t maxShown = 12) {
    size_t shown = std::min(maxShown, issues.size());
    for (size_t i = 0; i < shown; ++i) {
        std::cout << "  [" << issues[i].kind << "] ts=" << issues[i].timestamp
                  << " " << issues[i].detail << "\n";
    }
    if (issues.size() > shown)
        std::cout << "  ... and " << (issues.size() - shown) << " more\n";
}

// ---------------------------------------------------------------------------
// Environments (a symbol, optionally restricted to a date window) and the
// walk-forward machinery that splits them into train/test.
// ---------------------------------------------------------------------------

struct Environment {
    std::string label;
    CandleSeries series;
};

std::optional<std::vector<Environment>> loadEnvironments(const std::map<std::string, std::string>& flags,
                                                          const std::string& commandName) {
    std::string dataDir = flags.count("data-dir") ? flags.at("data-dir") : "data";

    std::vector<std::pair<std::string, int64_t>> envs;
    if (flags.count("envs")) {
        envs = parseEnvList(flags.at("envs"));
        if (envs.empty()) {
            std::cerr << "--envs must look like BTC_USDT:14400,ASML.AS:86400\n";
            return std::nullopt;
        }
    } else {
        auto a = flags.find("symbol-a"), pa = flags.find("period-a");
        auto b = flags.find("symbol-b"), pb = flags.find("period-b");
        if (a == flags.end() || pa == flags.end() || b == flags.end() || pb == flags.end()) {
            std::cerr << commandName << " requires --envs SYMBOL:PERIOD,... "
                         "(or --symbol-a/--period-a and --symbol-b/--period-b)\n";
            return std::nullopt;
        }
        envs.push_back({a->second, std::stoll(pa->second)});
        envs.push_back({b->second, std::stoll(pb->second)});
    }

    auto startFlag = flags.count("start") ? parseTimestampFlag(flags.at("start")) : std::nullopt;
    auto endFlag   = flags.count("end")   ? parseTimestampFlag(flags.at("end"))   : std::nullopt;
    int warmupBars = flagInt(flags, "warmup-bars", 0);
    if (warmupBars < 0)
        throw std::runtime_error("--warmup-bars cannot be negative");
    if (warmupBars > 0 && !startFlag.has_value())
        throw std::runtime_error("--warmup-bars requires --start so the scored window is explicit");

    std::vector<std::pair<std::optional<int64_t>, std::optional<int64_t>>> regimes;
    if (flags.count("regimes")) {
        regimes = parseRegimeList(flags.at("regimes"));
        if (regimes.empty()) {
            std::cerr << "--regimes must contain at least one START:END window\n";
            return std::nullopt;
        }
    }

    std::vector<Environment> out;
    for (const auto& e : envs) {
        CandleStore store(storePath(dataDir, e.first, e.second));
        if (!store.exists()) {
            std::cerr << "Missing local data for " << e.first << " " << e.second
                      << ". Run 'fetch' first.\n";
            return std::nullopt;
        }
        CandleSeries full = store.load();
        if (full.empty()) {
            std::cerr << "Stored series is empty for " << e.first << " " << e.second << "\n";
            return std::nullopt;
        }

        if (!regimes.empty()) {
            for (size_t r = 0; r < regimes.size(); ++r) {
                CandleSeries sliced = full.slice(regimes[r].first, regimes[r].second);
                if (sliced.empty()) {
                    std::cerr << "No candles for " << e.first << " in regime window #" << (r + 1) << "\n";
                    return std::nullopt;
                }
                out.push_back({e.first + " [regime " + std::to_string(r + 1) + "]", std::move(sliced)});
            }
        } else {
            CandleSeries s = full;
            if (startFlag.has_value() || endFlag.has_value()) {
                if (warmupBars > 0 && startFlag.has_value()) {
                    size_t boundary = 0;
                    while (boundary < full.size() &&
                           full.timestamp[boundary] < *startFlag) ++boundary;
                    size_t warmStart = boundary > static_cast<size_t>(warmupBars)
                                           ? boundary - static_cast<size_t>(warmupBars) : 0;
                    s = full.slice(full.timestamp[warmStart], endFlag);
                } else {
                    s = s.slice(startFlag, endFlag);
                }
                if (s.empty()) {
                    std::cerr << "No candles for " << e.first << " in the requested --start/--end range.\n";
                    return std::nullopt;
                }
            }
            out.push_back({e.first, std::move(s)});
        }
    }
    return out;
}

struct Fold {
    std::string label;
    std::vector<CandleSeries> train;
    std::vector<CandleSeries> test;        // includes a warm-up prefix
    std::vector<int64_t> evaluateFrom;     // first timestamp actually scored
    std::vector<std::string> labels;
};

// Longest indicator warm-up any bundled strategy needs before it can emit a
// signal (odiseo's default spanB window of 52 plus its 26-bar Ichimoku
// displacement, rounded up). A test window shorter than this cannot trade at
// all, and one sliced flush at the boundary spends this many bars blind while
// the benchmark is invested from bar one.
constexpr int64_t kWarmupBars = 80;

// Minimum bars a test window must contain beyond warm-up to be worth scoring.
constexpr int64_t kMinTestBars = 60;

// Builds one fold, giving the test window a warm-up PREFIX drawn from the
// bars immediately before it: indicators are computed over the prefix, but
// `evaluateFrom` tells the engine to start trading and measuring at the real
// boundary, so train and test stay strictly disjoint as evaluation windows
// while the strategy is not artificially blinded at the start of each test.
bool appendFoldEnvironment(Fold& fold, const Environment& e,
                            std::optional<int64_t> trainEnd, std::optional<int64_t> testEnd) {
    CandleSeries train = e.series.slice(std::nullopt, trainEnd);
    CandleSeries testEval = e.series.slice(trainEnd.has_value() ? *trainEnd + 1 : std::optional<int64_t>{},
                                            testEnd);
    if (train.size() < static_cast<size_t>(kWarmupBars + kMinTestBars) ||
        testEval.size() < static_cast<size_t>(kMinTestBars)) {
        return false;
    }

    // Extend the test slice backwards by kWarmupBars *bars* (not seconds, so
    // holidays and weekends don't shrink it).
    size_t boundaryIdx = 0;
    while (boundaryIdx < e.series.size() &&
           e.series.timestamp[boundaryIdx] < testEval.timestamp.front()) ++boundaryIdx;
    size_t warmStart = boundaryIdx > static_cast<size_t>(kWarmupBars)
                           ? boundaryIdx - static_cast<size_t>(kWarmupBars) : 0;
    CandleSeries testWithWarmup = e.series.slice(e.series.timestamp[warmStart],
                                                  testEval.timestamp.back());

    fold.train.push_back(std::move(train));
    fold.test.push_back(std::move(testWithWarmup));
    fold.evaluateFrom.push_back(testEval.timestamp.front());
    fold.labels.push_back(e.label);
    return true;
}

// Anchored (expanding-window) walk-forward: fold k trains on everything up to
// boundary k+1 and is validated on the window that follows. Each environment
// is split on its own time span, so markets with different histories still
// line up fold-for-fold. Folds that cannot give every environment a usable
// train and test window are dropped rather than silently reported.
std::vector<Fold> makeWalkForwardFolds(const std::vector<Environment>& envs, int folds) {
    std::vector<Fold> out;
    for (int k = 0; k < folds; ++k) {
        Fold f;
        f.label = "fold " + std::to_string(k + 1) + "/" + std::to_string(folds);
        bool usable = true;
        for (const auto& e : envs) {
            int64_t t0 = e.series.timestamp.front();
            int64_t t1 = e.series.timestamp.back();
            double span = static_cast<double>(t1 - t0);
            int64_t trainEnd = t0 + static_cast<int64_t>(span * (k + 1.0) / (folds + 1.0));
            int64_t testEnd  = t0 + static_cast<int64_t>(span * (k + 2.0) / (folds + 1.0));
            if (!appendFoldEnvironment(f, e, trainEnd, testEnd)) {
                std::cerr << "Skipping " << f.label << ": " << e.label
                          << " has too little data on one side of the split "
                             "(need >= " << (kWarmupBars + kMinTestBars) << " training bars and >= "
                          << kMinTestBars << " test bars).\n";
                usable = false;
                break;
            }
        }
        if (usable) out.push_back(std::move(f));
    }
    return out;
}

std::vector<Fold> makeSingleSplit(const std::vector<Environment>& envs, int64_t trainEnd) {
    Fold f;
    f.label = "holdout";
    for (const auto& e : envs) {
        if (!appendFoldEnvironment(f, e, trainEnd, std::nullopt)) {
            // An empty or near-empty training set would hand back an
            // essentially random genome and then report it as an out-of-sample
            // result - a fabricated holdout, which is worse than no holdout.
            std::cerr << "--train-end does not split " << e.label
                      << " usefully: need >= " << (kWarmupBars + kMinTestBars)
                      << " bars before it and >= " << kMinTestBars << " after.\n";
            return {};
        }
    }
    return {f};
}

// Score a genome on a fold's test set. Each environment gets its own
// evaluateFromTimestamp so the warm-up prefix feeds the indicators without
// being traded or measured.
template <typename MakeStrategy>
std::vector<BacktestReport> evaluateFoldTest(const Fold& fold, const BacktestConfig& base,
                                              MakeStrategy makeStrat) {
    std::vector<BacktestReport> reports;
    reports.reserve(fold.test.size());
    for (size_t i = 0; i < fold.test.size(); ++i) {
        BacktestConfig cfg = base;
        cfg.evaluateFromTimestamp = fold.evaluateFrom[i];
        BacktestEngine engine(cfg);
        auto strat = makeStrat();
        reports.push_back(engine.run(fold.test[i], *strat));
    }
    return reports;
}

// The verdict. Judged on out-of-sample excess SHARPE, not excess return.
//
// Return is the wrong yardstick for this comparison: a volatility-targeted
// strategy deliberately runs at a fraction of full exposure (and is flat much
// of the time), so it will lose on CAGR to a fully-invested buy-and-hold even
// when it is clearly the better risk-adjusted bet - and a better Sharpe at
// lower exposure is the one you can lever up to match, while the reverse is
// not true. Sharpe is scale-invariant, which is exactly what makes it
// comparable across strategies holding different amounts of capital.
void printOutOfSampleVerdict(const std::vector<BacktestReport>& oos) {
    std::vector<const BacktestReport*> usable;
    for (const auto& r : oos) if (r.numTrades > 0) usable.push_back(&r);

    std::cout << "\n===== Out-of-sample verdict =====\n";
    if (usable.empty()) {
        std::cout << "No out-of-sample trades at all - nothing to judge.\n";
        std::cout << "=================================\n";
        return;
    }

    double sumExcessSharpe = 0.0, worstExcessSharpe = 1e18;
    double sumExcessCagr = 0.0, worstExcessCagr = 1e18;
    double sumExposure = 0.0, sumPositionFraction = 0.0;
    size_t beatOnSharpe = 0, beatOnCagr = 0, profitable = 0, sized = 0;
    for (const auto* r : usable) {
        sumExcessSharpe += r->excessSharpe;
        worstExcessSharpe = std::min(worstExcessSharpe, r->excessSharpe);
        sumExcessCagr += r->excessCagrPct;
        worstExcessCagr = std::min(worstExcessCagr, r->excessCagrPct);
        sumExposure += r->exposurePct;
        if (r->avgPositionFraction > 0.0) { sumPositionFraction += r->avgPositionFraction; ++sized; }
        if (r->excessSharpe > 0.0) ++beatOnSharpe;
        if (r->excessCagrPct > 0.0) ++beatOnCagr;
        if (r->strategy.totalReturnPct > 0.0) ++profitable;
    }
    double n = static_cast<double>(usable.size());
    double meanExcessSharpe = sumExcessSharpe / n;

    // Standard error of the mean excess Sharpe across segments. Without this
    // the verdict was a bare majority sign test: 3-of-4 segments positive by a
    // hair read exactly like a real edge.
    double varExcess = 0.0;
    for (const auto* r : usable) {
        double d = r->excessSharpe - meanExcessSharpe;
        varExcess += d * d;
    }
    double seExcess = usable.size() > 1
                          ? std::sqrt(varExcess / (n - 1.0) / n)
                          : std::numeric_limits<double>::quiet_NaN();
    size_t zeroTradeSegments = oos.size() - usable.size();

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Segments evaluated:     " << usable.size() << " of " << oos.size();
    if (zeroTradeSegments > 0)
        std::cout << "  (" << zeroTradeSegments << " never traded and are excluded - "
                  << "a strategy that stands aside is not a strategy that won)";
    std::cout << "\n";
    std::cout << "Mean excess Sharpe:     " << meanExcessSharpe;
    if (std::isfinite(seExcess)) std::cout << " +/- " << seExcess << " (SE across segments)";
    std::cout << "\n";
    std::cout << "Worst excess Sharpe:    " << worstExcessSharpe << "\n";
    std::cout << "Beat buy & hold (risk): " << beatOnSharpe << "/" << usable.size() << " segments\n";
    std::cout << "Mean excess CAGR:       " << (sumExcessCagr / n) << "%"
              << "   (beat B&H on return in " << beatOnCagr << "/" << usable.size() << ")\n";
    std::cout << "Worst excess CAGR:      " << worstExcessCagr << "%\n";
    std::cout << "Profitable in:          " << profitable << "/" << usable.size() << " segments\n";
    std::cout << "Mean time in market:    " << (sumExposure / n) << "%";
    if (sized > 0) std::cout << " at " << (sumPositionFraction / sized * 100.0) << "% of equity";
    std::cout << "\n";

    double meanExposureFraction = (sumExposure / n / 100.0) *
                                   (sized > 0 ? sumPositionFraction / sized : 1.0);
    // A handful of segments cannot establish anything, however they land.
    constexpr size_t kMinSegments = 6;
    const bool enoughSegments = usable.size() >= kMinSegments;
    const bool significant = std::isfinite(seExcess) && seExcess > 0.0 &&
                              meanExcessSharpe > 2.0 * seExcess;

    if (meanExcessSharpe <= 0.0) {
        std::cout << "\nVERDICT: no out-of-sample risk-adjusted edge over buy-and-hold.\n"
                     "         Do not deploy this.\n";
    } else if (beatOnSharpe * 2 <= usable.size()) {
        std::cout << "\nVERDICT: positive on average but beaten by buy-and-hold in most segments -\n"
                     "         the average is being carried by one or two lucky windows. Not\n"
                     "         deployable.\n";
    } else if (!enoughSegments) {
        std::cout << "\nVERDICT: positive, but on only " << usable.size() << " segment(s) - too few to\n"
                     "         distinguish an edge from luck. Add markets or folds until there\n"
                     "         are at least " << kMinSegments << ", then judge.\n";
    } else if (!significant) {
        std::cout << "\nVERDICT: positive and consistent, but the mean excess (" << meanExcessSharpe
                  << ") is within\n         two standard errors of zero (" << seExcess
                  << "). Encouraging, not established.\n"
                     "         Gather more out-of-sample evidence before risking capital.\n";
    } else {
        std::cout << "\nVERDICT: a positive out-of-sample risk-adjusted excess, consistent across\n"
                     "         segments and more than two standard errors from zero. This is the\n"
                     "         only result here worth acting on. Size it by the WORST segment,\n"
                     "         not the mean, and re-validate before committing real capital.\n";
    }

    // Applies to every positive-Sharpe verdict, not just the strongest one:
    // a vol-targeted strategy legitimately trails buy-and-hold on raw return
    // while beating it per unit of risk, and that distinction is the whole
    // reason the verdict is judged on Sharpe.
    if (meanExcessSharpe > 0.0 && sumExcessCagr / n <= 0.0) {
        std::cout << "         It earns LESS than buy-and-hold in absolute terms because it\n"
                     "         holds ~" << (meanExposureFraction * 100.0) << "% of the exposure. That is the\n"
                     "         trade being offered: better return per unit of risk, with\n"
                     "         capital and time left over to diversify into other markets.\n";
    }
    std::cout << "=================================\n";
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int cmdFetch(const std::map<std::string, std::string>& flags) {
    auto symbolIt = flags.find("symbol");
    auto periodIt = flags.find("period");
    if (symbolIt == flags.end() || periodIt == flags.end()) {
        std::cerr << "fetch requires --symbol and --period\n";
        return 1;
    }
    std::string symbol = symbolIt->second;
    int64_t period = std::stoll(periodIt->second);
    std::string dataDir = flags.count("data-dir") ? flags.at("data-dir") : "data";
    std::string sourceName = flags.count("source") ? flags.at("source") : "poloniex";
    bool allowForming = flags.count("allow-forming") > 0;
    bool repair = flags.count("repair") > 0;
    int overlapBars = flagInt(flags, "overlap", 5);
    fs::create_directories(dataDir);

    std::string path = storePath(dataDir, symbol, period);
    CandleStore store(path);
    auto since = store.lastTimestamp();

    // Request rate, overridable because venues throttle by IP: a long deep-
    // history walk at the default 3 req/s was observed to escalate Poloniex
    // from refused windows to refused TCP connects. Resuming at a lower rate
    // is the difference between finishing and being banned.
    double rps = flagDouble(flags, "rps", 3.0);
    if (rps <= 0.0 || rps > 10.0) throw std::runtime_error("--rps must be in (0, 10]");
    RateLimiter limiter(/*capacity=*/std::max(1, (int)rps * 2), /*refillPerSecond=*/rps);
    std::vector<Candle> fetched;

    if (sourceName == "poloniex") {
        // Deliberately re-fetch a few bars we already have. Comparing them
        // against the store is the only way to notice that the venue has
        // revised or re-based history; an append-only store that only ever
        // asks for "everything after my last bar" cannot see it happen.
        std::optional<int64_t> from = since;
        if (from.has_value() && overlapBars > 0)
            from = *from - static_cast<int64_t>(overlapBars) * period;
        PoloniexSource source(limiter);
        int maxPages = flagInt(flags, "max-pages", 0);
        std::cout << "Fetching " << symbol << " period=" << period << " from Poloniex"
                  << (from.has_value() ? (" since=" + std::to_string(*from)) : " (full history)")
                  << (maxPages > 0 ? (" [max " + std::to_string(maxPages) + " pages]") : "")
                  << (allowForming ? " [including the forming candle]" : "") << " ...\n";
        fetched = source.fetchHistory(symbol, period, from, allowForming,
                                       /*nowUnixSeconds=*/0, maxPages);
    } else if (sourceName == "yahoo") {
        std::string interval;
        if (period == 86400) interval = "1d";
        else if (period == 604800) interval = "1wk";
        else if (period == 3600) interval = "1h";
        else if (period == 1800) interval = "30m";
        else if (period == 900) interval = "15m";
        else if (period == 300) interval = "5m";
        else {
            // Silently mapping an unsupported period to "1d" used to store
            // daily candles in a file whose header claimed another timeframe.
            std::cerr << "Yahoo source does not support --period " << period
                      << ". Supported: 300, 900, 1800, 3600, 86400, 604800.\n";
            return 1;
        }
        std::string range = flags.count("range") ? flags.at("range") : "10y";
        bool adjust = flags.count("raw-prices") == 0;
        YahooFinanceSource source(limiter);
        std::cout << "Fetching " << symbol << " interval=" << interval << " range=" << range
                  << " from Yahoo Finance" << (adjust ? " (split- and dividend-adjusted)" : " (raw prices)")
                  << " ...\n";
        fetched = source.fetchHistory(symbol, interval, range, adjust, allowForming);
    } else {
        std::cerr << "Unknown --source '" << sourceName << "'. Use 'poloniex' or 'yahoo'.\n";
        return 1;
    }

    if (fetched.empty()) {
        std::cout << "No candles returned.\n";
        return 0;
    }

    if (store.exists()) {
        auto issues = store.compareOverlap(fetched);
        if (!issues.empty()) {
            bool rebased = false;
            for (const auto& i : issues) if (i.kind == "rebased") rebased = true;
            std::cout << (rebased ? "\nHISTORY RE-BASED: " : "\nSTORE DISAGREES WITH VENUE: ")
                      << issues.size() << " overlapping bar(s) no longer match what is stored.\n";
            printDataIssues(issues);
            if (rebased) {
                std::cout << "\nThis is the signature of a corporate action (typically a stock split):\n"
                             "the venue re-scaled the whole history. Appending new bars on top of the\n"
                             "old basis would leave a fabricated one-day crash in the middle of the\n"
                             "series, and every indicator and backtest downstream would read it as\n"
                             "real price action.\n";
            }
            if (!repair) {
                std::cout << "\nNothing was written. Re-run with --repair to rebuild the store from\n"
                             "the freshly fetched history.\n";
                return 1;
            }
            CandleSeries stored = store.load();
            if (!stored.empty() && fetched.front().timestamp > stored.timestamp.front() &&
                !flags.count("force")) {
                std::cout << "\nRefusing to repair: the fetched history starts at "
                          << fetched.front().timestamp << " but the store starts at "
                          << stored.timestamp.front() << ", so rewriting would DISCARD "
                          << "earlier history.\n"
                          << (sourceName == "yahoo"
                                  ? "Widen --range so the fetch covers the full stored history, "
                                    "or pass --force to accept the loss.\n"
                                  : "Increase --overlap so the fetch reaches further back, or pass "
                                    "--force to accept the loss.\n");
                return 1;
            }
            try {
                // Validating by default matters most here: repair is the one
                // path that writes venue data the append-time checks never saw.
                store.rewrite(symbol, period, fetched);
            } catch (const std::exception& e) {
                std::cerr << "\nRefusing to repair with bad data: " << e.what() << "\n"
                          << "The store was left untouched.\n";
                return 1;
            }
            std::cout << "\nRepaired: store rewritten with " << fetched.size()
                      << " candles on the current basis -> " << path << "\n";
            return 0;
        }
    }

    size_t appended = 0;
    try {
        appended = store.append(symbol, period, fetched, /*strict=*/true);
    } catch (const std::exception& e) {
        std::cerr << "Refusing to store bad data: " << e.what() << "\n";
        return 1;
    }
    if (appended == 0) std::cout << "No new candles.\n";
    else std::cout << "Stored " << appended << " new candles -> " << path << "\n";
    return 0;
}

int cmdValidate(const std::map<std::string, std::string>& flags) {
    auto symbolIt = flags.find("symbol");
    auto periodIt = flags.find("period");
    if (symbolIt == flags.end() || periodIt == flags.end()) {
        std::cerr << "validate requires --symbol and --period\n";
        return 1;
    }
    std::string dataDir = flags.count("data-dir") ? flags.at("data-dir") : "data";
    std::string path = storePath(dataDir, symbolIt->second, std::stoll(periodIt->second));
    CandleStore store(path);
    if (!store.exists()) {
        std::cerr << "No local data at " << path << "\n";
        return 1;
    }
    CandleSeries series = store.load();
    std::cout << "Store:     " << path << "\n"
              << "Symbol:    " << series.symbol << "\n"
              << "Period:    " << series.periodSeconds << "s\n"
              << "Candles:   " << series.size() << "\n";
    if (!series.empty()) {
        std::cout << "Range:     " << series.timestamp.front() << " .. " << series.timestamp.back()
                  << "  (" << std::fixed << std::setprecision(2) << series.spanYears() << " years, "
                  << std::setprecision(1) << series.barsPerYear() << " bars/year)\n";
    }
    auto issues = validateSeries(series);
    if (issues.empty()) {
        std::cout << "\nNo integrity issues found.\n";
        return 0;
    }
    std::cout << "\n" << issues.size() << " issue(s):\n";
    printDataIssues(issues, 40);
    return 0;
}

int cmdBacktest(const std::map<std::string, std::string>& flags) {
    auto symbolIt = flags.find("symbol");
    auto periodIt = flags.find("period");
    if (symbolIt == flags.end() || periodIt == flags.end()) {
        std::cerr << "backtest requires --symbol and --period\n";
        return 1;
    }
    std::string symbol = symbolIt->second;
    int64_t period = std::stoll(periodIt->second);
    std::string dataDir = flags.count("data-dir") ? flags.at("data-dir") : "data";
    std::string strategyName = flags.count("strategy") ? flags.at("strategy") : "odiseo";

    std::string path = storePath(dataDir, symbol, period);
    CandleStore store(path);
    if (!store.exists()) {
        std::cerr << "No local data at " << path << ". Run 'fetch' first.\n";
        return 1;
    }
    CandleSeries series = store.load();
    if (series.empty()) {
        std::cerr << "Stored series is empty.\n";
        return 1;
    }

    auto startFlag = flags.count("start") ? parseTimestampFlag(flags.at("start")) : std::nullopt;
    auto endFlag   = flags.count("end")   ? parseTimestampFlag(flags.at("end"))   : std::nullopt;
    int64_t evaluateFrom = 0;
    size_t warmupBarsUsed = 0;
    if (startFlag.has_value() || endFlag.has_value()) {
        CandleSeries requested = series.slice(startFlag, endFlag);
        if (requested.empty()) {
            std::cerr << "No candles in the requested --start/--end range.\n";
            return 1;
        }
        if (startFlag.has_value()) {
            // Carry a warm-up prefix from before the window, exactly as the
            // walk-forward folds do. Slicing flush at --start leaves every
            // indicator cold, so a strategy sits blind for its warm-up (~80
            // bars, which on daily data is four months) while buy-and-hold is
            // invested from the first bar. On a short window - a bear market,
            // say - that gap is most of the test, and it silently biases the
            // comparison against the strategy.
            size_t boundary = 0;
            while (boundary < series.size() &&
                   series.timestamp[boundary] < requested.timestamp.front()) ++boundary;
            size_t warmStart = boundary > static_cast<size_t>(kWarmupBars)
                                   ? boundary - static_cast<size_t>(kWarmupBars) : 0;
            warmupBarsUsed = boundary - warmStart;
            evaluateFrom = requested.timestamp.front();
            series = series.slice(series.timestamp[warmStart], requested.timestamp.back());
        } else {
            series = std::move(requested);
        }
        std::cout << "Window: " << (series.size() - warmupBarsUsed) << " candles scored";
        if (warmupBarsUsed > 0)
            std::cout << " (plus " << warmupBarsUsed << " warm-up bars before the window)";
        std::cout << "\n";
    }

    auto strategy = makeStrategy(strategyName, symbol,
                                  flags.count("profile") ? flags.at("profile") : "",
                                  flags.count("genome-file") ? flags.at("genome-file") : "",
                                  flags.count("genome-file") > 0, buildFibParams(flags),
                                  buildVolumeOverride(flags), buildDarvasParams(flags),
                                  buildTsmomParams(flags), buildPatternParams(flags),
                                  flags.count("sparams") ? flags.at("sparams") : "");
    if (!strategy) {
        std::cerr << "Unknown strategy '" << strategyName << "'. Use list-strategies.\n";
        return 1;
    }
    if (strategyName == "odiseo") {
        std::string profile = flags.count("profile") ? flags.at("profile")
                                                     : OdiseoParams::profileNameForSymbol(symbol);
        std::cout << "odiseo profile: " << profile << " (auto-detected unless --profile given)\n";
    }
    strategy = applyHurstFilter(std::move(strategy), flags);
    if (flags.count("hurst-filter"))
        std::cout << "Wrapped with Hurst regime filter (entries suppressed outside a trending regime)\n";

    BacktestConfig config = buildBacktestConfig(flags);
    config.evaluateFromTimestamp = evaluateFrom;
    BacktestEngine engine(config);
    BacktestReport report = engine.run(series, *strategy);
    report.print();

    // --dump-equity FILE: timestamp,equity per scored bar. Exists so several
    // runs can be combined OUTSIDE the binary (e.g. a two-tranche overlay,
    // where one coin is modelled as two virtual sleeves with different vote
    // thresholds) without teaching cmdPortfolio to run a different strategy
    // per sleeve. The curve is the engine's own closeCurve, so anything built
    // on it inherits the real fee, slippage and fill-timing model rather than
    // a reimplementation.
    if (flags.count("dump-equity")) {
        const std::string path = flags.at("dump-equity");
        std::ofstream out(path);
        if (!out) { std::cerr << "cannot write " << path << "\n"; return 1; }
        const size_t evalStart = series.size() - report.equityCurve.size();
        out << "timestamp,equity\n";
        for (size_t i = 0; i < report.equityCurve.size(); ++i)
            out << series.timestamp[evalStart + i] << "," << std::setprecision(12)
                << report.equityCurve[i] << "\n";
        if (!out) { std::cerr << "write failed: " << path << "\n"; return 1; }
        std::cerr << "wrote " << report.equityCurve.size() << " equity points to " << path << "\n";
    }
    return 0;
}

int cmdListStrategies() {
    std::cout << "Registry strategies (searchable by 'tournament', tunable with --sparams):\n";
    for (const auto& f : zoo::families()) {
        std::cout << "  " << std::left << std::setw(20) << f.name << f.provenance << "\n";
        if (f.params.empty()) { std::cout << std::setw(22) << " " << "(no tunable parameters)\n"; continue; }
        std::cout << std::setw(22) << " ";
        for (size_t i = 0; i < f.params.size(); ++i) {
            const auto& p = f.params[i];
            std::cout << p.name << "=" << p.def << " [" << p.lo << ".." << p.hi << "]"
                      << (i + 1 < f.params.size() ? ", " : "");
        }
        std::cout << "\n";
    }
    std::cout << std::right << "\n";
    std::cout << "Available strategies:\n"
                 "  odiseo         - Ichimoku + DMI/ADX trend-following + ATR trailing stop\n"
                 "  pure_ichimoku  - Ichimoku cloud breakout + tenkan/kijun cross only, no\n"
                 "                   DMI/ADX filter and no ATR stop - the baseline that shows\n"
                 "                   what those additions actually buy\n"
                 "  sma_cross      - Simple fast/slow SMA crossover\n"
                 "  tsmom          - Time Series Momentum: sign of the trailing N-bar return,\n"
                 "                   gated by an annualized realized-volatility ceiling\n"
                 "                   (Moskowitz/Ooi/Pedersen; arXiv:1904.04912)\n"
                 "  emergent       - Rule structure found by 'evolve-strategy' - requires\n"
                 "                   --genome-file pointing at a saved genome JSON\n\n"
                 "Modifiers (apply to any strategy above):\n"
                 "  --hurst-filter [threshold]  - wrap the strategy in a rolling-Hurst regime\n"
                 "                   gate (R/S analysis, bias-corrected against the Anis-Lloyd\n"
                 "                   expected R/S so that H = 0.5 really means random walk).\n"
                 "                   Entries are suppressed unless H >= threshold (default\n"
                 "                   0.55); exits are never suppressed.\n"
                 "                   --hurst-estimator sf|rs picks the estimator: 'sf' (default) is\n"
                 "                   the q=2 structure-function slope, 'rs' the classic rescaled\n"
                 "                   range. They are NOT interchangeable at a fixed threshold - R/S\n"
                 "                   under-reads persistent regimes by ~0.08 at H=0.7.\n"
                 "                   --hurst-window sets the rolling window (default 100).\n\n"
                 "All strategies are pure functions of (series, bar, position): the position is\n"
                 "owned by the caller, so the identical call sequence runs in backtest, in\n"
                 "walk-forward validation and in the live loop.\n";
    return 0;
}

int cmdRun(const std::map<std::string, std::string>& flags) {
    auto symbolIt = flags.find("symbol");
    auto periodIt = flags.find("period");
    if (symbolIt == flags.end() || periodIt == flags.end()) {
        std::cerr << "run requires --symbol and --period\n";
        return 1;
    }
    BacktestConfig friction = buildBacktestConfig(flags);

    LiveTraderConfig cfg;
    cfg.symbol = symbolIt->second;
    cfg.periodSeconds = std::stoll(periodIt->second);
    cfg.dataDir = flags.count("data-dir") ? flags.at("data-dir") : "data";
    cfg.stateDir = flags.count("state-dir") ? flags.at("state-dir") : "state";
    cfg.pollIntervalSeconds = flagInt(flags, "poll-interval", 60);
    cfg.feePct = friction.feePct;
    cfg.slippagePct = friction.slippagePct;
    cfg.startingQuoteBalance = friction.startingEquity;
    // Carry the SAME risk sizing the backtest was validated with. Leaving
    // these behind meant the live bot ran all-in while the report that
    // justified it did not - the two would be different strategies, and the
    // riskier one would be the one holding the money.
    cfg.volTargetAnnual = friction.volTargetAnnual;
    cfg.volWindow = friction.volWindow;
    cfg.maxPositionFraction = friction.maxPositionFraction;
    cfg.volForecast = friction.volForecast;
    cfg.adoptVenueBasePosition = flags.count("adopt-venue-position") > 0;
    cfg.quoteCap = flagDouble(flags, "quote-cap", cfg.quoteCap);

    std::string strategyName = flags.count("strategy") ? flags.at("strategy") : "odiseo";
    std::string profileOverride = flags.count("profile") ? flags.at("profile") : "";
    auto strategy = makeStrategy(strategyName, cfg.symbol, profileOverride,
                                  flags.count("genome-file") ? flags.at("genome-file") : "",
                                  flags.count("genome-file") > 0, buildFibParams(flags),
                                  buildVolumeOverride(flags), buildDarvasParams(flags),
                                  buildTsmomParams(flags), buildPatternParams(flags),
                                  flags.count("sparams") ? flags.at("sparams") : "");
    if (!strategy) {
        std::cerr << "Unknown strategy '" << strategyName << "'. Use list-strategies.\n";
        return 1;
    }
    std::string profileName = strategyName == "odiseo"
        ? (profileOverride.empty() ? OdiseoParams::profileNameForSymbol(cfg.symbol) : profileOverride)
        : "n/a";
    strategy = applyHurstFilter(std::move(strategy), flags);

    std::string mode = flags.count("mode") ? flags.at("mode") : "paper";
    std::unique_ptr<TradingClient> client;
    if (mode == "live") {
        try {
            client = std::make_unique<PoloniexTradingClient>(cfg.feePct);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return 1;
        }
    } else if (mode == "paper") {
        client = std::make_unique<PaperTradingClient>(cfg.feePct, cfg.slippagePct);
    } else {
        std::cerr << "Unknown --mode '" << mode << "'. Use 'paper' or 'live'.\n";
        return 1;
    }

    fs::create_directories(cfg.dataDir);
    fs::create_directories(cfg.stateDir);

    std::signal(SIGINT, [](int) { liveTraderStopFlag().store(true); });
    std::signal(SIGTERM, [](int) { liveTraderStopFlag().store(true); });

    try {
        LiveTrader trader(cfg, std::move(strategy), std::move(client), profileName);
        trader.runLoop();
    } catch (const std::exception& e) {
        std::cerr << "run failed: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

int cmdStatus(const std::map<std::string, std::string>& flags) {
    auto symbolIt = flags.find("symbol");
    if (symbolIt == flags.end()) {
        std::cerr << "status requires --symbol\n";
        return 1;
    }
    std::string stateDir = flags.count("state-dir") ? flags.at("state-dir") : "state";
    std::string statePath = stateDir + "/" + symbolIt->second + ".state.json";
    std::string logPath = stateDir + "/" + symbolIt->second + ".trades.jsonl";

    std::optional<TradingState> state;
    try {
        state = TradingState::load(statePath);
    } catch (const std::exception& e) {
        std::cerr << "State file is corrupt: " << e.what() << "\n";
        return 1;
    }
    if (!state.has_value()) {
        std::cerr << "No state file at " << statePath << " - has 'run' been started for this symbol?\n";
        return 1;
    }

    std::cout << state->toJson().dump(2) << "\n";

    size_t n = static_cast<size_t>(std::max(0, flagInt(flags, "trades", 5)));
    TradeLog log(logPath);
    auto trades = log.tail(n);
    if (!trades.empty()) {
        std::cout << "\nLast " << trades.size() << " trade(s):\n";
        for (auto& t : trades) std::cout << "  " << t.dump() << "\n";
    }
    return 0;
}

// Phase 5: does the live bot actually execute the strategy that was tested?
// Replays the stored candles through the backtest engine over the window the
// live run covered, and matches the trades it would have made against the
// trades the bot actually logged. A high match rate is the evidence that
// backtest results transfer; paper P&L on its own proves nothing.
int cmdParity(const std::map<std::string, std::string>& flags) {
    auto symbolIt = flags.find("symbol");
    auto periodIt = flags.find("period");
    if (symbolIt == flags.end() || periodIt == flags.end()) {
        std::cerr << "parity requires --symbol and --period\n";
        return 1;
    }
    std::string symbol = symbolIt->second;
    int64_t period = std::stoll(periodIt->second);
    std::string dataDir = flags.count("data-dir") ? flags.at("data-dir") : "data";
    std::string stateDir = flags.count("state-dir") ? flags.at("state-dir") : "state";
    std::string logPath = stateDir + "/" + symbol + ".trades.jsonl";

    std::ifstream in(logPath);
    if (!in.good()) {
        std::cerr << "No trade log at " << logPath << " - nothing to compare against.\n";
        return 1;
    }
    // Match on the BAR each decision came from, not on wall-clock fill time.
    // LiveTrader embeds the bar timestamp in the client order id
    // ("ct-BTC_USDT-buy-1787140800") precisely so this comparison can be
    // exact: fill time drifts with the poll interval and order latency, but
    // the bar that produced the signal is the thing both sides agree on.
    struct LoggedTrade { int64_t barTime; int64_t fillTime; std::string side; };
    std::vector<LoggedTrade> logged;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            auto j = nlohmann::json::parse(line);
            int64_t fillTime = j.value("time", static_cast<int64_t>(0));
            int64_t barTime = fillTime;
            std::string cid = j.value("client_order_id", std::string());
            auto dash = cid.rfind('-');
            if (dash != std::string::npos) {
                try { barTime = std::stoll(cid.substr(dash + 1)); } catch (...) {}
            }
            logged.push_back({barTime, fillTime, j.value("side", std::string())});
        } catch (...) {}
    }
    if (logged.empty()) {
        std::cerr << "Trade log is empty - run the bot for a while first.\n";
        return 1;
    }

    CandleStore store(storePath(dataDir, symbol, period));
    if (!store.exists()) {
        std::cerr << "No local data for " << symbol << " " << period << "\n";
        return 1;
    }
    // Run the backtest over the FULL stored series so indicators warm up
    // exactly as they do live; only the comparison is restricted to the
    // window the bot was actually running.
    CandleSeries window = store.load();
    if (window.size() < 3) {
        std::cerr << "Not enough stored candles to compare.\n";
        return 1;
    }

    // Anchor the comparison at BOTH ends of the window the bot was actually
    // responsible for. Trades before it started are not its to reproduce - and
    // neither are trades after its last processed bar. Without the upper
    // anchor, a bot that executed perfectly and then stopped scored near zero,
    // because every signal in the data it never saw counted as a miss (a
    // correctly-executing bot stopped ~3 months before the store's end scored
    // 84%, tripping the "not reproducing" verdict with zero unexplained fills).
    int64_t botStart = logged.front().barTime;
    int64_t botEnd = logged.back().barTime;
    {
        std::string statePath = stateDir + "/" + symbol + ".state.json";
        try {
            auto st = TradingState::load(statePath);
            if (st.has_value()) {
                if (st->startedAt > 0) botStart = std::min(botStart, st->startedAt);
                // The newest bar the bot actually fed to the strategy is the
                // honest end of its responsibility.
                if (st->lastProcessedTimestamp > 0)
                    botEnd = std::max(botEnd, st->lastProcessedTimestamp);
                else if (st->lastPollAt > 0)
                    botEnd = std::max(botEnd, st->lastPollAt);
            }
        } catch (...) {}
    }

    std::string strategyName = flags.count("strategy") ? flags.at("strategy") : "odiseo";
    auto strategy = makeStrategy(strategyName, symbol,
                                  flags.count("profile") ? flags.at("profile") : "",
                                  flags.count("genome-file") ? flags.at("genome-file") : "",
                                  flags.count("genome-file") > 0, buildFibParams(flags),
                                  buildVolumeOverride(flags), buildDarvasParams(flags),
                                  buildTsmomParams(flags), buildPatternParams(flags),
                                  flags.count("sparams") ? flags.at("sparams") : "");
    if (!strategy) {
        std::cerr << "Unknown strategy '" << strategyName << "'.\n";
        return 1;
    }
    strategy = applyHurstFilter(std::move(strategy), flags);

    BacktestConfig config = buildBacktestConfig(flags);
    BacktestEngine engine(config);
    BacktestReport report = engine.run(window, *strategy);

    // A live fill matches a backtest fill if it is on the same side and its
    // decision bar is within two bars - the backtest fills at the bar AFTER
    // the signal, while the bot stamps the order with the signal bar itself,
    // so a one-bar offset is expected rather than a discrepancy.
    const int64_t tolerance = 2 * period;

    auto inWindow = [&](int64_t ts) {
        return ts >= botStart - tolerance && ts <= botEnd + tolerance;
    };
    struct ExpectedTrade { int64_t time; std::string side; };
    std::vector<ExpectedTrade> expected;
    for (const auto& t : report.trades) {
        if (inWindow(t.entryTime)) expected.push_back({t.entryTime, "buy"});
        if (!t.forcedClose && inWindow(t.exitTime)) expected.push_back({t.exitTime, "sell"});
    }
    std::sort(expected.begin(), expected.end(),
              [](const ExpectedTrade& a, const ExpectedTrade& b) { return a.time < b.time; });

    std::vector<bool> loggedMatched(logged.size(), false);
    size_t matched = 0;
    for (const auto& e : expected) {
        for (size_t i = 0; i < logged.size(); ++i) {
            if (loggedMatched[i] || logged[i].side != e.side) continue;
            if (std::llabs(logged[i].barTime - e.time) <= tolerance) {
                loggedMatched[i] = true;
                ++matched;
                break;
            }
        }
    }
    size_t unmatchedLive = 0;
    for (bool m : loggedMatched) if (!m) ++unmatchedLive;

    std::cout << "===== Backtest / live parity =====\n";
    std::cout << "Symbol:                 " << symbol << " (" << period << "s bars)\n";
    std::cout << "Candles replayed:       " << window.size() << " ("
              << window.timestamp.front() << " .. " << window.timestamp.back() << ")\n";
    std::cout << "Bot responsible for:    " << botStart << " .. " << botEnd << "\n";
    std::cout << "Strategy:               " << report.strategyName << "\n";
    std::cout << "Match tolerance:        " << tolerance << "s (2 bars)\n\n";
    std::cout << "Backtest fills in window: " << expected.size() << "\n";
    std::cout << "Live fills logged:        " << logged.size() << "\n";
    std::cout << "Matched:                  " << matched << "\n";
    std::cout << "Backtest fills missed:    " << (expected.size() - matched) << "\n";
    std::cout << "Live fills unexplained:   " << unmatchedLive << "\n";
    double rate = expected.empty() ? 0.0 : 100.0 * static_cast<double>(matched) / expected.size();
    std::cout << std::fixed << std::setprecision(1) << "\nSignal match rate:        " << rate << "%\n";

    if (expected.empty() && logged.size() == 1) {
        std::cout << "\nOnly a cold-start fill to compare. When a bot starts mid-trend it adopts\n"
                     "the position the strategy is already holding, which the backtest opened\n"
                     "long before the bot existed - so that first fill legitimately has no\n"
                     "counterpart here. Let it run past a few more signals and re-check.\n";
    } else if (rate < 90.0 || unmatchedLive > 0) {
        std::cout << "\nThe live bot is NOT reproducing the tested strategy. Backtest results say\n"
                     "nothing about this configuration until this reconciles. Usual causes:\n"
                     "downtime across bar closes, orders that failed or partially filled, a\n"
                     "cold-start entry (see above), or a different strategy/profile/genome than\n"
                     "the one passed here.\n";
    } else {
        std::cout << "\nLive execution tracks the backtest over this window.\n";
    }
    std::cout << "==================================\n";
    return 0;
}

// ---------------------------------------------------------------------------
// order-spectrum: the logarithmic-spiral order estimator as a diagnostic.
//
// Answers "what ORDER is this series' memory?" - integer (exponential
// relaxation, ordinary ARMA) or fractional (power-law, long memory) - by
// evaluating the truncated Laplace transform of an autocorrelation on a
// logarithmic spiral and reading the order off a matrix-pencil line spectrum.
// See math/spiral.h for why a spiral and not a circle.
//
// Two things this command refuses to do, both learned the hard way:
//   * report a single radius. A Laplace contour at |s| interrogates timescales
//     t ~ 1/|s|, and exponential and power-law memory are indistinguishable at
//     short t. A true k^-0.3 autocorrelation reads -0.963 at radius 1 and
//     -0.705 (correct) at radius 0.01. One radius is not a measurement.
//   * report a number without controls. The estimator always returns an order;
//     the question is whether it is stable and whether it differs from memory
//     that is known to be exponential. Both references are computed here, at
//     the same settings, every run.
// ---------------------------------------------------------------------------

// Normalized autocorrelation of |log returns| (the volatility proxy) or of the
// log returns themselves, out to `maxLag`.
std::vector<double> autocorrelation(const std::vector<double>& series, size_t maxLag) {
    size_t n = series.size();
    if (n < 4) return {};
    maxLag = std::min(maxLag, n / 4);
    double mean = 0.0;
    for (double v : series) mean += v;
    mean /= static_cast<double>(n);

    std::vector<double> acf(maxLag + 1, 0.0);
    double var = 0.0;
    for (double v : series) var += (v - mean) * (v - mean);
    if (var <= 0.0) return {};
    for (size_t k = 0; k <= maxLag; ++k) {
        double s = 0.0;
        for (size_t i = 0; i + k < n; ++i) s += (series[i] - mean) * (series[i + k] - mean);
        acf[k] = s / var;
    }
    return acf;
}

struct SpectrumRow {
    std::string label;
    std::vector<double> alphaByRadius;
    double spread = 0.0;      // max-min across ALL radii
    // The statistic that actually discriminates. Every model looks wrong at
    // large radius, because a contour at |s| interrogates timescales t ~ 1/|s|
    // and nothing has revealed its asymptotics yet at t ~ 1 bar. What separates
    // a power law from an exponential is whether the order CONVERGES as the
    // contour moves out to long timescales. Measured over the smallest radii,
    // true power laws settle to 0.04-0.09 while exp(-k/20) still swings 0.68.
    double tailSpread = 0.0;
    double tailMean = 0.0;
    double mean = 0.0;
    // Complex-order component over the same tail radii. A genuinely real
    // order pins this near zero (synthetic power laws: |beta| <= 0.003);
    // exponentials and log-periodic (DSI) structure leak away from it. Used
    // by the verdict as a model-consistency criterion, calibrated against
    // the power-law references like everything else.
    double tailBeta = std::numeric_limits<double>::quiet_NaN();
    // Spread of beta over the same radii. Load-bearing: a MEAN alone cannot
    // tell a genuine complex order from noise, because beta values that flip
    // sign across radii average to whatever the cancellation leaves. Measured
    // on real stores, the per-radius beta spread ran 2x-8.6x its own mean with
    // sign flips - i.e. every "complex order" the mean-only test found was
    // noise. Discrete scale invariance means a STABLE nonzero beta, so beta
    // gets the identical stability treatment alpha gets.
    double tailBetaSpread = std::numeric_limits<double>::quiet_NaN();
};

SpectrumRow sweepRadii(const std::string& label, const std::vector<double>& acf,
                        const std::vector<double>& radii, double sigma,
                        double arc, int points, int modes) {
    SpectrumRow row;
    row.label = label;
    double lo = 1e18, hi = -1e18, sum = 0.0;
    size_t ok = 0;
    std::vector<double> betaByRadius;
    for (double r : radii) {
        mathx::SpiralConfig cfg;
        cfg.radius = r; cfg.sigma = sigma; cfg.arcHalfWidth = arc;
        cfg.points = points; cfg.modelOrder = modes;
        auto res = mathx::spiralOrder(acf, /*dt=*/1.0, cfg);
        double a = res.ok ? res.alpha : std::numeric_limits<double>::quiet_NaN();
        row.alphaByRadius.push_back(a);
        betaByRadius.push_back(res.ok ? res.beta : std::numeric_limits<double>::quiet_NaN());
        if (std::isfinite(a)) { lo = std::min(lo, a); hi = std::max(hi, a); sum += a; ++ok; }
    }
    row.spread = ok > 1 ? hi - lo : std::numeric_limits<double>::quiet_NaN();
    row.mean = ok > 0 ? sum / static_cast<double>(ok) : std::numeric_limits<double>::quiet_NaN();

    // Convergence measured over the smallest-radius third of the sweep (at
    // least 3 points), i.e. the longest timescales probed.
    size_t tailN = std::max<size_t>(3, row.alphaByRadius.size() / 3);
    tailN = std::min(tailN, row.alphaByRadius.size());
    double tlo = 1e18, thi = -1e18, tsum = 0.0;
    size_t tok = 0;
    for (size_t i = row.alphaByRadius.size() - tailN; i < row.alphaByRadius.size(); ++i) {
        double a = row.alphaByRadius[i];
        if (!std::isfinite(a)) continue;
        tlo = std::min(tlo, a); thi = std::max(thi, a); tsum += a; ++tok;
    }
    row.tailSpread = tok > 1 ? thi - tlo : std::numeric_limits<double>::quiet_NaN();
    row.tailMean = tok > 0 ? tsum / static_cast<double>(tok) : std::numeric_limits<double>::quiet_NaN();

    double bsum = 0.0, blo = 1e18, bhi = -1e18;
    size_t bok = 0;
    for (size_t i = betaByRadius.size() - tailN; i < betaByRadius.size(); ++i) {
        if (!std::isfinite(betaByRadius[i])) continue;
        bsum += betaByRadius[i];
        blo = std::min(blo, betaByRadius[i]);
        bhi = std::max(bhi, betaByRadius[i]);
        ++bok;
    }
    if (bok > 0) row.tailBeta = bsum / static_cast<double>(bok);
    if (bok > 1) row.tailBetaSpread = bhi - blo;
    return row;
}

void printSpectrumRow(const SpectrumRow& r) {
    std::cout << "  " << std::left << std::setw(34) << r.label << std::right;
    for (double a : r.alphaByRadius) {
        if (std::isfinite(a)) std::cout << std::setw(9) << std::fixed << std::setprecision(3) << a;
        else std::cout << std::setw(9) << "n/a";
    }
    for (double v : {r.spread, r.tailSpread, r.tailMean, r.tailBeta, r.tailBetaSpread}) {
        if (std::isfinite(v)) std::cout << std::setw(10) << std::fixed << std::setprecision(3) << v;
        else std::cout << std::setw(10) << "n/a";
    }
    std::cout << "\n";
}

int cmdOrderSpectrum(const std::map<std::string, std::string>& flags) {
    auto symbolIt = flags.find("symbol");
    auto periodIt = flags.find("period");
    if (symbolIt == flags.end() || periodIt == flags.end()) {
        std::cerr << "order-spectrum requires --symbol and --period\n";
        return 1;
    }
    std::string symbol = symbolIt->second;
    int64_t period = std::stoll(periodIt->second);
    std::string dataDir = flags.count("data-dir") ? flags.at("data-dir") : "data";

    CandleStore store(storePath(dataDir, symbol, period));
    if (!store.exists()) {
        std::cerr << "No local data at " << storePath(dataDir, symbol, period) << "\n";
        return 1;
    }
    CandleSeries series = store.load();

    // --start/--end exist so the order can be estimated CAUSALLY: a walk-forward
    // test of the gate has to rank instruments using only bars the strategy
    // could already have seen, and the whole estimate (autocorrelation, contour,
    // pencil) must therefore be built from the training window alone. Slicing
    // here rather than at the ACF is what makes that airtight - nothing
    // downstream ever sees a bar past --end.
    auto specStart = flags.count("start") ? parseTimestampFlag(flags.at("start")) : std::nullopt;
    auto specEnd = flags.count("end") ? parseTimestampFlag(flags.at("end")) : std::nullopt;
    if (specStart.has_value() || specEnd.has_value()) {
        if (specStart.has_value() && specEnd.has_value() && *specStart >= *specEnd)
            throw std::runtime_error("--start must be strictly before --end");
        series = series.slice(specStart, specEnd);
    }

    if (series.size() < 200) {
        std::cerr << "Need at least 200 candles for an order estimate"
                  << ((specStart.has_value() || specEnd.has_value())
                          ? " (got " + std::to_string(series.size()) +
                                " after applying --start/--end).\n"
                          : ".\n");
        return 1;
    }

    std::string input = flags.count("input") ? flags.at("input") : "volatility";
    if (input != "volatility" && input != "returns")
        throw std::runtime_error("--input must be 'volatility' (|log return| autocorrelation, "
                                  "the default) or 'returns'");

    size_t maxLag = static_cast<size_t>(std::max(50, flagInt(flags, "lags", 2000)));
    double sigma = flagDouble(flags, "sigma", 0.3);
    double arc = flagDouble(flags, "arc", 1.35);
    int points = flagInt(flags, "points", 64);
    int modes = flagInt(flags, "modes", 1);
    if (sigma == 0.0)
        throw std::runtime_error("--sigma must be non-zero: sigma = 0 is the unit circle, on which "
                                  "non-integer orders are provably invisible (see math/spiral.h)");
    if (std::fabs(arc) >= M_PI / 2.0)
        throw std::runtime_error("--arc must be < pi/2 so the contour stays in Re(s) > 0");

    std::vector<double> radii;
    if (flags.count("radii")) {
        std::stringstream ss(flags.at("radii"));
        std::string tok;
        while (std::getline(ss, tok, ',')) if (!tok.empty()) radii.push_back(std::stod(tok));
    } else {
        radii = {1.0, 0.3, 0.1, 0.03, 0.01};
    }
    if (radii.size() < 2)
        throw std::runtime_error("--radii needs at least two values: a single radius is not a "
                                  "measurement (see math/spiral.h)");

    // Build the signal whose memory we are characterising.
    std::vector<double> logRet;
    logRet.reserve(series.size());
    for (size_t i = 1; i < series.size(); ++i) {
        if (series.close[i] > 0.0 && series.close[i - 1] > 0.0)
            logRet.push_back(std::log(series.close[i] / series.close[i - 1]));
    }
    std::vector<double> signal = logRet;
    if (input == "volatility") for (double& v : signal) v = std::fabs(v);
    auto acf = autocorrelation(signal, maxLag);
    if (acf.size() < 32) {
        std::cerr << "Not enough data to form an autocorrelation.\n";
        return 1;
    }

    std::cout << "===== Order spectrum: " << symbol << " (" << period << "s bars) =====\n";
    std::cout << "Input:        autocorrelation of "
              << (input == "volatility" ? "|log return|  (volatility memory)"
                                         : "log return    (return memory)") << "\n";
    std::cout << "Samples:      " << series.size() << " candles, autocorrelation to lag "
              << (acf.size() - 1) << "\n";
    std::cout << "Contour:      s = r * exp((" << sigma << " + i)u), |u| <= " << arc
              << ", " << points << " points, pencil order " << modes << "\n\n";
    std::cout << "The recovered order says which model class the memory belongs to:\n"
                 "    alpha ~ -1      integer order  -> exponential relaxation (ARMA-like)\n"
                 "    -1 < alpha < 0  fractional     -> power-law memory, ACF ~ k^-(alpha+1)\n"
                 "Every model looks wrong at large radius, because a contour at |s| probes\n"
                 "timescales t ~ 1/|s| and nothing has shown its asymptotics yet at t ~ 1 bar.\n"
                 "What separates a power law from an exponential is whether the order CONVERGES\n"
                 "as the contour moves out to long timescales. So 'tailSprd' (spread over the\n"
                 "smallest radii) is the statistic, and 'tailMean' is the order it settles on.\n"
                 "'tailBeta' is the COMPLEX part of the order over the same radii and 'betaSprd'\n"
                 "its spread: a real order pins beta at ~0, while a genuine complex order (log-\n"
                 "periodic modulation, discrete scale invariance) holds a NONZERO beta STEADY.\n"
                 "A large beta with a large spread is noise in the leading mode, not structure.\n"
                 "A single radius is not a measurement, and a bare order is not interpretable.\n\n";

    std::cout << "  " << std::left << std::setw(34) << "series" << std::right;
    for (double r : radii) {
        std::ostringstream h; h << "r=" << r;
        std::cout << std::setw(9) << h.str();
    }
    std::cout << std::setw(10) << "spread" << std::setw(10) << "tailSprd"
              << std::setw(10) << "tailMean" << std::setw(10) << "tailBeta"
              << std::setw(10) << "betaSprd" << "\n";
    std::cout << "  " << std::string(34 + 9 * radii.size() + 50, '-') << "\n";

    SpectrumRow measured = sweepRadii(symbol, acf, radii, sigma, arc, points, modes);
    printSpectrumRow(measured);

    // Controls at identical settings. Without these the number above is
    // uninterpretable: the estimator returns an order for anything.
    std::cout << "\n  -- references, identical settings, same number of lags --\n";
    size_t K = acf.size() - 1;
    std::vector<SpectrumRow> refs;
    for (double beta : {0.3, 0.6}) {
        std::vector<double> pl(K + 1, 1.0);
        for (size_t k = 1; k <= K; ++k) pl[k] = std::pow(static_cast<double>(k), -beta);
        std::ostringstream lab;
        lab << "power law k^-" << beta << " (fractional)";
        refs.push_back(sweepRadii(lab.str(), pl, radii, sigma, arc, points, modes));
    }
    for (double tau : {20.0, 200.0}) {
        std::vector<double> ex(K + 1, 0.0);
        for (size_t k = 0; k <= K; ++k) ex[k] = std::exp(-static_cast<double>(k) / tau);
        std::ostringstream lab;
        lab << "exp(-k/" << static_cast<int>(tau) << ") (integer)";
        refs.push_back(sweepRadii(lab.str(), ex, radii, sigma, arc, points, modes));
    }
    for (const auto& r : refs) printSpectrumRow(r);

    // Verdict. Judged on convergence in the small-radius tail, against the
    // references computed at identical settings - never on the bare value.
    // Two criteria, both reference-calibrated: the order must be STABLE
    // across radii (tailSprd vs the power laws') and it must lie ON the
    // real-order manifold (|tailBeta| vs the power laws' - a stable reading
    // with nonzero beta is a complex order: log-periodic structure, not a
    // plain power law).
    double fracTail = std::max(refs[0].tailSpread, refs[1].tailSpread);
    double betaTol = std::max(0.01, 3.0 * std::max(std::fabs(refs[0].tailBeta),
                                                    std::fabs(refs[1].tailBeta)));
    bool betaOk = std::isfinite(measured.tailBeta) && std::fabs(measured.tailBeta) <= betaTol;
    // A nonzero beta only MEANS something if it is stable across radii, for
    // exactly the reason a nonzero alpha does. Calibrate the spread against
    // the power-law references (whose true beta is 0) the same way fracTail
    // calibrates alpha's, and additionally require the reading to be large
    // relative to its own scatter - |mean| > spread - so a value produced by
    // sign-flipping noise cannot be reported as discrete scale invariance.
    double betaSpreadTol = std::max(0.01, 3.0 * std::max(refs[0].tailBetaSpread,
                                                          refs[1].tailBetaSpread));
    bool betaStable = std::isfinite(measured.tailBetaSpread) &&
                      measured.tailBetaSpread <= betaSpreadTol &&
                      std::fabs(measured.tailBeta) > measured.tailBetaSpread;
    std::cout << "\n===== Verdict =====\n";
    std::cout << std::fixed << std::setprecision(3);
    if (!std::isfinite(measured.tailSpread)) {
        std::cout << "The estimator did not converge at enough radii to judge.\n";
    } else if (std::fabs(measured.tailMean + 1.0) < 0.15) {
        std::cout << "INTEGER order (" << measured.tailMean << ", i.e. ~ -1).\n"
                  << "The memory is exponential - ordinary ARMA-type relaxation. There is no\n"
                     "long-memory structure here for a fractional model to capture.\n";
    } else if (measured.tailSpread <= fracTail * 1.5 && !betaOk && betaStable) {
        std::cout << "COMPLEX order: alpha is radius-stable (" << measured.tailMean
                  << ", spread " << measured.tailSpread << ") but sits off the\n"
                  << "real-order manifold: beta = " << measured.tailBeta << " against |beta| <= "
                  << betaTol << " for true power laws\nat these settings, and beta is itself "
                     "stable (spread " << measured.tailBetaSpread << " <= " << betaSpreadTol
                  << ").\nThat is log-periodic modulation - ACF ~ k^" << (-(measured.tailMean + 1.0))
                  << " * cos(" << measured.tailBeta << " ln k + phase) -\n"
                     "discrete scale invariance, not a plain power law. Verify against surrogates\n"
                     "before trusting it; a complex order is also what a corrupted or strongly\n"
                     "periodic series produces.\n";
    } else if (measured.tailSpread <= fracTail * 1.5 && !betaOk) {
        // Off-manifold but the beta reading is not itself stable - which is
        // what noise looks like, not what discrete scale invariance looks
        // like. Reporting this as a COMPLEX order (the first version of this
        // code did) fired on 4 of 43 real stores, every one a misfire.
        std::cout << "NOT identified: alpha converges (" << measured.tailMean << ", spread "
                  << measured.tailSpread << ") but the order does not sit on\n"
                     "the real-order manifold and its complex part is unstable: beta = "
                  << measured.tailBeta << " +/- " << measured.tailBetaSpread
                  << " across the tail radii\n(|beta| <= " << betaTol
                  << " would pass as real; a genuine complex order would hold beta steady to "
                  << betaSpreadTol << ").\nAn order with an unstable imaginary part is noise in "
                     "the leading mode, not log-periodicity.\n";
    } else if (measured.tailSpread <= fracTail * 1.5) {
        std::cout << "FRACTIONAL order identified: alpha = " << measured.tailMean
                  << ", stable to " << measured.tailSpread << " across the\n"
                  << "longest timescales (a true power law settles to " << fracTail
                  << " at these settings),\n"
                  << "and on the real-order manifold: beta = " << measured.tailBeta
                  << " (|beta| <= " << betaTol << " passes).\n"
                  << "That implies autocorrelation ~ k^" << (-(measured.tailMean + 1.0))
                  << ": genuine long memory, not exponential.\n"
                  << "Sanity-check it against the exp(-k/...) rows above, which sit near -1.\n";
    } else {
        std::cout << "NOT identified: the order still wanders by " << measured.tailSpread
                  << " at the longest\ntimescales probed, against " << fracTail
                  << " for a true power law at identical settings.\n"
                     "An order that changes with the contour is not an order. Most likely the\n"
                     "autocorrelation is too noisy at long lags - try more --lags (needs more\n"
                     "data, not just a bigger number) before reading anything into the value.\n";
    }
    std::cout << "===================\n";
    return 0;
}

// ---------------------------------------------------------------------------
// portfolio: trade several instruments as ONE book, and compare against
// holding the same instruments as a basket.
//
// Why this command has to exist. Every other command here backtests ONE
// instrument and compares it against holding THAT instrument. That comparison
// systematically flatters nothing and hides one real effect: combining N
// imperfectly-correlated sleeves raises Sharpe by roughly
// sqrt(N / (1 + (N-1)rho)) for free, without any new signal. Measured on the
// eight crypto pairs here, single-instrument tsmom Sharpes run 0.50-1.28 with a
// mean of 0.83 - and the portfolio number is not the mean, it is materially
// higher. Judging a multi-instrument strategy by its average single-instrument
// result understates it; judging it against single-instrument buy-and-hold
// OVERstates it, because the benchmark diversifies too. Both errors are avoided
// only by building both sides as portfolios, which is what this does.
//
// CONSTRUCTION. Each instrument is an independent sleeve holding weight w_i of
// capital and running the identical strategy and sizing rule the
// single-instrument path uses - so a sleeve's returns come from the same engine,
// with the same next-open fills, the same fees and the same volatility
// targeting. Sleeve returns are then combined on a COMMON TIMESTAMP GRID
// (matched by timestamp, never by index: the stores have different lengths and
// different gaps, and index alignment would silently compare different dates).
//
// GROSS EXPOSURE IS CAPPED BY CONSTRUCTION. Weights sum to 1 and each sleeve
// can hold at most 100% of its own slice, so total exposure never exceeds 100%
// of equity. There is no hidden leverage here, which is exactly what makes the
// comparison against a fully-invested basket fair.
//
// WEIGHTS. `equal` is 1/N throughout. `invvol` sets w_i proportional to
// 1/sigma_i measured over a TRAILING window of sleeve returns and refreshed
// every --rebalance bars, so the weights use only past information; a
// full-sample volatility estimate would be a look-ahead, which is the kind of
// detail that turns a portfolio result into a fiction.
//
// THE BENCHMARK is an equal-weight basket of the same instruments, bought at
// the first common bar and held, charged the same entry and liquidation fee
// and slippage. Not
// rebalanced: "holding" means holding. A rebalanced basket is a strategy, and
// comparing against it would be crediting this command for a decision the
// do-nothing alternative never makes.
//
// HONEST LIMITATION. The per-sleeve intrabar low-water marks are not carried
// through the combination, so portfolio drawdown here is CLOSE-TO-CLOSE, while
// single-instrument reports are intrabar-aware. Portfolio drawdown is therefore
// slightly understated relative to those; the command labels it as such rather
// than quietly mixing the two conventions.
// ---------------------------------------------------------------------------

struct Sleeve {
    std::string label;
    const CandleSeries* series = nullptr;
    std::vector<double> equity;   // aligned with series index
    size_t equityOffset = 0;      // first series index represented in equity
    BacktestReport report;
};

int cmdPortfolio(const std::map<std::string, std::string>& flags) {
    auto envsOpt = loadEnvironments(flags, "portfolio");
    if (!envsOpt.has_value()) return 1;
    auto envs = *envsOpt;
    if (envs.size() < 2) {
        std::cerr << "portfolio needs at least two environments in --envs\n";
        return 1;
    }

    std::string strategyName = flags.count("strategy") ? flags.at("strategy") : "tsmom";
    std::string weightMode = flags.count("weights") ? flags.at("weights") : "equal";
    if (weightMode != "equal" && weightMode != "invvol")
        throw std::runtime_error("--weights must be 'equal' or 'invvol'");
    int volLookback = flagInt(flags, "vol-lookback", 120);
    int rebalance = flagInt(flags, "rebalance", 30);
    if (volLookback < 10) throw std::runtime_error("--vol-lookback must be >= 10 bars");
    if (rebalance < 1) throw std::runtime_error("--rebalance must be >= 1 bar");
    std::string profileOverride = flags.count("profile") ? flags.at("profile") : "";

    BacktestConfig config = buildBacktestConfig(flags);
    config.startingEquity = 1.0;   // sizing is scale-free; combine by weight
    if (flags.count("warmup-bars") && flags.count("start")) {
        config.evaluateFromTimestamp = *parseTimestampFlag(flags.at("start"));
        std::cout << "Window: portfolio scores from " << flags.at("start")
                  << " with " << flags.at("warmup-bars") << " warm-up bars\n";
    }

    // 1. Run every sleeve through the ordinary single-instrument engine.
    std::vector<Sleeve> sleeves;
    for (auto& e : envs) {
        auto strategy = makeStrategy(strategyName, e.series.symbol, profileOverride,
                                      flags.count("genome-file") ? flags.at("genome-file") : "",
                                      flags.count("genome-file") > 0, buildFibParams(flags),
                                  buildVolumeOverride(flags), buildDarvasParams(flags),
                                  buildTsmomParams(flags), buildPatternParams(flags),
                                  flags.count("sparams") ? flags.at("sparams") : "");
        if (!strategy) {
            std::cerr << "Unknown --strategy '" << strategyName << "'\n";
            return 1;
        }
        strategy = applyHurstFilter(std::move(strategy), flags);
        BacktestEngine engine(config);
        Sleeve s;
        s.label = e.label;
        s.series = &e.series;
        s.report = engine.run(e.series, *strategy);
        s.equity = s.report.equityCurve;
        while (s.equityOffset < e.series.size() &&
               config.evaluateFromTimestamp > 0 &&
               e.series.timestamp[s.equityOffset] < config.evaluateFromTimestamp) {
            ++s.equityOffset;
        }
        if (s.equity.size() + s.equityOffset != e.series.size()) {
            std::cerr << "internal: scored equity curve length mismatch for " << e.label << "\n";
            return 1;
        }
        sleeves.push_back(std::move(s));
    }

    // 2. Common timestamp grid: timestamps present in EVERY sleeve. Matching by
    //    timestamp rather than index is what makes this a portfolio rather than
    //    a coincidence.
    std::vector<int64_t> grid;
    {
        std::map<int64_t, size_t> seen;
        for (const auto& s : sleeves)
            for (int64_t t : s.series->timestamp) seen[t]++;
        for (const auto& kv : seen)
            if (kv.second == sleeves.size()) grid.push_back(kv.first);
    }
    if (config.evaluateFromTimestamp > 0) {
        auto firstScored = std::lower_bound(grid.begin(), grid.end(), config.evaluateFromTimestamp);
        grid.erase(grid.begin(), firstScored);
    }
    if (grid.size() < 200) {
        std::cerr << "Only " << grid.size() << " timestamps are common to all "
                  << sleeves.size() << " instruments - not enough overlap to form a "
                     "portfolio. Check the periods match and the histories overlap.\n";
        return 1;
    }

    // Per-sleeve index of each grid timestamp.
    const size_t N = sleeves.size(), T = grid.size();
    std::vector<std::vector<size_t>> idx(N, std::vector<size_t>(T, 0));
    for (size_t i = 0; i < N; ++i) {
        const auto& ts = sleeves[i].series->timestamp;
        size_t k = 0;
        for (size_t t = 0; t < T; ++t) {
            while (k < ts.size() && ts[k] < grid[t]) ++k;
            idx[i][t] = k;
        }
    }

    // 3. Sleeve returns on the grid.
    std::vector<std::vector<double>> ret(N, std::vector<double>(T, 0.0));
    for (size_t i = 0; i < N; ++i)
        for (size_t t = 1; t < T; ++t) {
            if (idx[i][t - 1] < sleeves[i].equityOffset || idx[i][t] < sleeves[i].equityOffset) {
                std::cerr << "internal: portfolio grid includes an unscored warm-up bar for "
                          << sleeves[i].label << "\n";
                return 1;
            }
            double prev = sleeves[i].equity[idx[i][t - 1] - sleeves[i].equityOffset];
            double cur = sleeves[i].equity[idx[i][t] - sleeves[i].equityOffset];
            ret[i][t] = (prev > 0.0 && std::isfinite(prev) && std::isfinite(cur))
                            ? cur / prev - 1.0 : 0.0;
        }

    // 4. Combine, refreshing weights every `rebalance` bars from TRAILING data.
    std::vector<double> w(N, 1.0 / static_cast<double>(N));
    std::vector<double> portfolio(T, 0.0);
    std::vector<double> avgWeight(N, 0.0);
    portfolio[0] = 1.0;
    for (size_t t = 1; t < T; ++t) {
        if (weightMode == "invvol" && t > static_cast<size_t>(volLookback) &&
            (t % static_cast<size_t>(rebalance)) == 0) {
            std::vector<double> inv(N, 0.0);
            double total = 0.0;
            for (size_t i = 0; i < N; ++i) {
                double mean = 0.0;
                size_t from = t - static_cast<size_t>(volLookback);
                for (size_t k = from; k < t; ++k) mean += ret[i][k];
                mean /= static_cast<double>(volLookback);
                double var = 0.0;
                for (size_t k = from; k < t; ++k) var += (ret[i][k] - mean) * (ret[i][k] - mean);
                double sd = std::sqrt(var / static_cast<double>(volLookback - 1));
                inv[i] = sd > 1e-12 ? 1.0 / sd : 0.0;
                total += inv[i];
            }
            // A degenerate window (every sleeve flat) leaves the weights alone
            // rather than dividing by zero.
            if (total > 0.0)
                for (size_t i = 0; i < N; ++i) w[i] = inv[i] / total;
        }
        double r = 0.0;
        for (size_t i = 0; i < N; ++i) { r += w[i] * ret[i][t]; avgWeight[i] += w[i]; }
        portfolio[t] = portfolio[t - 1] * (1.0 + r);
    }
    for (size_t i = 0; i < N; ++i) avgWeight[i] /= static_cast<double>(T - 1);

    // 5. Equal-weight buy-and-hold basket over the same grid. It uses the
    // same next-open entry and end-of-window liquidation friction as a sleeve.
    std::vector<const CandleSeries*> benchmarkSeries;
    benchmarkSeries.reserve(N);
    for (const auto& s : sleeves) benchmarkSeries.push_back(s.series);
    std::vector<double> basket = equalWeightBuyAndHoldCurve(
        benchmarkSeries, idx, config.feePct, config.slippagePct);

    // 6. Measure both with the engine's own statistics code.
    double span = static_cast<double>(grid.back() - grid.front());
    constexpr double kSecondsPerYear = 365.25 * 24.0 * 3600.0;
    double barsPerYear = span > 0.0 ? (static_cast<double>(T) - 1.0) * kSecondsPerYear / span : 0.0;
    double years = span / kSecondsPerYear;
    PerformanceStats ps = performanceFromCurve(portfolio, portfolio, 1.0, barsPerYear, years);
    PerformanceStats bs = performanceFromCurve(basket, basket, 1.0, barsPerYear, years);

    // Average pairwise correlation of sleeve returns: the number that decides
    // how much the combination can possibly buy.
    double corrSum = 0.0; size_t corrN = 0;
    for (size_t i = 0; i < N; ++i)
        for (size_t j = i + 1; j < N; ++j) {
            double mi = 0.0, mj = 0.0;
            for (size_t t = 1; t < T; ++t) { mi += ret[i][t]; mj += ret[j][t]; }
            mi /= (T - 1); mj /= (T - 1);
            double num = 0.0, di = 0.0, dj = 0.0;
            for (size_t t = 1; t < T; ++t) {
                double a = ret[i][t] - mi, b = ret[j][t] - mj;
                num += a * b; di += a * a; dj += b * b;
            }
            if (di > 0.0 && dj > 0.0) { corrSum += num / std::sqrt(di * dj); ++corrN; }
        }
    double avgCorr = corrN ? corrSum / static_cast<double>(corrN) : std::nan("");

    // 7. Report.
    std::cout << "===== Portfolio Report =====\n"
              << "Strategy:        " << strategyName << " on " << N << " instruments\n"
              << "Weights:         " << weightMode;
    if (weightMode == "invvol")
        std::cout << " (trailing " << volLookback << " bars, refreshed every " << rebalance << ")";
    std::cout << "\nCommon window:   " << T << " bars, " << std::fixed << std::setprecision(2)
              << years << " years, " << std::setprecision(1) << barsPerYear << " bars/year\n"
              << "Gross exposure:  capped at 100% by construction (weights sum to 1)\n\n";

    std::cout << "                    portfolio    EW basket       excess\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Total return:  " << std::setw(14) << ps.totalReturnPct << "%"
              << std::setw(13) << bs.totalReturnPct << "%"
              << std::setw(12) << (ps.totalReturnPct - bs.totalReturnPct) << "%\n";
    std::cout << "CAGR:          " << std::setw(14) << ps.cagrPct << "%"
              << std::setw(13) << bs.cagrPct << "%"
              << std::setw(12) << (ps.cagrPct - bs.cagrPct) << "%\n";
    std::cout << "Sharpe (ann.): " << std::setw(14) << ps.sharpeAnnual
              << std::setw(14) << bs.sharpeAnnual
              << std::setw(13) << (ps.sharpeAnnual - bs.sharpeAnnual) << "\n";
    std::cout << "  +/-          " << std::setw(14) << ps.sharpeStdErr << "\n";
    std::cout << "Sortino (ann.):" << std::setw(14) << ps.sortinoAnnual
              << std::setw(14) << bs.sortinoAnnual << "\n";
    std::cout << "Max drawdown:  " << std::setw(14) << ps.maxDrawdownPct << "%"
              << std::setw(13) << bs.maxDrawdownPct << "%"
              << "   (close-to-close, see note)\n\n";

    std::cout << "Avg pairwise sleeve correlation: " << std::setprecision(3) << avgCorr << "\n";
    if (std::isfinite(avgCorr) && avgCorr > 0.0) {
        double meanSharpe = 0.0;
        for (const auto& s : sleeves) meanSharpe += s.report.strategy.sharpeAnnual;
        meanSharpe /= static_cast<double>(N);
        double theoretical = meanSharpe * std::sqrt(static_cast<double>(N) /
                              (1.0 + (static_cast<double>(N) - 1.0) * avgCorr));
        std::cout << "Mean single-instrument Sharpe:   " << std::setprecision(2) << meanSharpe
                  << "   -> diversification predicts " << theoretical
                  << ", achieved " << ps.sharpeAnnual << "\n";
    }

    std::cout << "\n--- per sleeve (each traded standalone, for reference) ---\n";
    std::cout << "  " << std::left << std::setw(22) << "instrument" << std::right
              << std::setw(9) << "Sharpe" << std::setw(9) << "B&H" << std::setw(10) << "excess"
              << std::setw(9) << "maxDD%" << std::setw(9) << "trades" << std::setw(10) << "avg wt%"
              << "\n";
    for (size_t i = 0; i < N; ++i) {
        const auto& r = sleeves[i].report;
        std::cout << "  " << std::left << std::setw(22) << sleeves[i].label << std::right
                  << std::setw(9) << std::setprecision(2) << r.strategy.sharpeAnnual
                  << std::setw(9) << r.benchmark.sharpeAnnual
                  << std::setw(10) << r.excessSharpe
                  << std::setw(9) << std::setprecision(1) << r.strategy.maxDrawdownPct
                  << std::setw(9) << r.numTrades
                  << std::setw(10) << std::setprecision(1) << (100.0 * avgWeight[i]) << "\n";
    }
    std::cout << "\nNote: portfolio drawdown is close-to-close; the per-sleeve column is\n"
                 "intrabar-aware, so the two are not directly comparable.\n"
                 "============================\n";
    return 0;
}

// ---------------------------------------------------------------------------
// xsmom: CROSS-SECTIONAL momentum. Rank the universe, hold the leaders.
//
// Every other strategy here is TIME-SERIES: "is BTC going up?" This one asks a
// different question - "which of these N will outperform the others?" - and the
// difference is not cosmetic. A time-series rule must predict DIRECTION, which
// this project has now failed to do six separate ways. A cross-sectional rule
// only needs the relative ORDERING to carry information; it can be right about
// which coin leads while being wrong about whether the sector rises. Both sides
// of the comparison below are fully invested at all times, so nothing here is a
// bet on crypto going up: the basket captures that, and what is being measured
// is purely the SELECTION.
//
// This is the Jegadeesh-Titman cross-sectional momentum anomaly, which is among
// the most replicated results in asset pricing (equities, futures, currencies,
// and - in the more recent literature - crypto).
//
// THE RULE. At each rebalance, rank instruments by trailing return over
// --lookback bars, hold the top --hold of them at equal weight, and keep that
// book until the next rebalance. Ranking uses only closes at or before the
// decision bar. Fills are charged --fee + --slippage on the TURNOVER of every
// weight change, so a full rotation out of one name into another pays both
// legs.
//
// WHY IT IS NOT SIZED BY VOLATILITY. The comparison is against an equal-weight
// basket that is always 100% invested. Adding volatility targeting on top would
// change the exposure profile as well as the selection, and the point of this
// test is to isolate whether the RANKING carries information. Sizing is a
// separate question, already answered elsewhere in this repo (it reduces risk
// and does not add return).
//
// HOLDOUT DISCIPLINE. Parameters are explored on data BEFORE the boundary in
// experiments/holdout.json; the holdout is judged once, on a rule written down
// in advance. Use --start/--end to enforce that; the command does not know
// about the boundary and will happily let you cheat.
// ---------------------------------------------------------------------------

int cmdXsMom(const std::map<std::string, std::string>& flags) {
    auto envsOpt = loadEnvironments(flags, "xsmom");
    if (!envsOpt.has_value()) return 1;
    auto envs = *envsOpt;
    const size_t N = envs.size();
    if (N < 3) {
        std::cerr << "xsmom needs at least three environments: ranking two "
                     "instruments and holding one is a coin flip, not a cross-section\n";
        return 1;
    }

    int lookback = flagInt(flags, "lookback", 90);
    int hold = flagInt(flags, "hold", static_cast<int>(N / 2));
    int rebalance = flagInt(flags, "rebalance", 30);
    bool trendFilter = flags.count("trend-filter") > 0;
    if (lookback < 2) throw std::runtime_error("--lookback must be >= 2 bars");
    if (hold < 1 || static_cast<size_t>(hold) > N)
        throw std::runtime_error("--hold must be between 1 and the number of instruments");
    if (rebalance < 1) throw std::runtime_error("--rebalance must be >= 1 bar");

    BacktestConfig config = buildBacktestConfig(flags);
    const double costRate = config.feePct + config.slippagePct;

    // Common timestamp grid, matched by timestamp (never by index).
    std::vector<int64_t> grid;
    {
        std::map<int64_t, size_t> seen;
        for (const auto& e : envs)
            for (int64_t t : e.series.timestamp) seen[t]++;
        for (const auto& kv : seen) if (kv.second == N) grid.push_back(kv.first);
    }
    if (grid.size() < static_cast<size_t>(lookback) + 200) {
        std::cerr << "Only " << grid.size() << " timestamps common to all " << N
                  << " instruments; need at least lookback + 200.\n";
        return 1;
    }
    const size_t T = grid.size();

    std::vector<std::vector<size_t>> idx(N, std::vector<size_t>(T, 0));
    for (size_t i = 0; i < N; ++i) {
        const auto& ts = envs[i].series.timestamp;
        size_t k = 0;
        for (size_t t = 0; t < T; ++t) {
            while (k < ts.size() && ts[k] < grid[t]) ++k;
            idx[i][t] = k;
        }
    }
    auto px = [&](size_t i, size_t t) { return envs[i].series.close[idx[i][t]]; };

    // Per-bar asset returns on the grid.
    std::vector<std::vector<double>> aret(N, std::vector<double>(T, 0.0));
    for (size_t i = 0; i < N; ++i)
        for (size_t t = 1; t < T; ++t) {
            double p0 = px(i, t - 1), p1 = px(i, t);
            aret[i][t] = (p0 > 0.0) ? p1 / p0 - 1.0 : 0.0;
        }

    std::vector<double> w(N, 0.0), wPrev(N, 0.0);
    std::vector<double> equity(T, 0.0), basket(T, 0.0);
    std::vector<double> heldCount(N, 0.0);
    double turnoverTotal = 0.0;
    size_t rebalances = 0;
    const size_t start = static_cast<size_t>(lookback);

    equity[start] = 1.0;
    for (size_t t = start + 1; t < T; ++t) {
        // Decide the book at rebalance points, using information at t-1.
        if (((t - start - 1) % static_cast<size_t>(rebalance)) == 0) {
            std::vector<std::pair<double, size_t>> score;
            for (size_t i = 0; i < N; ++i) {
                double pNow = px(i, t - 1);
                double pThen = px(i, t - 1 - static_cast<size_t>(lookback));
                double trailing = (pThen > 0.0) ? pNow / pThen - 1.0
                                                : -std::numeric_limits<double>::infinity();
                score.emplace_back(trailing, i);
            }
            std::sort(score.begin(), score.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
            std::fill(w.begin(), w.end(), 0.0);
            size_t chosen = 0;
            for (int r = 0; r < hold; ++r) {
                // --trend-filter turns this into a hybrid: a leader with a
                // NEGATIVE trailing return is skipped and its slice held in
                // cash, so the book can be less than fully invested.
                if (trendFilter && score[r].first <= 0.0) continue;
                w[score[r].second] = 1.0;
                ++chosen;
            }
            if (chosen > 0)
                for (size_t i = 0; i < N; ++i) w[i] /= static_cast<double>(chosen);
            double turnover = 0.0;
            for (size_t i = 0; i < N; ++i) turnover += std::fabs(w[i] - wPrev[i]);
            turnoverTotal += turnover;
            ++rebalances;
            // Costs are charged the moment the book changes, against equity.
            equity[t - 1] *= (1.0 - turnover * costRate);
            wPrev = w;
        }
        double r = 0.0;
        for (size_t i = 0; i < N; ++i) { r += w[i] * aret[i][t]; heldCount[i] += (w[i] > 0.0 ? 1.0 : 0.0); }
        equity[t] = equity[t - 1] * (1.0 + r);
    }

    // Equal-weight basket over the identical window, same entry cost, held.
    for (size_t t = start; t < T; ++t) {
        double v = 0.0;
        for (size_t i = 0; i < N; ++i) {
            double p0 = px(i, start), pt = px(i, t);
            if (p0 > 0.0) v += (1.0 / static_cast<double>(N)) * (pt / p0);
        }
        basket[t] = v * (1.0 - costRate);
    }

    std::vector<double> eq(equity.begin() + start, equity.end());
    std::vector<double> bk(basket.begin() + start, basket.end());
    double span = static_cast<double>(grid.back() - grid[start]);
    constexpr double kSecondsPerYear = 365.25 * 24.0 * 3600.0;
    double barsPerYear = span > 0.0 ? (static_cast<double>(eq.size()) - 1.0) * kSecondsPerYear / span : 0.0;
    double years = span / kSecondsPerYear;
    PerformanceStats ps = performanceFromCurve(eq, eq, 1.0, barsPerYear, years);
    PerformanceStats bs = performanceFromCurve(bk, bk, 1.0, barsPerYear, years);

    std::cout << "===== Cross-sectional momentum =====\n"
              << "Universe:        " << N << " instruments, holding top " << hold << "\n"
              << "Signal:          trailing return over " << lookback
              << " bars, rebalanced every " << rebalance << " bars"
              << (trendFilter ? ", trend-filtered" : "") << "\n"
              << "Window:          " << eq.size() << " bars, " << std::fixed
              << std::setprecision(2) << years << " years\n"
              << "Rebalances:      " << rebalances << ", mean turnover "
              << std::setprecision(3) << (rebalances ? turnoverTotal / rebalances : 0.0)
              << " of book per rebalance\n\n";

    std::cout << "                       xsmom    EW basket       excess\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Total return:  " << std::setw(14) << ps.totalReturnPct << "%"
              << std::setw(13) << bs.totalReturnPct << "%"
              << std::setw(12) << (ps.totalReturnPct - bs.totalReturnPct) << "%\n";
    std::cout << "CAGR:          " << std::setw(14) << ps.cagrPct << "%"
              << std::setw(13) << bs.cagrPct << "%"
              << std::setw(12) << (ps.cagrPct - bs.cagrPct) << "%\n";
    std::cout << "Sharpe (ann.): " << std::setw(14) << ps.sharpeAnnual
              << std::setw(14) << bs.sharpeAnnual
              << std::setw(13) << (ps.sharpeAnnual - bs.sharpeAnnual) << "\n";
    std::cout << "  +/-          " << std::setw(14) << ps.sharpeStdErr << "\n";
    std::cout << "Sortino (ann.):" << std::setw(14) << ps.sortinoAnnual
              << std::setw(14) << bs.sortinoAnnual << "\n";
    std::cout << "Max drawdown:  " << std::setw(14) << ps.maxDrawdownPct << "%"
              << std::setw(13) << bs.maxDrawdownPct << "%"
              << "   (close-to-close)\n\n";

    std::cout << "--- share of bars held ---\n";
    for (size_t i = 0; i < N; ++i)
        std::cout << "  " << std::left << std::setw(22) << envs[i].label << std::right
                  << std::setw(7) << std::setprecision(1)
                  << (100.0 * heldCount[i] / static_cast<double>(T - start - 1)) << "%\n";
    std::cout << "====================================\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Evolution commands. Both follow the same protocol: optionally split the
// environments into train/test, evolve on TRAIN only, report on TEST, then
// (for the artifact you would actually deploy) re-evolve on everything and
// save that - clearly labelled, because its in-sample numbers are not
// evidence.
// ---------------------------------------------------------------------------

void warnIfNoHoldout(bool hasHoldout, size_t trials) {
    if (hasHoldout) return;
    std::cout << "\n*** NO HOLDOUT ***\n"
                 "This run will report the winner on the same candles that selected it,\n"
                 "after trying " << trials << " candidate strategies against them. The best of\n"
                 "that many draws posts a strong-looking Sharpe on pure noise, so the numbers\n"
                 "below are a measure of the search, not of an edge. Pass --train-end DATE or\n"
                 "--wf-folds N to get a number with predictive content.\n\n";
}

int cmdEvolve(const std::map<std::string, std::string>& flags) {
    auto envsOpt = loadEnvironments(flags, "evolve");
    if (!envsOpt.has_value()) return 1;
    auto envs = *envsOpt;

    int population = flagInt(flags, "population", 40);
    int generations = flagInt(flags, "generations", 30);
    size_t trials = evolution::GeneticOptimizer::trialCount(population, generations);

    BacktestConfig config = buildBacktestConfig(flags);
    BacktestConfig reportConfig = config;
    reportConfig.searchTrials = trials;
    evolution::FitnessConfig fitness;

    int wfFolds = flagInt(flags, "wf-folds", 0);
    auto trainEnd = flags.count("train-end") ? parseTimestampFlag(flags.at("train-end")) : std::nullopt;

    std::vector<Fold> folds;
    bool holdoutRequested = wfFolds > 0 || trainEnd.has_value();
    if (wfFolds > 0) folds = makeWalkForwardFolds(envs, wfFolds);
    else if (trainEnd.has_value()) folds = makeSingleSplit(envs, *trainEnd);
    if (holdoutRequested && folds.empty()) {
        // Silently continuing would report an in-sample result to a user who
        // explicitly asked for validation - the single worst failure mode here.
        std::cerr << "Requested validation could not be constructed (see above). "
                     "Refusing to fall back to an in-sample run.\n";
        return 1;
    }

    std::cout << "Evolving odiseo parameters across " << envs.size() << " environment(s):\n";
    for (const auto& e : envs)
        std::cout << "  - " << e.label << " (" << e.series.size() << " candles)\n";
    std::cout << "Population=" << population << " Generations=" << generations
              << " (" << trials << " candidates)\n";
    warnIfNoHoldout(!folds.empty(), trials);

    std::vector<BacktestReport> oos;
    for (size_t f = 0; f < folds.size(); ++f) {
        std::cout << "\n===== " << folds[f].label << " =====\n";
        evolution::GeneticOptimizer optimizer(folds[f].train, config, evolution::GenomeBounds{}, fitness);
        OdiseoParams best = optimizer.evolve(population, generations);
        auto reports = evaluateFoldTest(folds[f], reportConfig,
                                         [&] { return std::make_unique<OdiseoStrategy>(best); });
        std::cout << "Out-of-sample:\n";
        for (size_t i = 0; i < reports.size(); ++i) {
            std::cout << "  " << folds[f].labels[i] << " -> " << reports[i].summaryLine() << "\n";
            oos.push_back(reports[i]);
        }
    }
    if (!folds.empty()) printOutOfSampleVerdict(oos);

    // The deployable artifact: evolved on everything available.
    std::vector<CandleSeries> allSeries;
    for (auto& e : envs) allSeries.push_back(e.series);
    evolution::GeneticOptimizer finalOptimizer(allSeries, config, evolution::GenomeBounds{}, fitness);
    OdiseoParams best = finalOptimizer.evolve(population, generations);
    auto finalResult = finalOptimizer.evaluateOn(best, allSeries, reportConfig);

    std::cout << "\n===== Best genome (evolved on ALL data) =====\n"
              << "dmiWindow=" << best.dmiWindow << " atrWindow=" << best.atrWindow
              << " adxThreshold=" << best.adxThreshold << " atrStopMultiple=" << best.atrStopMultiple
              << " requireTenkanAboveKijun=" << (best.requireTenkanAboveKijun ? "true" : "false")
              << " tenkanWindow=" << best.tenkanWindow << " kijunWindow=" << best.kijunWindow
              << " spanBWindow=" << best.spanBWindow << "\n"
              << "in-sample fitness=" << finalResult.fitness
              << "  (in-sample: not evidence - see the verdict above)\n\n";
    for (size_t i = 0; i < finalResult.reports.size(); ++i) {
        std::cout << "--- " << envs[i].label << " (IN-SAMPLE) ---\n";
        finalResult.reports[i].print();
        if (i + 1 < finalResult.reports.size()) std::cout << "\n";
    }
    return 0;
}

int cmdEvolveStrategy(const std::map<std::string, std::string>& flags) {
    auto envsOpt = loadEnvironments(flags, "evolve-strategy");
    if (!envsOpt.has_value()) return 1;
    auto envs = *envsOpt;

    int population = flagInt(flags, "population", 60);
    int generations = flagInt(flags, "generations", 40);
    size_t trials = evolution::StrategyEvolver::trialCount(population, generations);

    evolution::StrategyGenomeBounds bounds;
    bounds.minRules = flagInt(flags, "min-rules", bounds.minRules);
    bounds.maxRules = flagInt(flags, "max-rules", bounds.maxRules);

    BacktestConfig config = buildBacktestConfig(flags);
    BacktestConfig reportConfig = config;
    reportConfig.searchTrials = trials;
    evolution::FitnessConfig fitness;

    int wfFolds = flagInt(flags, "wf-folds", 0);
    auto trainEnd = flags.count("train-end") ? parseTimestampFlag(flags.at("train-end")) : std::nullopt;

    std::vector<Fold> folds;
    bool holdoutRequested = wfFolds > 0 || trainEnd.has_value();
    if (wfFolds > 0) folds = makeWalkForwardFolds(envs, wfFolds);
    else if (trainEnd.has_value()) folds = makeSingleSplit(envs, *trainEnd);
    if (holdoutRequested && folds.empty()) {
        // Silently continuing would report an in-sample result to a user who
        // explicitly asked for validation - the single worst failure mode here.
        std::cerr << "Requested validation could not be constructed (see above). "
                     "Refusing to fall back to an in-sample run.\n";
        return 1;
    }

    std::cout << "Evolving an EMERGENT strategy (rules, not just parameters) across "
              << envs.size() << " environment(s):\n";
    for (const auto& e : envs)
        std::cout << "  - " << e.label << " (" << e.series.size() << " candles)\n";
    std::cout << "Fitness = minimum across environments of (excess Sharpe over buy & hold\n"
                 "          minus a quadratic drawdown penalty).\n"
              << "Population=" << population << " Generations=" << generations
              << " rules in [" << bounds.minRules << "," << bounds.maxRules << "]"
              << " (" << trials << " candidates)\n";
    warnIfNoHoldout(!folds.empty(), trials);

    std::vector<BacktestReport> oos;
    for (size_t f = 0; f < folds.size(); ++f) {
        std::cout << "\n===== " << folds[f].label << " =====\n";
        evolution::StrategyEvolver evolver(folds[f].train, config, bounds, fitness);
        EmergentGenome best = evolver.evolve(population, generations);
        auto reports = evaluateFoldTest(folds[f], reportConfig,
                                         [&] { return std::make_unique<EmergentStrategy>(best); });
        std::cout << "Out-of-sample:\n";
        for (size_t i = 0; i < reports.size(); ++i) {
            std::cout << "  " << folds[f].labels[i] << " -> " << reports[i].summaryLine() << "\n";
            oos.push_back(reports[i]);
        }
    }
    if (!folds.empty()) printOutOfSampleVerdict(oos);

    std::vector<CandleSeries> allSeries;
    for (auto& e : envs) allSeries.push_back(e.series);
    evolution::StrategyEvolver finalEvolver(allSeries, config, bounds, fitness);
    EmergentGenome best = finalEvolver.evolve(population, generations);
    auto finalResult = finalEvolver.evaluateOn(best, allSeries, reportConfig);

    std::cout << "\n===== Best emergent strategy (evolved on ALL data) =====\n" << best.describe()
              << "in-sample fitness=" << finalResult.fitness
              << "  (in-sample: not evidence - see the verdict above)\n\n";
    for (size_t i = 0; i < finalResult.reports.size(); ++i) {
        std::cout << "--- " << envs[i].label << " (IN-SAMPLE) ---\n";
        finalResult.reports[i].print();
        if (i + 1 < finalResult.reports.size()) std::cout << "\n";
    }

    std::string savePath = flags.count("save") ? flags.at("save") : "emergent_genome.json";
    nlohmann::json j = best.toJson();
    // Be precise about what this genome is. It was fitted on 100% of the data,
    // including whatever windows the walk-forward folds used as their test
    // sets - so it is NOT the genome those out-of-sample numbers describe, and
    // stamping it "validated" would be exactly the kind of claim this tool
    // exists to stop making. What the folds validated is the SEARCH PROCEDURE;
    // this artifact is that procedure's output on all available data.
    j["_provenance"] = {
        {"environments", envs.size()},
        {"population", population},
        {"generations", generations},
        {"candidates_evaluated", trials},
        {"fitted_on", "all available data (in-sample)"},
        {"walk_forward_folds", wfFolds},
        {"procedure_validated_out_of_sample", !folds.empty()},
        {"this_genome_validated_out_of_sample", false},
        {"note", folds.empty()
                     ? "No holdout was used. These results are the search's own selection "
                       "criterion and carry no predictive weight."
                     : "The walk-forward verdict measured the search procedure, not this "
                       "genome. This genome saw every bar it was scored on."},
    };
    std::ofstream out(savePath);
    out << j.dump(2);
    out.close();
    if (!out) {
        std::cerr << "\nFAILED to write genome to '" << savePath << "' - the result is lost.\n";
        return 1;
    }
    std::cout << "\nSaved genome to '" << savePath << "'.\n"
                 "  It is fitted on ALL the data. The out-of-sample verdict above judged the\n"
                 "  search procedure that produced it, not this particular genome.\n"
                 "  Reuse with: backtest --strategy emergent --genome-file " << savePath << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// tournament: survival-of-the-fittest across the whole strategy zoo.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// .env support. Credentials are still only ever read from the environment;
// this just fills the environment from a local file so keys never appear in
// shell history or process listings of the command line.
//
// Rules: real environment always wins (setenv with overwrite=0); values are
// never printed or logged anywhere; common alias names (API-KEY, SECRET-KEY,
// as Poloniex's own UI labels them) map onto the canonical variables.
// ---------------------------------------------------------------------------
int loadDotEnv(const std::string& path = ".env") {
    std::ifstream in(path);
    if (!in) return 0;
    auto trim = [](std::string v) {
        size_t a = v.find_first_not_of(" \t\r\n");
        size_t b = v.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        v = v.substr(a, b - a + 1);
        if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') ||
                              (v.front() == '\'' && v.back() == '\'')))
            v = v.substr(1, v.size() - 2);
        return v;
    };
    auto canonical = [](std::string k) -> std::string {
        for (auto& c : k) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        std::replace(k.begin(), k.end(), '-', '_');
        if (k == "API_KEY" || k == "KEY" || k == "POLONIEX_API_KEY") return "POLONIEX_API_KEY";
        if (k == "SECRET_KEY" || k == "API_SECRET" || k == "SECRET" ||
            k == "POLONIEX_API_SECRET") return "POLONIEX_API_SECRET";
        return "";  // unknown keys are ignored, not exported blindly
    };
    int loaded = 0;
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.rfind("export ", 0) == 0) t = trim(t.substr(7));
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string name = canonical(trim(t.substr(0, eq)));
        std::string value = trim(t.substr(eq + 1));
        if (name.empty() || value.empty()) continue;
        if (setenv(name.c_str(), value.c_str(), /*overwrite=*/0) == 0) ++loaded;
    }
    return loaded;
}

// ---------------------------------------------------------------------------
// order: place ONE real order from the command line. Exists for exactly two
// jobs - verifying the order path with a throwaway trade, and deliberate
// operator actions like funding a book - and is deliberately inconvenient for
// anything beyond that: symbol, side and size are all explicit with no
// defaults, and the order is an aggressive IOC (fills what it can immediately
// at the book's actual price, cancels the rest, never rests on the book).
// ---------------------------------------------------------------------------
int cmdOrder(const std::map<std::string, std::string>& flags) {
    auto need = [&](const char* k) -> std::string {
        auto it = flags.find(k);
        if (it == flags.end() || it->second.empty())
            throw std::runtime_error(std::string("order requires --") + k);
        return it->second;
    };
    std::string symbol = need("symbol");
    std::string side = need("side");
    if (side != "buy" && side != "sell")
        throw std::runtime_error("--side must be buy or sell");
    bool hasAmount = flags.count("amount") > 0, hasQuote = flags.count("quote") > 0;
    if (hasAmount == hasQuote)
        throw std::runtime_error("give exactly one of --amount (base units) or "
                                  "--quote (quote-currency value to trade)");
    // How far past the last close the IOC limit is set, to guarantee crossing
    // the spread. The fill happens at the book's actual price; this only
    // bounds the worst acceptable one.
    double aggression = flagDouble(flags, "aggression", 0.01);

    // Reference price: the last CLOSED 5m candle, fetched fresh from the venue.
    RateLimiter limiter(3.0, 3);
    PoloniexSource source(limiter);
    int64_t now = std::time(nullptr);
    auto candles = source.fetchHistory(symbol, 300, now - 3600);
    if (candles.empty()) {
        std::cerr << "could not fetch a reference price for " << symbol << "\n";
        return 1;
    }
    double last = candles.back().close;
    double limitPrice = side == "buy" ? last * (1.0 + aggression) : last * (1.0 - aggression);
    double amount = hasAmount ? std::stod(flags.at("amount"))
                              : std::stod(flags.at("quote")) / last;
    if (amount <= 0.0) throw std::runtime_error("amount must be positive");

    PoloniexTradingClient client;
    std::cout << side << " " << std::setprecision(10) << amount << " " << symbol
              << "  (last " << last << ", IOC limit " << limitPrice << ", ~"
              << std::fixed << std::setprecision(2) << amount * last << " quote)\n";
    OrderResult r = side == "buy" ? client.buy(symbol, limitPrice, amount)
                                  : client.sell(symbol, limitPrice, amount);
    std::cout << "ok=" << r.ok << "  orderId=" << (r.orderId.empty() ? "-" : r.orderId)
              << "  filled=" << std::defaultfloat << std::setprecision(10) << r.filledAmount
              << "  avgPrice=" << r.filledPrice << "  fee=" << r.fee << "\n";
    if (!r.ok || r.filledAmount <= 0.0) {
        std::cout << "raw: " << (r.raw.size() < 1500 ? r.raw : r.raw.substr(0, 1500)) << "\n";
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// balances: read the real account's holdings. The only subcommand that talks
// to the private API outside of `run --mode live`, and it is read-only.
// ---------------------------------------------------------------------------
int cmdBalances(const std::map<std::string, std::string>& flags) {
    std::string dataDir = flags.count("data-dir") ? flags.at("data-dir") : "data";
    std::unique_ptr<PoloniexTradingClient> client;
    try {
        client = std::make_unique<PoloniexTradingClient>();
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n\nOr put them in a .env file next to the binary:\n"
                     "  API-KEY=...\n  SECRET-KEY=...\n";
        return 1;
    }

    auto result = client->balances("BTC_USDT");  // raw carries ALL currencies
    nlohmann::json j = nlohmann::json::parse(result.raw, nullptr, false);
    if (!result.ok || j.is_discarded() || !j.is_array()) {
        std::cerr << "Balances request failed.\n"
                  << (result.raw.size() < 2000 ? result.raw : result.raw.substr(0, 2000)) << "\n"
                  << "\nIf the response mentions authentication or IP: this key may be\n"
                     "IP-restricted to a different address than this machine's.\n";
        return 1;
    }

    // Live marks for EVERY currency, from one public call.
    //
    // This used to price only currencies that happened to have a local candle
    // store, so anything else printed "(no mark)" and silently vanished from
    // the total - the account showed 77 USDT less than it held (NEAR, FET,
    // BTT, ETHW). A total that omits assets without saying so is worse than no
    // total. One GET /markets/price covers every market; the local store is
    // kept only as an offline fallback.
    std::map<std::string, double> livePrice;
    {
        HttpClient http;
        try {
            std::string body = http.get("https://api.poloniex.com/markets/price");
            auto arr = nlohmann::json::parse(body, nullptr, false);
            if (arr.is_array()) {
                for (const auto& m : arr) {
                    std::string sym = m.value("symbol", std::string());
                    std::string px = m.value("price", std::string());
                    if (sym.size() > 5 && sym.compare(sym.size() - 5, 5, "_USDT") == 0 && !px.empty()) {
                        try { livePrice[sym.substr(0, sym.size() - 5)] = std::stod(px); } catch (...) {}
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "(live prices unavailable: " << e.what()
                      << " - falling back to stored candles)\n";
        }
    }
    auto markUsdt = [&](const std::string& ccy) -> double {
        if (ccy == "USDT" || ccy == "USDC" || ccy == "USDD" || ccy == "USDS") return 1.0;
        auto it = livePrice.find(ccy);
        if (it != livePrice.end() && it->second > 0.0) return it->second;
        CandleStore store(storePath(dataDir, ccy + "_USDT", 14400));
        if (!store.exists()) return 0.0;
        CandleSeries s = store.load();
        return s.empty() ? 0.0 : s.close.back();
    };

    std::cout << "===== Poloniex account holdings =====\n";
    std::cout << std::left << std::setw(10) << "account" << std::setw(9) << "currency"
              << std::right << std::setw(18) << "available" << std::setw(16) << "on hold"
              << std::setw(16) << "~USDT value" << "\n";
    std::cout << std::string(69, '-') << "\n";
    double totalUsdt = 0.0;
    size_t shown = 0;
    size_t unpriced = 0;
    for (const auto& account : j) {
        if (!account.is_object() || !account.contains("balances") ||
            !account["balances"].is_array())
            continue;
        std::string type = account.value("accountType", std::string("?"));
        for (const auto& b : account["balances"]) {
            std::string ccy = b.value("currency", std::string());
            double avail = 0.0, hold = 0.0;
            try { avail = std::stod(b.value("available", std::string("0"))); } catch (...) {}
            try { hold = std::stod(b.value("hold", std::string("0"))); } catch (...) {}
            if (b.contains("available") && b["available"].is_number())
                avail = b["available"].get<double>();
            if (b.contains("hold") && b["hold"].is_number()) hold = b["hold"].get<double>();
            if (avail == 0.0 && hold == 0.0) continue;
            double mark = markUsdt(ccy);
            double value = mark > 0.0 ? (avail + hold) * mark : 0.0;
            if (mark <= 0.0) ++unpriced;
            totalUsdt += value;
            ++shown;
            std::cout << std::left << std::setw(10) << type << std::setw(9) << ccy
                      << std::right << std::fixed << std::setprecision(8) << std::setw(18) << avail
                      << std::setw(16) << hold << std::setprecision(2) << std::setw(16)
                      << (mark > 0.0 ? std::to_string(value).substr(0, 12) : std::string("(no mark)"))
                      << "\n";
        }
    }
    if (shown == 0) std::cout << "(no nonzero balances)\n";
    if (unpriced > 0)
        std::cout << "  (" << unpriced << " holding(s) had no USDT market and are NOT in the total)\n";
    std::cout << std::string(69, '-') << "\n"
              << "Estimated total: ~" << std::fixed << std::setprecision(2) << totalUsdt
              << " USDT  (live marks from GET /markets/price;\n"
                 " stablecoins at 1.00; stored 4h closes used only if that call fails)\n";
    return 0;
}

std::vector<std::string> parseCsvList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) if (!tok.empty()) out.push_back(tok);
    return out;
}

int cmdTournament(const std::map<std::string, std::string>& flags) {
    auto envsOpt = loadEnvironments(flags, "tournament");
    if (!envsOpt) return 1;

    evolution::TournamentConfig cfg;
    cfg.backtest = buildBacktestConfig(flags);
    cfg.population = flagInt(flags, "population", cfg.population);
    cfg.generations = flagInt(flags, "generations", cfg.generations);
    cfg.surviveThreshold = flagDouble(flags, "survive-threshold", cfg.surviveThreshold);
    cfg.trainFraction = flagDouble(flags, "train-frac", cfg.trainFraction);
    cfg.warmupBars = flagInt(flags, "warmup-bars", cfg.warmupBars);
    cfg.mutationSigma = flagDouble(flags, "mutation-sigma", cfg.mutationSigma);
    cfg.crossoverRate = flagDouble(flags, "crossover-rate", cfg.crossoverRate);
    cfg.immigrantFraction = flagDouble(flags, "immigrant-frac", cfg.immigrantFraction);
    cfg.eliteKeep = flagInt(flags, "elite", cfg.eliteKeep);
    cfg.seedsPerFamily = flagInt(flags, "seeds-per-family", cfg.seedsPerFamily);
    cfg.seed = static_cast<unsigned>(flagInt(flags, "seed", static_cast<int>(cfg.seed)));
    cfg.jobs = flagInt(flags, "jobs", 0);
    if (flags.count("only")) cfg.onlyFamilies = parseCsvList(flags.at("only"));
    if (flags.count("exclude")) cfg.excludeFamilies = parseCsvList(flags.at("exclude"));
    if (flags.count("no-gates")) cfg.allowGates = false;
    if (flags.count("no-vol-target")) cfg.allowVolTarget = false;
    cfg.costLimit = flagDouble(flags, "cost-limit", cfg.costLimit);
    cfg.eliteKeepPerFamily = flagInt(flags, "elite-per-family", cfg.eliteKeepPerFamily);
    if (flags.count("no-speciate")) cfg.speciate = false;
    cfg.tuneGenerations = flagInt(flags, "tune-generations", cfg.tuneGenerations);
    cfg.tuneKeepPerFamily = flagInt(flags, "tune-keep", cfg.tuneKeepPerFamily);
    cfg.fitness.drawdownTolerance = flagDouble(flags, "dd-tolerance", cfg.fitness.drawdownTolerance);
    cfg.fitness.minTrades = flagInt(flags, "min-trades", cfg.fitness.minTrades);

    if (cfg.trainFraction <= 0.2 || cfg.trainFraction >= 0.95) {
        std::cerr << "--train-frac must be in (0.2, 0.95); the holdout has to be big enough to "
                     "mean something and the training window big enough to search in.\n";
        return 1;
    }

    // Split every environment into a training window evolution may see and a
    // holdout it may not. The holdout carries a warm-up prefix so indicators
    // are warm at its first scored bar - without it the strategy sits blind
    // for its warm-up while buy-and-hold is invested from bar one, which
    // biases every out-of-sample comparison against the strategy.
    std::vector<evolution::Environment> tenvs;
    std::cout << "Environments:\n";
    for (const auto& e : *envsOpt) {
        size_t n = e.series.size();
        size_t split = static_cast<size_t>(cfg.trainFraction * n);
        if (split < 300 || n - split < 200) {
            std::cerr << "  " << e.label << ": only " << n
                      << " bars - not enough to split into a searchable train window and a "
                         "meaningful holdout.\n";
            return 1;
        }
        evolution::Environment te;
        te.label = e.label;
        te.train = e.series.slice(std::nullopt, e.series.timestamp[split - 1]);
        size_t warmStart = split > static_cast<size_t>(cfg.warmupBars)
                               ? split - static_cast<size_t>(cfg.warmupBars) : 0;
        te.holdoutFull = e.series.slice(e.series.timestamp[warmStart], std::nullopt);
        te.holdoutFrom = e.series.timestamp[split];
        std::cout << "  " << std::left << std::setw(22) << e.label << std::right
                  << " train " << std::setw(6) << te.train.size() << " bars ("
                  << std::fixed << std::setprecision(2) << te.train.spanYears() << "y)"
                  << "   holdout " << std::setw(6) << (n - split) << " bars ("
                  << (e.series.spanYears() - te.train.spanYears()) << "y)\n";
        tenvs.push_back(std::move(te));
    }

    size_t familyCount = 0;
    for (const auto& f : zoo::families()) {
        if (!cfg.onlyFamilies.empty() &&
            std::find(cfg.onlyFamilies.begin(), cfg.onlyFamilies.end(), f.name) == cfg.onlyFamilies.end())
            continue;
        if (std::find(cfg.excludeFamilies.begin(), cfg.excludeFamilies.end(), f.name) !=
            cfg.excludeFamilies.end())
            continue;
        if (cfg.onlyFamilies.empty() && f.costWeight > cfg.costLimit) {
            std::cout << "Excluded on cost: " << f.name << " (weight " << f.costWeight
                      << " > --cost-limit " << cfg.costLimit
                      << "). Run it with --only " << f.name << " to search it anyway.\n";
            continue;
        }
        ++familyCount;
    }
    if (familyCount == 0) {
        std::cerr << "No strategy families selected. Check --only/--exclude.\n";
        return 1;
    }

    std::cout << "\nTournament: " << familyCount << " families, population " << cfg.population
              << ", " << cfg.generations << " generations\n"
              << "Survival threshold X = " << std::fixed << std::setprecision(3)
              << cfg.surviveThreshold
              << " on fitness = min over environments of "
                 "(excess Sharpe vs buy&hold - drawdown penalty)\n"
              << "Structural genes: regime gate " << (cfg.allowGates ? "on" : "off")
              << ", volatility target " << (cfg.allowVolTarget ? "on" : "off") << "\n"
              << "Speciation: " << (cfg.speciate ? "on - breeding capacity is split across "
                                                    "surviving families, so one early winner\n"
                                                    "            cannot take every slot and leave "
                                                    "the rest untuned"
                                                 : "off - global rank decides every breeding slot")
              << "\n\n";

    evolution::Tournament tournament(std::move(tenvs), cfg);
    auto outcome = tournament.run(std::cout);

    // ---- results ---------------------------------------------------------
    std::string worstEnvName;
    auto minMeanExcess = [&worstEnvName](const std::vector<BacktestReport>& reports, double& mn,
                                          double& mean, double& worstDd, size_t& trades,
                                          double& expos) {
        mn = 1e9; mean = 0.0; worstDd = 0.0; trades = 0; expos = 0.0;
        worstEnvName = "-";
        if (reports.empty()) { mn = 0.0; return; }
        for (const auto& r : reports) {
            if (r.excessSharpe < mn) worstEnvName = r.symbol;
            mn = std::min(mn, r.excessSharpe);
            mean += r.excessSharpe;
            worstDd = std::max(worstDd, r.strategy.maxDrawdownPct);
            trades += r.numTrades;
            expos += r.exposurePct;
        }
        mean /= static_cast<double>(reports.size());
        expos /= static_cast<double>(reports.size());
    };

    // The table that actually answers "which of these ideas is worth
    // anything". Every family in it was given the same tuning budget, so a
    // family placing low here placed low after being adapted, not because it
    // was left at its published defaults while another family was tuned.
    std::cout << "\n===== Every family, after tuning =====\n";
    std::cout << std::left << std::setw(34) << "best genome of the family" << std::right
              << std::setw(9) << "train"
              << std::setw(11) << "hold(min)"
              << std::setw(11) << "hold(mean)"
              << std::setw(9) << "hold DD"
              << std::setw(9) << "trades"
              << "  " << std::left << std::setw(12) << "worst env" << std::right << "\n";
    std::cout << std::string(97, '-') << "\n";
    for (size_t i = 0; i < outcome.familyChampions.size(); ++i) {
        const auto& ind = outcome.familyChampions[i];
        double mn, mean, dd, expos; size_t trades;
        minMeanExcess(outcome.familyChampionHoldout[i].reports, mn, mean, dd, trades, expos);
        bool isControl = ind.family.rfind("control_", 0) == 0;
        std::cout << std::left << std::setw(34)
                  << (ind.label() + (isControl ? "  <- CONTROL" : "")).substr(0, 33)
                  << std::right << std::fixed << std::setprecision(3)
                  << std::setw(9) << ind.fitness
                  << std::setw(11) << mn
                  << std::setw(11) << mean
                  << std::setprecision(1) << std::setw(8) << dd << "%"
                  << std::setw(9) << trades
                  << "  " << std::left << std::setw(12) << worstEnvName.substr(0, 12)
                  << std::right << "\n";
    }

    std::cout << "\n===== Finalists: train (searched) vs holdout (never seen) =====\n";
    std::cout << std::left << std::setw(34) << "strategy" << std::right
              << std::setw(9) << "train"
              << std::setw(11) << "hold(min)"
              << std::setw(11) << "hold(mean)"
              << std::setw(9) << "hold DD"
              << std::setw(9) << "trades"
              << std::setw(9) << "expos%" << "\n";
    std::cout << std::string(92, '-') << "\n";

    int positiveHoldout = 0;
    int beatNull = 0;
    double bestHoldoutMin = -1e9;
    size_t bestIdx = 0;
    for (size_t i = 0; i < outcome.finalists.size(); ++i) {
        const auto& ind = outcome.finalists[i];
        double mn, mean, dd, expos; size_t trades;
        minMeanExcess(outcome.finalistHoldout[i].reports, mn, mean, dd, trades, expos);
        bool isControl = ind.family.rfind("control_", 0) == 0;
        if (!isControl) {
            if (mn > 0.0) ++positiveHoldout;
            if (outcome.haveControl && ind.fitness > outcome.bestControlTrainFitness) ++beatNull;
            if (mn > bestHoldoutMin) { bestHoldoutMin = mn; bestIdx = i; }
        }
        std::string tag = isControl ? " <- CONTROL" : "";
        if (!isControl && outcome.haveControl && ind.fitness <= outcome.bestControlTrainFitness)
            tag = " <- below null";
        std::cout << std::left << std::setw(34) << (ind.label() + tag).substr(0, 33)
                  << std::right << std::fixed << std::setprecision(3)
                  << std::setw(9) << ind.fitness
                  << std::setw(11) << mn
                  << std::setw(11) << mean
                  << std::setprecision(1)
                  << std::setw(8) << dd << "%"
                  << std::setw(9) << trades
                  << std::setw(8) << expos << "%\n";
    }

    std::cout << "\nCandidates evaluated: " << outcome.totalCandidates
              << " (" << outcome.distinctCandidates << " distinct genomes)\n";

    std::cout << "\n===== The empirical null =====\n";
    if (outcome.haveControl) {
        std::cout << "Control genomes bred and selected alongside everything else: "
                  << outcome.controlGenomes << "\n"
                  << "Best train fitness reached by a CONTROL:  " << std::fixed
                  << std::setprecision(3) << outcome.bestControlTrainFitness
                  << "   (" << outcome.bestControlLabel << ")\n"
                  << "  that control's holdout min excess Sharpe: "
                  << outcome.bestControlHoldoutMin << "\n\n";
        if (outcome.bestControlTrainFitness >= cfg.surviveThreshold) {
            std::cout << "  The survival threshold X = " << cfg.surviveThreshold
                      << " IS PASSABLE BY LUCK at this population and\n"
                         "  generation count: coin-flip entries cleared it. Any survivor whose\n"
                         "  train fitness is at or below " << outcome.bestControlTrainFitness
                      << " is not distinguishable from noise,\n"
                         "  and is marked 'below null' above. Raise X, widen the environment set,\n"
                         "  or judge on the holdout columns only.\n\n";
        } else {
            std::cout << "  No control reached the survival threshold, so X = "
                      << cfg.surviveThreshold << " is not passable by luck\n"
                         "  at this population and generation count.\n\n";
        }
        std::cout << "Real families whose train fitness beat the best control: " << beatNull
                  << "/" << outcome.finalists.size() << "\n";
    } else {
        std::cout << "No control family was in the population (--only/--exclude removed it),\n"
                     "so there is no empirical null and the numbers above are uncalibrated.\n";
    }
    std::cout << "Non-control finalists with POSITIVE holdout excess Sharpe in EVERY environment: "
              << positiveHoldout << "\n";
    if (bestHoldoutMin > -1e8) {
        std::cout << "Best non-control finalist by holdout: " << outcome.finalists[bestIdx].label()
                  << "  (holdout min excess Sharpe " << std::fixed << std::setprecision(3)
                  << bestHoldoutMin << ")\n";
        // The analytic counterpart to the empirical null: the Sharpe the
        // luckiest of N trials posts when none of them has any edge, and the
        // probability the true Sharpe is positive after correcting for N.
        double worstDsr = 1.0, worstBar = 0.0;
        for (const auto& r : outcome.finalistHoldout[bestIdx].reports) {
            worstDsr = std::min(worstDsr, r.deflatedSharpe);
            worstBar = std::max(worstBar, r.expectedMaxSharpeUnderNull);
        }
        std::cout << "  after correcting for " << outcome.distinctCandidates << " candidates:\n"
                  << "    Sharpe expected from luck alone (worst env): " << worstBar << "\n"
                  << "    deflated Sharpe, worst environment:          " << worstDsr
                  << (worstDsr >= 0.95 ? "   <- significant" : "   <- NOT significant") << "\n";
    }

    std::cout << "\n===== Did selection transfer? =====\n"
                 "Rank correlation between what the search maximized (train fitness) and what\n"
                 "the holdout delivered, across the " << outcome.rankedFamilies
              << " tuned family champions.\n\n";
    auto verdictLine = [](double rho, double p) {
        if (std::isnan(rho)) return std::string("too few families to measure");
        std::ostringstream os;
        os << std::fixed << std::setprecision(3) << "rho = " << rho << ", permutation p = " << p;
        os << (p < 0.05 ? (rho > 0 ? "   <- transfers" : "   <- transfers, WRONG WAY")
                        : "   <- no evidence of transfer");
        return os.str();
    };
    std::cout << "  all families:        " << verdictLine(outcome.rhoAll, outcome.pAll) << "\n"
              << "  better half only:    " << verdictLine(outcome.rhoTopHalf, outcome.pTopHalf)
              << "\n\n"
                 "  Read the second line, not the first. A search that only knows which ideas\n"
                 "  are hopeless will score well on the first line while being useless for the\n"
                 "  decision anybody actually makes, which is choosing among the good ones.\n";

    if (outcome.controlHoldoutRank > 0) {
        std::cout << "\n  On the holdout, the coin-flip control ranked "
                  << outcome.controlHoldoutRank << " of " << outcome.holdoutRankedCount
                  << " family champions.\n";
        if (outcome.controlHoldoutRank <= 3)
            std::cout << "  Nothing in this pool generalized meaningfully better than random\n"
                         "  entries. Whatever the top of the train column says, that is the\n"
                         "  result.\n";
    }

    std::cout << "\n===== Control survival, selection phase only =====\n"
                 "(during the tuning phase every family keeps its best few by construction,\n"
                 " so control survival there says nothing.)\n";
    bool sawControl = false;
    for (const auto& rec : outcome.history) {
        if (rec.tuning) continue;
        for (const auto& [fam, count] : rec.familyCensus) {
            if (fam.rfind("control_", 0) == 0) {
                std::cout << "  gen " << rec.generation << ": " << fam
                          << " survivors = " << count << "\n";
                sawControl = true;
            }
        }
    }
    if (!sawControl)
        std::cout << "  No control candidate ever cleared the survival threshold. That is the\n"
                     "  result you want: the threshold is not passable by luck here.\n";

    if (flags.count("save")) {
        nlohmann::json j;
        j["config"] = {{"population", cfg.population},
                       {"generations", cfg.generations},
                       {"surviveThreshold", cfg.surviveThreshold},
                       {"trainFraction", cfg.trainFraction},
                       {"seed", cfg.seed},
                       {"candidatesEvaluated", outcome.totalCandidates},
                       {"distinctGenomes", outcome.distinctCandidates}};
        j["transfer"] = {{"rhoAll", outcome.rhoAll}, {"pAll", outcome.pAll},
                          {"rhoTopHalf", outcome.rhoTopHalf}, {"pTopHalf", outcome.pTopHalf},
                          {"rankedFamilies", outcome.rankedFamilies},
                          {"controlHoldoutRank", outcome.controlHoldoutRank},
                          {"holdoutRankedCount", outcome.holdoutRankedCount},
                          {"bestControlTrainFitness", outcome.bestControlTrainFitness}};
        for (const auto& rec : outcome.history) {
            nlohmann::json g;
            g["generation"] = rec.generation;
            g["evaluated"] = rec.evaluated;
            g["survivors"] = rec.survivors;
            g["bestFitness"] = rec.bestFitness;
            g["medianFitness"] = rec.medianFitness;
            g["bestLabel"] = rec.bestLabel;
            g["familyCensus"] = rec.familyCensus;
            j["history"].push_back(g);
        }
        for (size_t i = 0; i < outcome.finalists.size(); ++i) {
            const auto& ind = outcome.finalists[i];
            nlohmann::json f;
            f["family"] = ind.family;
            f["label"] = ind.label();
            f["params"] = ind.params;
            const auto* spec = zoo::findFamily(ind.family);
            if (spec) {
                std::vector<std::string> names;
                for (const auto& p : spec->params) names.push_back(p.name);
                f["paramNames"] = names;
                f["provenance"] = spec->provenance;
            }
            f["gate"] = evolution::gateName(ind.gate);
            f["gateGenes"] = ind.gateGenes;
            f["volTarget"] = ind.volTarget();
            f["trainFitness"] = ind.fitness;
            for (const auto& r : outcome.finalistTrain[i].reports)
                f["train"].push_back({{"env", r.symbol}, {"excessSharpe", r.excessSharpe},
                                       {"sharpe", r.strategy.sharpeAnnual},
                                       {"benchSharpe", r.benchmark.sharpeAnnual},
                                       {"maxDD", r.strategy.maxDrawdownPct},
                                       {"trades", r.numTrades}});
            for (const auto& r : outcome.finalistHoldout[i].reports)
                f["holdout"].push_back({{"env", r.symbol}, {"excessSharpe", r.excessSharpe},
                                         {"sharpe", r.strategy.sharpeAnnual},
                                         {"benchSharpe", r.benchmark.sharpeAnnual},
                                         {"excessCagr", r.excessCagrPct},
                                         {"maxDD", r.strategy.maxDrawdownPct},
                                         {"trades", r.numTrades},
                                         {"exposurePct", r.exposurePct}});
            j["finalists"].push_back(f);
        }
        for (size_t i = 0; i < outcome.familyChampions.size(); ++i) {
            const auto& ind = outcome.familyChampions[i];
            nlohmann::json f;
            f["family"] = ind.family;
            f["label"] = ind.label();
            f["params"] = ind.params;
            const auto* spec = zoo::findFamily(ind.family);
            if (spec) {
                std::vector<std::string> names;
                for (const auto& p : spec->params) names.push_back(p.name);
                f["paramNames"] = names;
                f["provenance"] = spec->provenance;
            }
            f["gate"] = evolution::gateName(ind.gate);
            f["volTarget"] = ind.volTarget();
            f["trainFitness"] = ind.fitness;
            for (const auto& r : outcome.familyChampionHoldout[i].reports)
                f["holdout"].push_back({{"env", r.symbol}, {"excessSharpe", r.excessSharpe},
                                         {"sharpe", r.strategy.sharpeAnnual},
                                         {"benchSharpe", r.benchmark.sharpeAnnual},
                                         {"excessCagr", r.excessCagrPct},
                                         {"maxDD", r.strategy.maxDrawdownPct},
                                         {"trades", r.numTrades},
                                         {"exposurePct", r.exposurePct}});
            j["familyChampions"].push_back(f);
        }
        std::ofstream out(flags.at("save"));
        out << j.dump(2);
        if (!out) {
            std::cerr << "FAILED to write results to '" << flags.at("save") << "'\n";
            return 1;
        }
        std::cout << "\nSaved full results to '" << flags.at("save") << "'\n";
    }

    std::cout << "\nRead this the way the rest of the repo asks you to: the train column was\n"
                 "maximized over and means nothing on its own. Only the holdout columns are\n"
                 "evidence, they were produced by " << outcome.distinctCandidates
              << " distinct candidates competing for them,\nand the finalists were still chosen "
                 "by train fitness - so even the holdout figures\ncarry the selection of "
              << outcome.finalists.size() << " survivors out of that pool.\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    loadDotEnv();  // fills POLONIEX_API_* from ./.env; real environment wins
    if (argc < 2) {
        printUsage();
        return 1;
    }
    std::string command = argv[1];
    auto flags = parseFlags(argc, argv, 2);

    try {
        if (command == "fetch") return cmdFetch(flags);
        if (command == "validate") return cmdValidate(flags);
        if (command == "backtest") return cmdBacktest(flags);
        if (command == "list-strategies") return cmdListStrategies();
        if (command == "run") return cmdRun(flags);
        if (command == "status") return cmdStatus(flags);
        if (command == "parity") return cmdParity(flags);
        if (command == "order-spectrum") return cmdOrderSpectrum(flags);
        if (command == "portfolio") return cmdPortfolio(flags);
        if (command == "xsmom") return cmdXsMom(flags);
        if (command == "evolve") return cmdEvolve(flags);
        if (command == "evolve-strategy") return cmdEvolveStrategy(flags);
        if (command == "tournament") return cmdTournament(flags);
        if (command == "balances") return cmdBalances(flags);
        if (command == "order") return cmdOrder(flags);
        if (command == "--help" || command == "-h") { printUsage(); return 0; }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "Unknown command: " << command << "\n\n";
    printUsage();
    return 1;
}
