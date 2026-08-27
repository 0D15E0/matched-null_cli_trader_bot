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

// The fully general 3-member vote: entry and stay conditions are arbitrary
// monotone Boolean functions of WHICH members want to be long, encoded as
// 8-bit truth tables over the vote vector
//     v = tsmom*1 + faber*2 + donchian*4          (0..7)
// entryMask bit v set  -> open a long when the votes are exactly v and flat
// stayMask  bit v set  -> keep the long while the votes are v; else sell
// Consistency (entry implies stay) is the caller's job; the registry sweep
// enumerates only consistent pairs.
//
// Defaults reproduce the live ensemble_vote(enter>=2, exit<=1) rule bit for
// bit: mask 232 = 0b11101000 = the four vote vectors with >=2 bits set.
//
// WHY THIS EXISTS, AND THE WARNING THAT COMES WITH IT. ensemble_vote only
// counts votes. This lets a sweep ask "does it matter WHICH member" - e.g.
// "tsmom alone is enough to buy, but faber alone is not". There are 18
// non-constant monotone rules and 129 consistent (entry, stay) pairs. The
// three members are statistically indistinguishable on dev data (Sharpe
// 1.64-1.66), so a rule that privileges one of them is, by prior, fitting
// the noise between them. Read any sweep over this family as a distribution
// with a best-of-129 selection effect, never as the best cell.
struct EnsembleRuleParams {
    int entryMask = 232;
    int stayMask = 232;
};

class EnsembleRuleStrategy : public Strategy {
public:
    explicit EnsembleRuleStrategy(EnsembleRuleParams p = {}) : p_(p) {
        members_.push_back(std::make_unique<TsmomStrategy>());      // bit 0
        members_.push_back(std::make_unique<FaberMaStrategy>());    // bit 1
        members_.push_back(std::make_unique<DonchianStrategy>());   // bit 2
    }
    std::string name() const override { return "ensemble_rule"; }

    void prepare(const CandleSeries& s) override {
        size_t n = s.size();
        votes_.assign(n, 0);
        for (size_t m = 0; m < members_.size(); ++m) {
            members_[m]->prepare(s);
            PositionContext vpos;
            for (size_t i = 0; i < n; ++i) {
                vpos.observe(s.close[i]);
                Signal sig = members_[m]->onBar(s, i, vpos);
                if (sig == Signal::Buy && !vpos.inPosition)
                    vpos.open(s.close[i], s.timestamp[i], i);
                else if (sig == Signal::Sell && vpos.inPosition)
                    vpos.close();
                if (vpos.inPosition) votes_[i] |= static_cast<unsigned char>(1u << m);
            }
        }
    }

    Signal onBar(const CandleSeries&, size_t i, const PositionContext& pos) override {
        if (i >= votes_.size()) return Signal::Hold;
        unsigned v = votes_[i];
        bool entry = (static_cast<unsigned>(p_.entryMask) >> v) & 1u;
        bool stay  = (static_cast<unsigned>(p_.stayMask)  >> v) & 1u;
        if (!pos.inPosition && entry) return Signal::Buy;
        if (pos.inPosition && !stay) return Signal::Sell;
        return Signal::Hold;
    }

private:
    EnsembleRuleParams p_;
    std::vector<std::unique_ptr<Strategy>> members_;
    std::vector<unsigned char> votes_;
};

} // namespace trader
