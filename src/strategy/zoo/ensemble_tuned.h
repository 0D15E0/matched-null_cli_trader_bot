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

// ensemble_vote with the MEMBERS' parameters exposed, for sensitivity studies.
// Defaults reproduce the live book exactly (verified against ensemble_vote
// enterVotes=2 exitVotes=0). Everything else about the mechanics is identical
// to EnsembleVoteStrategy: each member is replayed with a virtual position to
// produce a per-bar "wants long" vote; long while >= enterVotes agree, flat
// once <= exitVotes remain.
//
// The 4h bar is the unit, so the day-equivalents people reason in are:
//   faberWindow 200 = 33 days, donEntry 55 = 9 days, donExit 20 = 3.3 days,
//   tsLookback 90 = 15 days.
struct EnsembleTunedParams {
    int    enterVotes = 2;
    int    exitVotes = 0;
    int    faberWindow = 200;
    int    donEntry = 55;
    int    donExit = 20;
    double donAtrMult = 2.5;
    int    tsLookback = 90;
    double tsThreshold = 0.05;      // absolute, as TsmomParams' default
};

class EnsembleTunedStrategy : public Strategy {
public:
    explicit EnsembleTunedStrategy(EnsembleTunedParams p = {}) : p_(p) {
        TsmomParams t; t.lookbackWindow = p_.tsLookback; t.entryThreshold = p_.tsThreshold;
        FaberParams f; f.window = p_.faberWindow;
        DonchianParams d; d.entryWindow = p_.donEntry; d.exitWindow = p_.donExit; d.atrStopMult = p_.donAtrMult;
        members_.push_back(std::make_unique<TsmomStrategy>(t));
        members_.push_back(std::make_unique<FaberMaStrategy>(f));
        members_.push_back(std::make_unique<DonchianStrategy>(d));
        if (p_.exitVotes >= p_.enterVotes) p_.exitVotes = p_.enterVotes - 1;
    }
    std::string name() const override { return "ensemble_tuned"; }

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
    Signal onBar(const CandleSeries&, size_t i, const PositionContext& pos) override {
        if (i >= votes_.size()) return Signal::Hold;
        int v = votes_[i];
        if (!pos.inPosition && v >= p_.enterVotes) return Signal::Buy;
        if (pos.inPosition && v <= p_.exitVotes) return Signal::Sell;
        return Signal::Hold;
    }
private:
    EnsembleTunedParams p_;
    std::vector<std::unique_ptr<Strategy>> members_;
    std::vector<int> votes_;
};

} // namespace trader
