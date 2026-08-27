#pragma once
#include "strategy.h"
#include "../indicators/indicators.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace trader {

// Classical chart patterns, made decidable: inverse head-and-shoulders, double
// bottom, cup-and-handle and bull flag, each confirmed on the breakout by an
// optional candlestick signal and an optional volume expansion.
//
// HOW A "SHAPE" BECOMES A RULE. Every pattern here is a CONFIGURATION OF
// CONFIRMED PIVOTS - alternating swing highs and lows with prices and indices -
// plus tolerances saying how equal "equal shoulders" has to be. That is the only
// way to make these objective. Drawing them by eye on a finished chart is where
// the published evidence for chart patterns mostly comes from, and it is not
// evidence: a shape you can only name after the breakout has already resolved
// tells you nothing you could have traded.
//
// CAUSALITY. A pivot at bar j is usable only from bar j + pivotWindow, because
// "nothing exceeded it" is not knowable before then. Every pattern is therefore
// recognised LATE, exactly as a real trader recognises it late. The breakout
// test, the candlestick mask and the volume ratio all read bars <= i. This is
// audited by rewriting every future bar and demanding no past signal moves.
//
// ONLY BULLISH PATTERNS ARE TRADED, because the engine is long/flat. Head-and-
// shoulders (bearish) and the bearish candlesticks are used, if at all, as exit
// conditions rather than entries.
//
// WHAT TO EXPECT. Nine hypotheses have now been falsified in this project, and
// four of them were exposure-reducing entry filters that each cost Sharpe. A
// pattern gate is another such filter. The reason to run it anyway is that its
// information is genuinely different in KIND - a multi-pivot geometric
// configuration rather than a threshold on an indicator - and per-pattern
// attribution below means a failure says WHICH shape failed rather than
// producing one undifferentiated negative.
struct PatternParams {
    int    pivotWindow = 8;          // bars either side defining a pivot
    int    atrWindow = 14;

    // Which shapes are active. Tested one at a time for attribution.
    bool   useDoubleBottom = true;
    bool   useInverseHS = true;
    bool   useCupHandle = true;
    bool   useBullFlag = true;
    bool   useAscTriangle = true;   // horizontal resistance, rising lows
    bool   useSymTriangle = true;   // falling highs, rising lows (converging)
    bool   useButterfly = true;     // harmonic, D extends BEYOND X
    bool   useCypher = true;        // harmonic, D = 0.786 retrace of XC

    double levelTolFrac = 0.30;      // how equal two "equal" pivots must be,
                                     // as a fraction of the pattern's height
    double minHeadDropFrac = 0.03;   // inverse H&S head must undercut the
                                     // shoulders by this fraction of price
    double minPatternAtr = 1.5;      // pattern height in ATRs; smaller is noise
    int    maxPatternBars = 250;     // a shape older than this is history
    double handleMaxFrac = 0.5;      // cup-and-handle: handle depth <= this
                                     // fraction of cup depth
    double flagMaxWidthFrac = 0.5;   // bull flag: consolidation height <= this
                                     // fraction of the flagpole
    int    flagMaxBars = 40;

    // Harmonic ratio tolerance - and this is the parameter that decides
    // whether these patterns exist at all.
    //
    // Chart reading is RECOGNITION OF FAMILY RESEMBLANCE, not measurement. A
    // trader calls a shape a Butterfly when the legs are roughly 0.786 and
    // roughly 1.27; nobody checks three decimal places. Implemented at a 12%
    // tolerance this detector found 947 geometrically-valid Butterfly
    // configurations on BTC 4h and took ZERO trades; at 25% it took 5. The
    // tolerance was the whole result.
    //
    // 30% means "0.786" is satisfied anywhere in [0.55, 1.02]. That is loose
    // enough to be honest about what recognition means and still tight enough
    // to exclude an arbitrary four-pivot sequence. Note the cost: loosening a
    // threshold until a rule fires is itself a search, so a positive result at
    // a hand-picked tolerance deserves the holdout, not a press release.
    double harmonicTol = 0.30;      // "roughly 0.786", not 0.786 exactly

    // Confirmation at the breakout bar.
    bool   requireCandle = false;    // any bullish candlestick signal
    bool   requireVolume = false;
    int    volWindow = 20;
    double volMult = 1.3;

    // Exit. Target projects the pattern height above the breakout level, the
    // classical "measured move"; stop sits under the pattern low.
    double targetHeightMult = 1.0;
    double stopBelowLowFrac = 0.02;
    bool   exitOnBearCandle = false;
};

class PatternStrategy : public Strategy {
public:
    using Params = PatternParams;
    explicit PatternStrategy(Params p = {}) : params_(p) {}
    std::string name() const override { return "patterns"; }
    int confirmBars() const { return params_.pivotWindow; }

    // Per-pattern firing counts, so a result can be attributed.
    struct Counts { long doubleBottom = 0, inverseHS = 0, cupHandle = 0, bullFlag = 0,
                    ascTriangle = 0, symTriangle = 0, butterfly = 0, cypher = 0; };
    const Counts& counts() const { return counts_; }

    void prepare(const CandleSeries& series) override {
        const size_t n = series.size();
        atr_ = indicators::atr(series.high, series.low, series.close, params_.atrWindow);
        relVol_ = indicators::relativeVolume(series.volume, params_.volWindow);
        cand_ = indicators::candleSignals(series.open, series.high, series.low, series.close);
        const int k = std::max(1, params_.pivotWindow);
        isHigh_.assign(n, false);
        isLow_.assign(n, false);
        for (size_t j = static_cast<size_t>(k); j + static_cast<size_t>(k) < n; ++j) {
            bool hi = true, lo = true;
            for (int d = -k; d <= k; ++d) {
                if (d == 0) continue;
                size_t m = j + static_cast<size_t>(d);
                if (series.high[m] >= series.high[j]) hi = false;
                if (series.low[m] <= series.low[j]) lo = false;
            }
            isHigh_[j] = hi;
            isLow_[j] = lo;
        }
        pivots_.clear();
        counts_ = Counts{};
        target_ = stop_ = std::nan("");
    }

    Signal onBar(const CandleSeries& series, size_t i, const PositionContext& pos) override {
        const int k = std::max(1, params_.pivotWindow);
        if (i < static_cast<size_t>(k) + 3) return Signal::Hold;

        // Append the newly CONFIRMED pivot, keeping alternation: two highs in a
        // row means the later/higher one supersedes, which is what a swing
        // sequence means.
        const size_t j = i - static_cast<size_t>(k);
        if (isHigh_[j]) push({j, series.high[j], true});
        else if (isLow_[j]) push({j, series.low[j], false});

        if (pos.inPosition) {
            if (std::isfinite(target_) && series.close[i] >= target_) return Signal::Sell;
            if (std::isfinite(stop_) && series.close[i] <= stop_) return Signal::Sell;
            if (params_.exitOnBearCandle && (cand_[i] & indicators::candle::kBearish))
                return Signal::Sell;
            return Signal::Hold;
        }

        if (!std::isfinite(atr_[i]) || atr_[i] <= 0.0) return Signal::Hold;

        double breakLevel = 0.0, patLow = 0.0, height = 0.0;
        const char* which = nullptr;
        bool reversal = false;   // harmonics are BID INTO, not broken out of
        if (params_.useDoubleBottom && matchDoubleBottom(i, breakLevel, patLow, height)) which = "db";
        else if (params_.useInverseHS && matchInverseHS(i, breakLevel, patLow, height)) which = "ihs";
        else if (params_.useCupHandle && matchCupHandle(series, i, breakLevel, patLow, height)) which = "cup";
        else if (params_.useBullFlag && matchBullFlag(series, i, breakLevel, patLow, height)) which = "flag";
        else if (params_.useAscTriangle && matchAscTriangle(i, breakLevel, patLow, height)) which = "asc";
        else if (params_.useSymTriangle && matchSymTriangle(i, breakLevel, patLow, height)) which = "sym";
        else if (params_.useButterfly && matchButterfly(series, i, breakLevel, patLow, height))
            { which = "butterfly"; reversal = true; }
        else if (params_.useCypher && matchCypher(series, i, breakLevel, patLow, height))
            { which = "cypher"; reversal = true; }
        if (!which) return Signal::Hold;

        if (height < params_.minPatternAtr * atr_[i]) return Signal::Hold;
        // Breakout patterns need a close ABOVE the level; harmonic reversals
        // need price to trade DOWN INTO the D zone, which is what a resting bid
        // there actually does.
        if (reversal) {
            if (!(series.low[i] <= breakLevel && series.close[i] > patLow)) return Signal::Hold;
        } else if (!(series.close[i] > breakLevel)) {
            return Signal::Hold;
        }
        if (params_.requireCandle && !(cand_[i] & indicators::candle::kBullish)) return Signal::Hold;
        if (params_.requireVolume) {
            if (relVol_.empty() || !std::isfinite(relVol_[i]) || relVol_[i] < params_.volMult)
                return Signal::Hold;
        }

        target_ = breakLevel + params_.targetHeightMult * height;
        stop_ = patLow * (1.0 - params_.stopBelowLowFrac);
        if (!std::strcmp(which, "db")) ++counts_.doubleBottom;
        else if (!std::strcmp(which, "ihs")) ++counts_.inverseHS;
        else if (!std::strcmp(which, "cup")) ++counts_.cupHandle;
        else if (!std::strcmp(which, "flag")) ++counts_.bullFlag;
        else if (!std::strcmp(which, "asc")) ++counts_.ascTriangle;
        else if (!std::strcmp(which, "sym")) ++counts_.symTriangle;
        else if (!std::strcmp(which, "butterfly")) ++counts_.butterfly;
        else ++counts_.cypher;
        return Signal::Buy;
    }

private:
    struct Piv { size_t idx; double px; bool high; };

    void push(const Piv& p) {
        if (!pivots_.empty() && pivots_.back().high == p.high) {
            // Same kind twice: keep the more extreme, which is the real swing.
            if ((p.high && p.px > pivots_.back().px) || (!p.high && p.px < pivots_.back().px))
                pivots_.back() = p;
            return;
        }
        pivots_.push_back(p);
        while (pivots_.size() > 8) pivots_.pop_front();
    }

    bool fresh(size_t i, size_t idx) const {
        return i >= idx && (i - idx) <= static_cast<size_t>(params_.maxPatternBars);
    }

    // low, high, low with the two lows at a similar level; break the high.
    bool matchDoubleBottom(size_t i, double& brk, double& low, double& h) const {
        if (pivots_.size() < 3) return false;
        const Piv& c = pivots_[pivots_.size() - 1];
        const Piv& b = pivots_[pivots_.size() - 2];
        const Piv& a = pivots_[pivots_.size() - 3];
        if (!(!a.high && b.high && !c.high)) return false;
        if (!fresh(i, a.idx)) return false;
        h = b.px - std::min(a.px, c.px);
        if (!(h > 0.0)) return false;
        if (std::fabs(a.px - c.px) > params_.levelTolFrac * h) return false;
        brk = b.px;
        low = std::min(a.px, c.px);
        return true;
    }

    // low(LS), high, low(head, lower), high, low(RS) with LS ~ RS and the head
    // clearly below both; neckline is the higher of the two intervening highs.
    bool matchInverseHS(size_t i, double& brk, double& low, double& h) const {
        if (pivots_.size() < 5) return false;
        const Piv& rs = pivots_[pivots_.size() - 1];
        const Piv& h2 = pivots_[pivots_.size() - 2];
        const Piv& hd = pivots_[pivots_.size() - 3];
        const Piv& h1 = pivots_[pivots_.size() - 4];
        const Piv& ls = pivots_[pivots_.size() - 5];
        if (!(!ls.high && h1.high && !hd.high && h2.high && !rs.high)) return false;
        if (!fresh(i, ls.idx)) return false;
        double shoulder = std::min(ls.px, rs.px);
        if (!(hd.px < shoulder * (1.0 - params_.minHeadDropFrac))) return false;
        double neck = std::max(h1.px, h2.px);
        h = neck - hd.px;
        if (!(h > 0.0)) return false;
        if (std::fabs(ls.px - rs.px) > params_.levelTolFrac * h) return false;
        brk = neck;
        low = hd.px;
        return true;
    }

    // high(left rim), low(cup), high(right rim ~ left), then a shallow pullback
    // (the handle) and a break of the rim.
    bool matchCupHandle(const CandleSeries& s, size_t i, double& brk, double& low, double& h) const {
        if (pivots_.size() < 3) return false;
        const Piv& r2 = pivots_[pivots_.size() - 1];
        const Piv& cup = pivots_[pivots_.size() - 2];
        const Piv& r1 = pivots_[pivots_.size() - 3];
        if (!(r1.high && !cup.high && r2.high)) return false;
        if (!fresh(i, r1.idx)) return false;
        h = std::min(r1.px, r2.px) - cup.px;
        if (!(h > 0.0)) return false;
        if (std::fabs(r1.px - r2.px) > params_.levelTolFrac * h) return false;
        // The handle: since the right rim, price must have pulled back but by
        // less than handleMaxFrac of the cup depth.
        double lowSince = s.low[r2.idx];
        for (size_t t = r2.idx; t < i; ++t) lowSince = std::min(lowSince, s.low[t]);
        double pull = r2.px - lowSince;
        if (!(pull > 0.0) || pull > params_.handleMaxFrac * h) return false;
        brk = std::max(r1.px, r2.px);
        low = lowSince;
        return true;
    }

    // Sharp impulse (low -> high), then a tight consolidation, then a break of
    // the consolidation high. The flagpole supplies the measured move.
    bool matchBullFlag(const CandleSeries& s, size_t i, double& brk, double& low, double& h) const {
        if (pivots_.size() < 2) return false;
        const Piv& top = pivots_[pivots_.size() - 1];
        const Piv& bot = pivots_[pivots_.size() - 2];
        if (!(top.high && !bot.high)) return false;
        if (!fresh(i, bot.idx)) return false;
        h = top.px - bot.px;
        if (!(h > 0.0)) return false;
        size_t age = i - top.idx;
        if (age < 2 || age > static_cast<size_t>(params_.flagMaxBars)) return false;
        // The consolidation is measured over [top.idx, i-1], EXCLUDING the
        // decision bar. Including bar i made this pattern unfireable: the
        // caller requires close[i] > brk, and with brk = max(high[top..i]) that
        // asks close[i] to exceed a maximum that already contains high[i],
        // which is impossible. bull-flag reported zero trades on every
        // instrument tested - a rule that can never fire, presented as a flat
        // result.
        if (i == 0) return false;
        double hi = s.high[top.idx], lo = s.low[top.idx];
        for (size_t t = top.idx; t < i; ++t) { hi = std::max(hi, s.high[t]); lo = std::min(lo, s.low[t]); }
        if ((hi - lo) > params_.flagMaxWidthFrac * h) return false;   // must be TIGHT
        brk = hi;
        low = lo;
        return true;
    }


    // |actual/expected - 1| within tolerance, guarding a zero denominator.
    bool ratioNear(double actual, double expected) const {
        if (!(expected > 0.0) || !std::isfinite(actual)) return false;
        return std::fabs(actual / expected - 1.0) <= params_.harmonicTol;
    }
    bool ratioIn(double actual, double lo, double hi) const {
        return std::isfinite(actual) && actual >= lo * (1.0 - params_.harmonicTol)
                                     && actual <= hi * (1.0 + params_.harmonicTol);
    }

    // ASCENDING TRIANGLE: two roughly equal highs (horizontal resistance) with
    // rising lows beneath. Bullish; entry is a break of the resistance.
    bool matchAscTriangle(size_t i, double& brk, double& low, double& h) const {
        if (pivots_.size() < 4) return false;
        const Piv& p4 = pivots_[pivots_.size() - 1];
        const Piv& p3 = pivots_[pivots_.size() - 2];
        const Piv& p2 = pivots_[pivots_.size() - 3];
        const Piv& p1 = pivots_[pivots_.size() - 4];
        // need ... high, low, high, low  or  low, high, low, high
        const Piv *hA, *hB, *lA, *lB;
        if (p1.high && !p2.high && p3.high && !p4.high) { hA=&p1; lA=&p2; hB=&p3; lB=&p4; }
        else if (!p1.high && p2.high && !p3.high && p4.high) { lA=&p1; hA=&p2; lB=&p3; hB=&p4; }
        else return false;
        if (!fresh(i, p1.idx)) return false;
        h = std::max(hA->px, hB->px) - std::min(lA->px, lB->px);
        if (!(h > 0.0)) return false;
        if (std::fabs(hA->px - hB->px) > params_.levelTolFrac * h) return false;  // flat top
        if (!(lB->px > lA->px)) return false;                                     // rising lows
        brk = std::max(hA->px, hB->px);
        low = lB->px;
        return true;
    }

    // SYMMETRICAL TRIANGLE: highs falling AND lows rising - the range
    // converges. Direction-agnostic in theory; long-only here, so an upside
    // break is the trade.
    bool matchSymTriangle(size_t i, double& brk, double& low, double& h) const {
        if (pivots_.size() < 4) return false;
        const Piv& p4 = pivots_[pivots_.size() - 1];
        const Piv& p3 = pivots_[pivots_.size() - 2];
        const Piv& p2 = pivots_[pivots_.size() - 3];
        const Piv& p1 = pivots_[pivots_.size() - 4];
        const Piv *hA, *hB, *lA, *lB;
        if (p1.high && !p2.high && p3.high && !p4.high) { hA=&p1; lA=&p2; hB=&p3; lB=&p4; }
        else if (!p1.high && p2.high && !p3.high && p4.high) { lA=&p1; hA=&p2; lB=&p3; hB=&p4; }
        else return false;
        if (!fresh(i, p1.idx)) return false;
        h = std::max(hA->px, hB->px) - std::min(lA->px, lB->px);
        if (!(h > 0.0)) return false;
        if (!(hB->px < hA->px)) return false;   // falling highs
        if (!(lB->px > lA->px)) return false;   // rising lows
        brk = hB->px;
        low = lB->px;
        return true;
    }

    // BULLISH BUTTERFLY (harmonic). Points X(high) A(low) B(high) C(low), with
    // D projected BELOW X - the defining feature. Textbook ratios:
    //   AB = 0.786 of XA,  BC in [0.382, 0.886] of AB,
    //   D such that AD in [1.27, 1.618] of XA.
    // Entry is a bid at D, i.e. price trading DOWN into the zone.
    bool matchButterfly(const CandleSeries& s, size_t i, double& entry, double& low,
                         double& h) const {
        if (pivots_.size() < 4) return false;
        const Piv& C = pivots_[pivots_.size() - 1];
        const Piv& B = pivots_[pivots_.size() - 2];
        const Piv& A = pivots_[pivots_.size() - 3];
        const Piv& X = pivots_[pivots_.size() - 4];
        if (!(X.high && !A.high && B.high && !C.high)) return false;
        if (!fresh(i, X.idx)) return false;
        double XA = X.px - A.px;
        if (!(XA > 0.0)) return false;
        if (!ratioNear((B.px - A.px) / XA, 0.786)) return false;
        double AB = B.px - A.px;
        if (!(AB > 0.0)) return false;
        if (!ratioIn((B.px - C.px) / AB, 0.382, 0.886)) return false;
        double D = X.px - 1.27 * XA;          // shallowest of the 1.27-1.618 band
        if (!(D > 0.0) || D >= C.px) return false;   // D must be a NEW low
        entry = D;
        low = X.px - 1.618 * XA;              // stop beyond the far edge
        h = C.px - D;
        return h > 0.0;
    }

    // BULLISH CYPHER (harmonic). X(high) A(low) B(high) C(low) with C BELOW A -
    // the feature that separates Cypher from Gartley/Butterfly - and D at the
    // 0.786 retracement of the whole XC leg, so entry is ABOVE C.
    //   AB in [0.382, 0.618] of XA,  BC in [1.13, 1.414] of AB,  D = 0.786 XC.
    bool matchCypher(const CandleSeries& s, size_t i, double& entry, double& low,
                      double& h) const {
        if (pivots_.size() < 4) return false;
        const Piv& C = pivots_[pivots_.size() - 1];
        const Piv& B = pivots_[pivots_.size() - 2];
        const Piv& A = pivots_[pivots_.size() - 3];
        const Piv& X = pivots_[pivots_.size() - 4];
        if (!(X.high && !A.high && B.high && !C.high)) return false;
        if (!fresh(i, X.idx)) return false;
        double XA = X.px - A.px;
        if (!(XA > 0.0)) return false;
        if (!ratioIn((B.px - A.px) / XA, 0.382, 0.618)) return false;
        double AB = B.px - A.px;
        if (!(AB > 0.0)) return false;
        if (!ratioIn((B.px - C.px) / AB, 1.13, 1.414)) return false;
        if (!(C.px < A.px)) return false;             // C undercuts A
        double XC = X.px - C.px;
        if (!(XC > 0.0)) return false;
        entry = C.px + 0.786 * XC;                    // retrace UP into D
        if (!(entry < X.px)) return false;
        low = C.px;
        h = X.px - entry;
        return h > 0.0;
    }

    Params params_;
    std::vector<double> atr_, relVol_;
    std::vector<unsigned> cand_;
    std::vector<bool> isHigh_, isLow_;
    std::deque<Piv> pivots_;
    Counts counts_;
    double target_ = 0.0, stop_ = 0.0;
};

} // namespace trader
