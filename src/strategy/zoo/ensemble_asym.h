#pragma once
#include "../strategy.h"
#include "../tsmom_strategy.h"
#include "breakout.h"
#include "trend.h"
#include "zoo_common.h"
#include <memory>
#include <string>
#include <vector>

namespace trader {

// ASYMMETRIC EXIT: the vote threshold for leaving depends on whether the
// position is working.
//
// The live rule (enter >=2, exit <=0) holds until every member has quit. That
// is what cut its turnover by two thirds, and it is also what deepened its
// per-sleeve drawdowns - a losing position gets the same patience as a
// winning one, so the book rides losers all the way to unanimous surrender.
//
// This asks whether patience should be earned. A position in profit keeps the
// loose exit (<= exitWinner, default 0); a position under water gets a
// tighter one (<= exitLoser, default 1), i.e. it leaves as soon as two of the
// three members have given up.
//
// Why this is not the bracket that died in addendum 11: that rule was a fixed
// +3%/-1.5% pair, and it failed because it CLIPPED WINNERS - trend following
// earns from the few trades that run far, so any ceiling on them removes the
// trades that pay for everything else. This is the mirror image. Winners are
// never touched; the only behaviour that changes is how long a losing
// position is given to recover. It is also not one of the eleven dead ENTRY
// filters: entries are untouched, and exposure is reduced only on positions
// that are already losing.
//
// P&L is measured against the position's entry FILL price (PositionContext::
// entryPrice), so it is the trade's real profit, not a signal-price proxy.
// The comparison uses the bar's close, the same information the vote uses.
struct EnsembleAsymParams {
    int enterVotes = 2;
    int exitWinner = 0;      // vote threshold to exit while in profit
    int exitLoser = 1;       // vote threshold to exit while under water
    double lossThreshold = 0.0;  // "losing" means return below this (fraction)
};

class EnsembleAsymStrategy : public Strategy {
public:
    explicit EnsembleAsymStrategy(EnsembleAsymParams p = {}) : p_(p) {
        members_.push_back(std::make_unique<TsmomStrategy>());
        members_.push_back(std::make_unique<FaberMaStrategy>());
        members_.push_back(std::make_unique<DonchianStrategy>());
    }
    std::string name() const override { return "ensemble_asym"; }

    void prepare(const CandleSeries& s) override {
        size_t n = s.size();
        votes_.assign(n, 0);
        for (auto& m : members_) {
            m->prepare(s);
            PositionContext vpos;
            for (size_t i = 0; i < n; ++i) {
                vpos.observe(s.close[i]);
                Signal sig = m->onBar(s, i, vpos);
                if (sig == Signal::Buy && !vpos.inPosition) vpos.open(s.close[i], s.timestamp[i], i);
                else if (sig == Signal::Sell && vpos.inPosition) vpos.close();
                if (vpos.inPosition) ++votes_[i];
            }
        }
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& pos) override {
        if (i >= votes_.size()) return Signal::Hold;
        const int v = votes_[i];
        if (!pos.inPosition) return v >= p_.enterVotes ? Signal::Buy : Signal::Hold;
        // In position: which leash applies depends on the trade's own P&L.
        const bool winning = pos.entryPrice > 0.0 &&
                             (s.close[i] / pos.entryPrice - 1.0) > p_.lossThreshold;
        const int threshold = winning ? p_.exitWinner : p_.exitLoser;
        return v <= threshold ? Signal::Sell : Signal::Hold;
    }

private:
    EnsembleAsymParams p_;
    std::vector<std::unique_ptr<Strategy>> members_;
    std::vector<int> votes_;
};

} // namespace trader
