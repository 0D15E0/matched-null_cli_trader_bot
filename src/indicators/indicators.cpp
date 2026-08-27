#include "indicators.h"
#include <numeric>
#include <cmath>
#include <limits>
#include <algorithm>

namespace trader::indicators {

static constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

std::vector<double> sma(const std::vector<double>& v, int window) {
    std::vector<double> out(v.size(), NaN);
    if (window <= 0 || v.size() < static_cast<size_t>(window)) return out;
    double sum = std::accumulate(v.begin(), v.begin() + window, 0.0);
    out[window - 1] = sum / window;
    for (size_t i = window; i < v.size(); ++i) {
        sum += v[i] - v[i - window];
        out[i] = sum / window;
    }
    return out;
}

std::vector<double> ema(const std::vector<double>& v, int window) {
    std::vector<double> out(v.size(), NaN);
    if (window <= 0 || v.size() < static_cast<size_t>(window)) return out;
    // Seed with the SMA of the first `window` values and stay NaN before
    // that, so callers' NaN checks actually mean "warmed up" - seeding at
    // v[0] made the first several dozen bars look valid while the average
    // was still dominated by a single price.
    double sum = std::accumulate(v.begin(), v.begin() + window, 0.0);
    double prev = sum / window;
    out[window - 1] = prev;
    double k = 2.0 / (window + 1.0);
    for (size_t i = window; i < v.size(); ++i) {
        prev = v[i] * k + prev * (1.0 - k);
        out[i] = prev;
    }
    return out;
}

std::vector<double> rsi(const std::vector<double>& close, int window) {
    std::vector<double> out(close.size(), NaN);
    if (window <= 0 || close.size() <= static_cast<size_t>(window)) return out;

    double gainSum = 0.0, lossSum = 0.0;
    for (int i = 1; i <= window; ++i) {
        double delta = close[i] - close[i - 1];
        if (delta > 0) gainSum += delta; else lossSum -= delta;
    }
    double avgGain = gainSum / window;
    double avgLoss = lossSum / window;
    auto rsiFromAvg = [](double ag, double al) {
        if (al == 0.0) return 100.0;
        double rs = ag / al;
        return 100.0 - (100.0 / (1.0 + rs));
    };
    out[window] = rsiFromAvg(avgGain, avgLoss);

    for (size_t i = window + 1; i < close.size(); ++i) {
        double delta = close[i] - close[i - 1];
        double gain = delta > 0 ? delta : 0.0;
        double loss = delta < 0 ? -delta : 0.0;
        avgGain = (avgGain * (window - 1) + gain) / window;
        avgLoss = (avgLoss * (window - 1) + loss) / window;
        out[i] = rsiFromAvg(avgGain, avgLoss);
    }
    return out;
}

static std::vector<double> trueRange(const std::vector<double>& high,
                                      const std::vector<double>& low,
                                      const std::vector<double>& close) {
    std::vector<double> tr(high.size(), NaN);
    if (high.empty()) return tr;
    tr[0] = high[0] - low[0];
    for (size_t i = 1; i < high.size(); ++i) {
        double a = high[i] - low[i];
        double b = std::fabs(high[i] - close[i - 1]);
        double c = std::fabs(low[i] - close[i - 1]);
        tr[i] = std::max({a, b, c});
    }
    return tr;
}

std::vector<double> atr(const std::vector<double>& high,
                         const std::vector<double>& low,
                         const std::vector<double>& close, int window) {
    auto tr = trueRange(high, low, close);
    std::vector<double> out(tr.size(), NaN);
    if (window <= 0 || tr.size() < static_cast<size_t>(window)) return out;
    // Wilder's smoothing (RMA), seeded with the SMA of the first window of
    // true ranges. This is the reference definition; the previous plain-SMA
    // version was noisier, so an ATR-multiple stop tuned against it would
    // trail at a different distance than the same multiple on any chart.
    double seed = std::accumulate(tr.begin(), tr.begin() + window, 0.0) / window;
    out[window - 1] = seed;
    for (size_t i = window; i < tr.size(); ++i) {
        out[i] = (out[i - 1] * (window - 1) + tr[i]) / window;
    }
    return out;
}

DMIResult dmi(const std::vector<double>& high,
              const std::vector<double>& low,
              const std::vector<double>& close, int window) {
    size_t n = high.size();
    DMIResult res;
    res.plusDI.assign(n, NaN);
    res.minusDI.assign(n, NaN);
    res.adx.assign(n, NaN);
    if (n < static_cast<size_t>(window) + 1) return res;

    std::vector<double> plusDM(n, 0.0), minusDM(n, 0.0);
    auto tr = trueRange(high, low, close);

    for (size_t i = 1; i < n; ++i) {
        double upMove = high[i] - high[i - 1];
        double downMove = low[i - 1] - low[i];
        plusDM[i]  = (upMove > downMove && upMove > 0) ? upMove : 0.0;
        minusDM[i] = (downMove > upMove && downMove > 0) ? downMove : 0.0;
    }

    auto smoothed = [&](const std::vector<double>& src) {
        std::vector<double> out(n, NaN);
        double sum = 0.0;
        for (int i = 1; i <= window; ++i) sum += src[i];
        out[window] = sum;
        for (size_t i = window + 1; i < n; ++i) {
            sum = out[i - 1] - (out[i - 1] / window) + src[i];
            out[i] = sum;
        }
        return out;
    };

    auto smPlusDM = smoothed(plusDM);
    auto smMinusDM = smoothed(minusDM);
    auto smTR = smoothed(tr);

    std::vector<double> dx(n, NaN);
    for (size_t i = window; i < n; ++i) {
        if (smTR[i] == 0.0 || std::isnan(smTR[i])) continue;
        double pdi = 100.0 * smPlusDM[i] / smTR[i];
        double mdi = 100.0 * smMinusDM[i] / smTR[i];
        res.plusDI[i] = pdi;
        res.minusDI[i] = mdi;
        double denom = pdi + mdi;
        dx[i] = denom == 0.0 ? 0.0 : 100.0 * std::fabs(pdi - mdi) / denom;
    }

    // ADX = smoothed average of DX
    double sum = 0.0;
    int count = 0;
    size_t firstDx = window;
    for (size_t i = firstDx; i < std::min(n, firstDx + window); ++i) {
        if (!std::isnan(dx[i])) { sum += dx[i]; count++; }
    }
    if (count > 0 && firstDx + window - 1 < n) {
        double avg = sum / count;
        res.adx[firstDx + window - 1] = avg;
        for (size_t i = firstDx + window; i < n; ++i) {
            // A NaN DX (smoothed true range of exactly zero - a completely
            // flat window, as happens on a halted or illiquid instrument)
            // used to poison this recursion permanently, leaving ADX NaN for
            // the entire rest of the series and silently freezing every
            // strategy that gates on it. Carry the previous average through
            // the gap instead.
            if (std::isnan(dx[i])) { res.adx[i] = avg; continue; }
            avg = (avg * (window - 1) + dx[i]) / window;
            res.adx[i] = avg;
        }
    }
    return res;
}

IchimokuResult ichimoku(const std::vector<double>& high,
                         const std::vector<double>& low,
                         int tenkanWindow, int kijunWindow, int spanBWindow,
                         int displacement) {
    size_t n = high.size();
    IchimokuResult res;
    res.tenkan.assign(n, NaN);
    res.kijun.assign(n, NaN);
    res.spanA.assign(n, NaN);
    res.spanB.assign(n, NaN);

    auto highLowMid = [&](int window) {
        std::vector<double> out(n, NaN);
        for (size_t i = 0; i < n; ++i) {
            if (i + 1 < static_cast<size_t>(window)) continue; // not enough warm-up data yet
            size_t start = i + 1 - window;
            double hi = *std::max_element(high.begin() + start, high.begin() + i + 1);
            double lo = *std::min_element(low.begin() + start, low.begin() + i + 1);
            out[i] = (hi + lo) / 2.0;
        }
        return out;
    };

    res.tenkan = highLowMid(tenkanWindow);
    res.kijun  = highLowMid(kijunWindow);
    auto spanBBase = highLowMid(spanBWindow);

    // Displace the cloud forward: the span plotted at bar i is computed from
    // the window ending at bar i - shift. Reading it at index i therefore
    // compares today's price against a cloud formed `shift` bars ago, which
    // is what Ichimoku actually specifies (and, using strictly older data,
    // is more conservative than the undisplaced version this replaces).
    const size_t shift = static_cast<size_t>(displacement < 0 ? kijunWindow : displacement);
    for (size_t i = shift; i < n; ++i) {
        size_t src = i - shift;
        if (!std::isnan(res.tenkan[src]) && !std::isnan(res.kijun[src]))
            res.spanA[i] = (res.tenkan[src] + res.kijun[src]) / 2.0;
        res.spanB[i] = spanBBase[src];
    }
    return res;
}

BollingerResult bollinger(const std::vector<double>& close, int window, double numSigma) {
    BollingerResult res;
    res.mid = sma(close, window);
    res.upper.assign(close.size(), NaN);
    res.lower.assign(close.size(), NaN);
    if (window <= 0) return res;
    for (size_t i = 0; i < close.size(); ++i) {
        if (i + 1 < static_cast<size_t>(window)) continue;
        size_t start = i + 1 - window;
        double mean = res.mid[i];
        double variance = 0.0;
        for (size_t j = start; j <= i; ++j) variance += (close[j] - mean) * (close[j] - mean);
        variance /= window;
        double sigma = std::sqrt(variance);
        res.upper[i] = mean + numSigma * sigma;
        res.lower[i] = mean - numSigma * sigma;
    }
    return res;
}

std::vector<double> rollingStd(const std::vector<double>& values, int window) {
    std::vector<double> out(values.size(), NaN);
    if (window <= 1) return out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i + 1 < static_cast<size_t>(window)) continue;
        size_t start = i + 1 - window;
        double mean = 0.0;
        for (size_t j = start; j <= i; ++j) mean += values[j];
        mean /= window;
        double variance = 0.0;
        for (size_t j = start; j <= i; ++j) variance += (values[j] - mean) * (values[j] - mean);
        variance /= window;
        out[i] = std::sqrt(variance);
    }
    return out;
}

std::vector<double> percentReturns(const std::vector<double>& close) {
    std::vector<double> out(close.size(), NaN);
    for (size_t i = 1; i < close.size(); ++i) {
        if (close[i - 1] == 0.0) continue;
        out[i] = close[i] / close[i - 1] - 1.0;
    }
    return out;
}

std::vector<double> rollingVolatility(const std::vector<double>& close, int window) {
    auto rets = percentReturns(close);
    return rollingStd(rets, window);
}

std::vector<double> hurstExponent(const std::vector<double>& close, int window) {
    std::vector<double> out(close.size(), NaN);
    auto rets = percentReturns(close);
    if (window < 8) return out; // need enough bars for a couple of sub-chunk sizes

    // Classic R/S analysis: for a windowed slice of returns, compute the
    // average R/S statistic over non-overlapping chunks of a given size,
    // for a handful of chunk sizes, then fit log(R/S) vs log(chunkSize) --
    // the slope is the Hurst exponent estimate.
    auto avgRsForChunkSize = [](const std::vector<double>& r, size_t start, size_t end,
                                 size_t chunkLen) -> double {
        double rsSum = 0.0;
        int numChunks = 0;
        for (size_t c = start; c + chunkLen <= end; c += chunkLen) {
            double mean = 0.0;
            for (size_t j = c; j < c + chunkLen; ++j) mean += r[j];
            mean /= chunkLen;
            double cumDev = 0.0, minCum = 0.0, maxCum = 0.0, variance = 0.0;
            for (size_t j = c; j < c + chunkLen; ++j) {
                double dev = r[j] - mean;
                cumDev += dev;
                minCum = std::min(minCum, cumDev);
                maxCum = std::max(maxCum, cumDev);
                variance += dev * dev;
            }
            variance /= chunkLen;
            double stdev = std::sqrt(variance);
            double range = maxCum - minCum;
            if (stdev > 1e-12) {
                rsSum += range / stdev;
                ++numChunks;
            }
        }
        return numChunks > 0 ? (rsSum / numChunks) : NaN;
    };

    // Anis-Lloyd expected R/S for an INDEPENDENT series of length n. Raw R/S
    // is biased sharply upward at the chunk lengths a rolling window forces
    // (measured: mean H of 0.56 at window=100, 0.59 at window=50 on iid
    // Gaussian returns), so fitting raw log(R/S) against log(n) produces a
    // "trend detector" that reports a trend on pure noise most of the time.
    // Subtracting log(E[R/S]) re-centres the estimator on H = 0.5 for a
    // random walk, which is the only thing that makes a threshold above 0.5
    // meaningful. See Peters, "Fractal Market Analysis".
    auto expectedRs = [](size_t n) -> double {
        if (n < 2) return NaN;
        double sum = 0.0;
        for (size_t i = 1; i < n; ++i)
            sum += std::sqrt(static_cast<double>(n - i) / static_cast<double>(i));
        double nd = static_cast<double>(n);
        // The Gamma form is exact but overflows for large n; its asymptotic
        // equivalent takes over well before that happens.
        double lead = (n > 340) ? 1.0 / std::sqrt(nd * M_PI / 2.0)
                                 : std::tgamma((nd - 1.0) / 2.0) /
                                       (std::sqrt(M_PI) * std::tgamma(nd / 2.0));
        return ((nd - 0.5) / nd) * lead * sum;
    };

    // Chunk lengths are the same for every bar, so the (expensive) expected
    // R/S values are computed once rather than per bar.
    std::vector<size_t> chunkLens;
    std::vector<double> logN, logExpectedRs;
    for (int divisor : {8, 4, 2, 1}) {
        size_t chunkLen = static_cast<size_t>(window) / divisor;
        if (chunkLen < 8) continue; // below this the correction itself is unreliable
        double ers = expectedRs(chunkLen);
        if (std::isnan(ers) || ers <= 0.0) continue;
        chunkLens.push_back(chunkLen);
        logN.push_back(std::log(static_cast<double>(chunkLen)));
        logExpectedRs.push_back(std::log(ers));
    }
    if (chunkLens.size() < 2) return out;

    for (size_t i = 0; i < close.size(); ++i) {
        if (i + 1 < static_cast<size_t>(window) + 1) continue; // +1: rets[i] undefined at i=0
        size_t start = i + 1 - window; // returns slice [start, i]
        if (start == 0) start = 1;     // rets[0] is NaN, never include it
        size_t end = i + 1;            // exclusive end

        // Regress the BIAS-CORRECTED log(R/S) on log(chunkSize): the slope of
        // log(R/S) - log(E[R/S]) estimates H - 0.5.
        std::vector<double> x, y;
        x.reserve(chunkLens.size());
        y.reserve(chunkLens.size());
        for (size_t k = 0; k < chunkLens.size(); ++k) {
            double rs = avgRsForChunkSize(rets, start, end, chunkLens[k]);
            if (std::isnan(rs) || rs <= 0.0) continue;
            x.push_back(logN[k]);
            y.push_back(std::log(rs) - logExpectedRs[k]);
        }
        if (x.size() < 2) continue;

        double meanX = 0.0, meanY = 0.0;
        for (size_t k = 0; k < x.size(); ++k) { meanX += x[k]; meanY += y[k]; }
        meanX /= static_cast<double>(x.size());
        meanY /= static_cast<double>(y.size());
        double num = 0.0, den = 0.0;
        for (size_t k = 0; k < x.size(); ++k) {
            num += (x[k] - meanX) * (y[k] - meanY);
            den += (x[k] - meanX) * (x[k] - meanX);
        }
        if (den > 1e-12) out[i] = 0.5 + num / den;
    }
    return out;
}

std::vector<double> hurstStructureFunction(const std::vector<double>& close,
                                            int window, int numLags) {
    std::vector<double> out(close.size(), NaN);
    if (window < 16 || numLags < 3 || close.size() < static_cast<size_t>(window)) return out;

    // Self-affinity is a property of the LOG price: S_2 of raw prices would
    // scale with the price level and the fitted slope would drift as the
    // instrument re-prices over a decade.
    std::vector<double> logPx(close.size(), NaN);
    for (size_t i = 0; i < close.size(); ++i)
        if (close[i] > 0.0) logPx[i] = std::log(close[i]);

    // Geometric ladder of lags, so the regression points are evenly spread in
    // log tau (the axis actually fitted). The largest lag is capped at a
    // quarter of the window: beyond that only a handful of pairs contribute
    // and S_2 becomes noise.
    const int maxLag = std::max(2, window / 4);
    std::vector<int> lags;
    for (int k = 0; k < numLags; ++k) {
        double t = std::pow(static_cast<double>(maxLag),
                             static_cast<double>(k) / (numLags - 1));
        int lag = std::max(1, static_cast<int>(std::lround(t)));
        if (lags.empty() || lag > lags.back()) lags.push_back(lag);
    }
    if (lags.size() < 3) return out;

    std::vector<double> logLag(lags.size());
    for (size_t k = 0; k < lags.size(); ++k) logLag[k] = std::log(static_cast<double>(lags[k]));

    // Pre-computed for the regression: the lag ladder is fixed, so the x-axis
    // moments never change.
    double meanX = 0.0;
    for (double v : logLag) meanX += v;
    meanX /= static_cast<double>(logLag.size());
    double denom = 0.0;
    for (double v : logLag) denom += (v - meanX) * (v - meanX);
    if (denom <= 1e-12) return out;

    std::vector<double> logS(lags.size());
    for (size_t i = static_cast<size_t>(window) - 1; i < close.size(); ++i) {
        size_t start = i + 1 - static_cast<size_t>(window);
        bool ok = true;
        for (size_t k = 0; k < lags.size() && ok; ++k) {
            size_t lag = static_cast<size_t>(lags[k]);
            double sum = 0.0;
            size_t count = 0;
            for (size_t t = start; t + lag <= i; ++t) {
                double a = logPx[t + lag], b = logPx[t];
                if (std::isnan(a) || std::isnan(b)) continue;
                double d = a - b;
                sum += d * d;
                ++count;
            }
            if (count < 4 || sum <= 0.0) { ok = false; break; }
            logS[k] = std::log(sum / static_cast<double>(count));
        }
        if (!ok) continue;

        double meanY = 0.0;
        for (double v : logS) meanY += v;
        meanY /= static_cast<double>(logS.size());
        double num = 0.0;
        for (size_t k = 0; k < logS.size(); ++k) num += (logLag[k] - meanX) * (logS[k] - meanY);
        // S_2 ~ tau^(2H): the slope is 2H, so H is half of it. Clamp to the
        // theoretically admissible range rather than emitting values a caller
        // would have to sanity-check itself.
        double h = 0.5 * num / denom;
        out[i] = std::min(1.0, std::max(0.0, h));
    }
    return out;
}


std::vector<double> relativeVolume(const std::vector<double>& volume, int window) {
    const size_t n = volume.size();
    std::vector<double> out(n, std::nan(""));
    if (window < 2 || n == 0) return out;
    const size_t w = static_cast<size_t>(window);
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        if (i >= w) {
            // Mean of the w bars BEFORE i. Including bar i would let a single
            // huge bar raise its own baseline and mask exactly the expansion
            // this indicator exists to detect.
            double mean = sum / static_cast<double>(w);
            out[i] = (mean > 0.0) ? volume[i] / mean : std::nan("");
            sum -= volume[i - w];
        }
        sum += volume[i];
    }
    return out;
}

std::vector<double> onBalanceVolume(const std::vector<double>& close,
                                     const std::vector<double>& volume) {
    const size_t n = std::min(close.size(), volume.size());
    std::vector<double> out(n, std::nan(""));
    if (n == 0) return out;
    double acc = 0.0;
    out[0] = 0.0;
    for (size_t i = 1; i < n; ++i) {
        if (close[i] > close[i - 1]) acc += volume[i];
        else if (close[i] < close[i - 1]) acc -= volume[i];
        out[i] = acc;
    }
    return out;
}


std::vector<unsigned> candleSignals(const std::vector<double>& open,
                                     const std::vector<double>& high,
                                     const std::vector<double>& low,
                                     const std::vector<double>& close,
                                     double dojiBodyFrac, double shadowBodyRatio) {
    const size_t n = std::min(std::min(open.size(), high.size()),
                               std::min(low.size(), close.size()));
    std::vector<unsigned> out(n, candle::None);
    for (size_t i = 0; i < n; ++i) {
        const double o = open[i], h = high[i], l = low[i], c = close[i];
        const double range = h - l;
        if (!(range > 0.0)) continue;
        const double body = std::fabs(c - o);
        const double bodyTop = std::max(o, c), bodyBot = std::min(o, c);
        const double upper = h - bodyTop, lower = bodyBot - l;

        if (body <= dojiBodyFrac * range) out[i] |= candle::Doji;

        // Hammer: body in the upper third, lower shadow dominating. A doji-thin
        // body is excluded because the shadow ratio is then meaningless.
        if (body > dojiBodyFrac * range) {
            if (lower >= shadowBodyRatio * body && upper <= body)
                out[i] |= candle::Hammer;
            if (upper >= shadowBodyRatio * body && lower <= body)
                out[i] |= candle::ShootingStar;
        }
        if (i == 0) continue;

        const double po = open[i - 1], pc = close[i - 1];
        const double pBodyTop = std::max(po, pc), pBodyBot = std::min(po, pc);
        const bool prevBear = pc < po, prevBull = pc > po;
        const bool bull = c > o, bear = c < o;

        // Engulfing compares BODIES, which is the classical definition; using
        // the full range instead makes it fire far too often.
        if (bull && prevBear && c >= pBodyTop && o <= pBodyBot) out[i] |= candle::BullEngulfing;
        if (bear && prevBull && c <= pBodyBot && o >= pBodyTop) out[i] |= candle::BearEngulfing;

        const double pMid = 0.5 * (po + pc);
        if (bull && prevBear && o < pc && c > pMid && c < po) out[i] |= candle::Piercing;
        if (bear && prevBull && o > pc && c < pMid && c > po) out[i] |= candle::DarkCloud;

        if (i < 2) continue;
        // Star patterns: strong bar, small-bodied bar, strong bar the other way.
        const double o2 = open[i - 2], c2 = close[i - 2], h2 = high[i - 2], l2 = low[i - 2];
        const double body2 = std::fabs(c2 - o2), range2 = h2 - l2;
        const double bodyMid = std::fabs(pc - po), rangeMid = high[i - 1] - low[i - 1];
        const bool bigDown = (c2 < o2) && range2 > 0.0 && body2 >= 0.5 * range2;
        const bool bigUp = (c2 > o2) && range2 > 0.0 && body2 >= 0.5 * range2;
        const bool smallMid = rangeMid > 0.0 && bodyMid <= dojiBodyFrac * 3.0 * rangeMid;
        if (bigDown && smallMid && bull && c > 0.5 * (o2 + c2)) out[i] |= candle::MorningStar;
        if (bigUp && smallMid && bear && c < 0.5 * (o2 + c2)) out[i] |= candle::EveningStar;
    }
    return out;
}

} // namespace trader::indicators
