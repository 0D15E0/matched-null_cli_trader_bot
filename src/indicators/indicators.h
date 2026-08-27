#pragma once
#include <vector>
#include <cstddef>

namespace trader::indicators {

// All indicators operate on plain contiguous vectors (close/high/low),
// mirroring the SoA CandleSeries layout. Output vectors are aligned with
// the input by index (i.e. output[i] corresponds to input[i]); indices
// before enough warm-up data exists are filled with NaN so callers can
// detect "not yet valid" the same way the legacy code checked studies
// length, but without needing an explicit valid/queue bookkeeping struct.

std::vector<double> sma(const std::vector<double>& values, int window);

// Exponential moving average, seeded with the SMA of the first `window`
// values and NaN before that (TA-Lib convention). It used to be seeded with
// values[0] and emitted from index 0, which contradicted the NaN contract
// above and let callers treat a still-converging EMA as valid - the evolved
// genomes' EMA-ratio features were reading a series-start artifact for the
// first few dozen bars.
std::vector<double> ema(const std::vector<double>& values, int window);

std::vector<double> rsi(const std::vector<double>& close, int window);

// Average True Range with Wilder's smoothing (the standard definition).
// Previously this was a plain SMA of true range, which is noisier and means
// an ATR-multiple stop tuned here would behave differently against any
// charting package or reference implementation.
std::vector<double> atr(const std::vector<double>& high,
                         const std::vector<double>& low,
                         const std::vector<double>& close, int window);

struct DMIResult {
    std::vector<double> plusDI;
    std::vector<double> minusDI;
    std::vector<double> adx;
};
DMIResult dmi(const std::vector<double>& high,
              const std::vector<double>& low,
              const std::vector<double>& close, int window);

// JAPANESE CANDLESTICK SIGNALS, as a per-bar bitmask.
//
// Each pattern is expressed in units of the bar's OWN range or body, never in
// absolute prices, so one definition works on a $3 ETF and a $78,000 BTC bar.
// The classical descriptions are qualitative ("a long lower shadow"); the
// thresholds below make them decidable, and they are stated here rather than
// buried so they can be argued with.
//
// Signals needing 2 or 3 bars read only bars at or before i, so the mask at bar
// i is knowable at bar i.
namespace candle {
enum Signal : unsigned {
    None            = 0u,
    BullEngulfing   = 1u << 0,  // prev body bearish, this body bullish and covering it
    BearEngulfing   = 1u << 1,
    Hammer          = 1u << 2,  // small body near the top, lower shadow >= 2x body
    ShootingStar    = 1u << 3,  // mirror of Hammer
    MorningStar     = 1u << 4,  // down bar, small-bodied bar, strong up bar
    EveningStar     = 1u << 5,
    Piercing        = 1u << 6,  // opens below prev close, closes past prev midpoint
    DarkCloud       = 1u << 7,
    Doji            = 1u << 8,  // body <= 10% of range: indecision, not direction
};
// Any signal a long entry can be confirmed by.
constexpr unsigned kBullish = BullEngulfing | Hammer | MorningStar | Piercing;
constexpr unsigned kBearish = BearEngulfing | ShootingStar | EveningStar | DarkCloud;
} // namespace candle

std::vector<unsigned> candleSignals(const std::vector<double>& open,
                                     const std::vector<double>& high,
                                     const std::vector<double>& low,
                                     const std::vector<double>& close,
                                     double dojiBodyFrac = 0.10,
                                     double shadowBodyRatio = 2.0);

// VOLUME INDICATORS.
//
// Volume was present in every stored candle and used by nothing in this repo
// until 2026-08-22 - an entire column of the data ignored. These are the two
// workhorses; both are deliberately SCALE-FREE or sign-based so the same
// parameter means the same thing on a $78,000 BTC bar and a $180 AAPL bar, and
// on 5m as on daily.

// volume[i] divided by the mean volume over the preceding `window` bars
// (EXCLUDING bar i, so the comparison is against what was normal BEFORE this
// bar). > 1 means this bar traded more than recent normal. NaN until warm.
//
// Why a ratio and not a level: raw volume trends by orders of magnitude over a
// decade as an asset grows, so any fixed threshold on the level silently means
// "after 2021" rather than "busy bar".
std::vector<double> relativeVolume(const std::vector<double>& volume, int window = 20);

// On-Balance Volume: cumulative sum of signed volume, +volume when the close
// rose and -volume when it fell. The LEVEL is meaningless (it depends on where
// the series starts); only its direction over a window carries information.
// The classic use is divergence: price making highs while OBV does not is read
// as a rally without participation behind it.
std::vector<double> onBalanceVolume(const std::vector<double>& close,
                                     const std::vector<double>& volume);

struct IchimokuResult {
    std::vector<double> tenkan;   // conversion line, at bar i
    std::vector<double> kijun;    // base line, at bar i
    std::vector<double> spanA;    // leading span A, already displaced forward
    std::vector<double> spanB;    // leading span B, already displaced forward
};

// Ichimoku Kinko Hyo. spanA/spanB at index i are computed from data ending
// at bar `i - displacement`, i.e. the cloud is genuinely *leading* the way
// the indicator is defined (and the way every charting package draws it).
//
// The previous implementation documented the displacement but never applied
// it, comparing today's close to a cloud built from today's own window -
// strictly causal, but a same-bar Donchian-midpoint breakout rather than
// Ichimoku. Displacing it is the more conservative reading (older data), and
// it makes signals here agree with what a chart would show.
//
// displacement < 0 means "use kijunWindow", the standard 26.
IchimokuResult ichimoku(const std::vector<double>& high,
                         const std::vector<double>& low,
                         int tenkanWindow = 9, int kijunWindow = 26, int spanBWindow = 52,
                         int displacement = -1);

struct BollingerResult {
    std::vector<double> mid;
    std::vector<double> upper;
    std::vector<double> lower;
};
BollingerResult bollinger(const std::vector<double>& close, int window, double numSigma = 2.0);

// Rolling standard deviation of `values` over `window` bars.
std::vector<double> rollingStd(const std::vector<double>& values, int window);

// Simple bar-to-bar percentage returns: ret[i] = close[i]/close[i-1] - 1,
// ret[0] = NaN (no prior bar). Used as the input to volatility/momentum
// calculations rather than raw price, so results are scale-invariant.
std::vector<double> percentReturns(const std::vector<double>& close);

// Annualization-agnostic rolling realized volatility: the rolling standard
// deviation of `percentReturns(close)` over `window` bars. Used to
// vol-scale a position size / signal strength (see strategy/tsmom_strategy.h),
// following the standard time-series-momentum convention of sizing
// positions inversely to recent volatility (Moskowitz, Ooi & Pedersen;
// Lim, Zohren & Roberts arXiv:1904.04912) rather than trading a fixed size
// regardless of how turbulent the instrument currently is.
std::vector<double> rollingVolatility(const std::vector<double>& close, int window);

// Rolling Hurst exponent via rescaled-range (R/S) analysis on
// `percentReturns(close)`, using sub-window chunking over `window` bars
// (classic Hurst 1951 / Mandelbrot R/S method). H ~ 0.5 => random walk
// (no persistent trend or mean-reversion), H > 0.5 => trending/persistent
// regime (trend-following signals should perform better), H < 0.5 =>
// mean-reverting/anti-persistent regime (trend-following signals should
// be suppressed or inverted). Used as a regime filter/gate in front of
// trend-following strategies - see strategy/hurst_regime_filter.h.
//
// The estimate is bias-corrected against the Anis-Lloyd expected R/S of an
// independent series (as in Peters, "Fractal Market Analysis"): raw R/S
// overestimates H badly at the short chunk lengths a rolling window forces.
// Measured on iid Gaussian returns, the uncorrected estimator returned a
// mean H of 0.56 at window=100 and 0.59 at window=50 - so a "trending
// regime" gate at H >= 0.55 was passing pure noise more than half the time.
// With the correction the estimator centres on 0.5 for a random walk, which
// is what makes a threshold above 0.5 mean anything at all.
std::vector<double> hurstExponent(const std::vector<double>& close, int window);

// Rolling Hurst exponent from the q=2 STRUCTURE FUNCTION, which is a
// substantially better estimator than the rescaled-range one above and should
// be preferred for new work.
//
// For a window ending at bar i it computes
//     S_2(tau) = < (logClose[t+tau] - logClose[t])^2 >   over the window
// on a geometric ladder of lags tau, regresses log S_2 on log tau, and returns
// half the slope: S_2(tau) ~ tau^(2H) by definition of self-affinity.
//
// Why this rather than R/S, measured on synthetic series with known H:
//   * accuracy - on fractional Brownian motion with H from 0.3 to 0.7 this
//     recovers H to about +/-0.015, where bias-corrected R/S sits around 0.47
//     for a true 0.5 and lets ~25-30% of pure random-walk windows pass an
//     H >= 0.55 "trending regime" gate;
//   * sample efficiency - R/S chops the window into non-overlapping chunks and
//     discards everything else, while every pair separated by tau contributes
//     here. That matters most at the short window lengths a rolling filter
//     forces, which is exactly where R/S is weakest.
//
// Note the two estimators are NOT interchangeable at a fixed threshold: R/S
// reads low (~0.47 on a random walk) while this reads ~0.50, so a gate tuned
// against one has to be re-tuned against the other.
std::vector<double> hurstStructureFunction(const std::vector<double>& close,
                                            int window, int numLags = 8);

} // namespace trader::indicators
