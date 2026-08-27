#pragma once
#include "../strategy.h"
#include "regime.h"
#include "zoo_common.h"
#include <memory>
#include <string>
#include <vector>

namespace trader {

// Regime gates: decorators that suppress a wrapped strategy's ENTRIES when a
// measured property of the series says the strategy's hypothesis is not
// currently supported. Exits are never suppressed, matching the convention
// HurstRegimeFilter established - a filter that can trap you in a position is
// not a risk control, it is a new risk.
//
// These exist as separate objects, rather than as parameters of each
// strategy, because the tournament treats "which gate, if any" as a
// STRUCTURAL gene it can mutate. A survivor can therefore acquire a regime
// filter it was not born with, which is the cross-family adaptation the
// per-family parameter search cannot reach.
//
// A gate is a strong claim and usually a losing one: every gate reduces the
// number of trades, and fewer trades means a noisier estimate of everything.
// The tournament is set up so a gate has to earn its place by improving the
// worst environment, not merely the average one.

// Bandt-Pompe permutation entropy gate. Entries only when the recent ordinal
// dynamics are more structured (lower entropy) than `maxEntropy`.
class PermutationEntropyGate : public Strategy {
public:
    PermutationEntropyGate(std::unique_ptr<Strategy> inner, int order, int window,
                            double maxEntropy)
        : inner_(std::move(inner)), order_(order), window_(window), maxEntropy_(maxEntropy) {}

    std::string name() const override { return inner_->name() + "+pe_gate"; }

    void prepare(const CandleSeries& s) override {
        inner_->prepare(s);
        pe_ = zoo::rollingPermutationEntropy(s.close, order_, window_);
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& pos) override {
        Signal sig = inner_->onBar(s, i, pos);
        if (sig != Signal::Buy) return sig;
        if (i >= pe_.size() || !zoo::ok(pe_[i])) return sig;  // unmeasured: do not block
        return pe_[i] <= maxEntropy_ ? Signal::Buy : Signal::Hold;
    }

private:
    std::unique_ptr<Strategy> inner_;
    int order_, window_;
    double maxEntropy_;
    std::vector<double> pe_;
};

// Hawkes self-excitation gate. Entries only when the branching intensity of
// large moves sits in the lower part of its own trailing distribution - i.e.
// not in the middle of a shock cluster.
class HawkesIntensityGate : public Strategy {
public:
    HawkesIntensityGate(std::unique_ptr<Strategy> inner, int volWindow, double jumpSigmas,
                         double decayBars, int rankWindow, double maxRank)
        : inner_(std::move(inner)), volWindow_(volWindow), jumpSigmas_(jumpSigmas),
          decayBars_(decayBars), rankWindow_(rankWindow), maxRank_(maxRank) {}

    std::string name() const override { return inner_->name() + "+hawkes_gate"; }

    void prepare(const CandleSeries& s) override {
        inner_->prepare(s);
        auto lambda = zoo::hawkesIntensity(s.close, volWindow_, jumpSigmas_, decayBars_);
        rank_ = zoo::rollingPercentileRank(lambda, rankWindow_);
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& pos) override {
        Signal sig = inner_->onBar(s, i, pos);
        if (sig != Signal::Buy) return sig;
        if (i >= rank_.size() || !zoo::ok(rank_[i])) return sig;
        return rank_[i] <= maxRank_ ? Signal::Buy : Signal::Hold;
    }

private:
    std::unique_ptr<Strategy> inner_;
    int volWindow_;
    double jumpSigmas_, decayBars_;
    int rankWindow_;
    double maxRank_;
    std::vector<double> rank_;
};

// Lo-MacKinlay variance-ratio gate. Entries only while q-bar moves are larger
// than a random walk would produce, i.e. while the series is actually
// persistent. This is the same question the Hurst gate asks, measured by a
// test with a known null distribution rather than by an exponent estimate,
// which is why both are offered and the search is allowed to pick.
class VarianceRatioGate : public Strategy {
public:
    VarianceRatioGate(std::unique_ptr<Strategy> inner, int window, int lag, double minVr)
        : inner_(std::move(inner)), window_(window), lag_(lag), minVr_(minVr) {}

    std::string name() const override { return inner_->name() + "+vr_gate"; }

    void prepare(const CandleSeries& s) override {
        inner_->prepare(s);
        vr_ = zoo::rollingVarianceRatio(s.close, window_, lag_);
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& pos) override {
        Signal sig = inner_->onBar(s, i, pos);
        if (sig != Signal::Buy) return sig;
        if (i >= vr_.size() || !zoo::ok(vr_[i])) return sig;
        return vr_[i] >= minVr_ ? Signal::Buy : Signal::Hold;
    }

private:
    std::unique_ptr<Strategy> inner_;
    int window_, lag_;
    double minVr_;
    std::vector<double> vr_;
};

} // namespace trader
