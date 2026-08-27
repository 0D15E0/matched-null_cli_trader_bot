#pragma once
#include "../strategy.h"
#include "ensemble.h"
#include "zoo_common.h"
#include <memory>
#include <string>
#include <vector>

namespace trader {

// Long/short extension of the live book's ensemble vote, for the engine's
// --long-short mode.
//
// The long side is the production rule unchanged: each of tsmom / faber_ma /
// donchian is replayed with a virtual position and votes "I would be long
// here". The SHORT side needed member definitions of "I would be short", and
// rather than hand-writing a mirrored variant of every member - three new
// state machines to review and keep in sync - the members are replayed
// unchanged on the RECIPROCAL series:
//
//     inverted bar:  open' = 1/open,  high' = 1/low,  low' = 1/high,
//                    close' = 1/close  (volume and timestamps unchanged)
//
// A downtrend in price is exactly an uptrend in 1/price: tsmom's trailing
// return flips sign, faber's price-below-average becomes price-above-average,
// and a Donchian breakdown through the 55-bar low becomes a breakout through
// the 55-bar high, ATR stop included. "Member m would be long on 1/p" IS
// "member m would be short on p", with zero new rule code.
//
// Signals, under the engine's long/short vocabulary (Buy = target long,
// Sell = target short, Hold = keep):
//     longVotes >= enterVotes and shortVotes < enterVotes  -> Buy
//     shortVotes >= enterVotes and longVotes < enterVotes  -> Sell
//     anything else                                        -> Hold
//
// Note what Hold means there: after a side's majority collapses the position
// is KEPT until the opposite majority forms. The long/flat ensemble exits to
// cash at <=1 votes; this one cannot express cash (three signals, no fourth),
// so it is always-in after its first signal - the academic TSMOM-LS
// convention. That difference is a real behavioural change and is why this is
// a separate registry family rather than a flag on ensemble_vote.
struct EnsembleLsParams {
    int enterVotes = 2;   // members that must agree to take/flip a side
};

class EnsembleLongShortStrategy : public Strategy {
public:
    explicit EnsembleLongShortStrategy(EnsembleLsParams p = {}) : p_(p) {}
    std::string name() const override { return "ensemble_ls"; }

    void prepare(const CandleSeries& s) override {
        longVotes_ = votesOn(s);
        CandleSeries inv;
        inv.symbol = s.symbol; inv.periodSeconds = s.periodSeconds;
        inv.timestamp = s.timestamp;
        inv.volume = s.volume;
        size_t n = s.size();
        inv.open.resize(n); inv.high.resize(n); inv.low.resize(n); inv.close.resize(n);
        for (size_t i = 0; i < n; ++i) {
            // high' = 1/low and low' = 1/high: reciprocation reverses order.
            inv.open[i]  = s.open[i]  > 0 ? 1.0 / s.open[i]  : 0.0;
            inv.high[i]  = s.low[i]   > 0 ? 1.0 / s.low[i]   : 0.0;
            inv.low[i]   = s.high[i]  > 0 ? 1.0 / s.high[i]  : 0.0;
            inv.close[i] = s.close[i] > 0 ? 1.0 / s.close[i] : 0.0;
        }
        shortVotes_ = votesOn(inv);
    }

    Signal onBar(const CandleSeries&, size_t i, const PositionContext&) override {
        if (i >= longVotes_.size()) return Signal::Hold;
        bool wantLong = longVotes_[i] >= p_.enterVotes;
        bool wantShort = shortVotes_[i] >= p_.enterVotes;
        if (wantLong && !wantShort) return Signal::Buy;
        if (wantShort && !wantLong) return Signal::Sell;
        return Signal::Hold;   // no majority, or (rare) contradictory majorities
    }

private:
    // Identical construction to EnsembleVoteStrategy::prepare: fresh member
    // instances, virtual close-fill replay, per-bar count of members long.
    static std::vector<int> votesOn(const CandleSeries& s) {
        std::vector<std::unique_ptr<Strategy>> members;
        members.push_back(std::make_unique<TsmomStrategy>());
        members.push_back(std::make_unique<FaberMaStrategy>());
        members.push_back(std::make_unique<DonchianStrategy>());
        std::vector<int> votes(s.size(), 0);
        for (auto& m : members) {
            m->prepare(s);
            PositionContext vpos;
            for (size_t i = 0; i < s.size(); ++i) {
                vpos.observe(s.close[i]);
                Signal sig = m->onBar(s, i, vpos);
                if (sig == Signal::Buy && !vpos.inPosition)
                    vpos.open(s.close[i], s.timestamp[i], i);
                else if (sig == Signal::Sell && vpos.inPosition)
                    vpos.close();
                if (vpos.inPosition) ++votes[i];
            }
        }
        return votes;
    }

    EnsembleLsParams p_;
    std::vector<int> longVotes_, shortVotes_;
};

} // namespace trader
