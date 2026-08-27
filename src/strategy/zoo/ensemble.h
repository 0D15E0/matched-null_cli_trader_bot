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

// Majority vote over three trend-following mechanisms.
//
// WHY THIS EXISTS. Measured on the (spent) 2024+ window, the same portfolio
// machinery run under different timing rules at their published defaults
// scattered from Sharpe -0.68 (vol_managed) to +0.97 (faber_ma), with the
// pre-registered rule (tsmom, 0.72) in the middle of the trend cluster. None
// of that ordering is stable: it moved visibly when four days of bars were
// appended, and twelve tournaments found the in-sample ranking of the good
// rules carries no out-of-sample information (rho over the better half:
// never significant in 12 runs). So WHICH trend rule to run is a
// high-variance choice that the data cannot make.
//
// The response is the same one the portfolio applies to assets: when a choice
// is noise, do not make it - diversify across it. The members are three
// mechanisms that read a trend through genuinely different statistics:
//
//   tsmom     - the level of the trailing 90-bar return (Moskowitz/Ooi/Pedersen)
//   faber_ma  - price against its own long moving average (Faber 2007)
//   donchian  - breakout of the prior N-bar extreme, ATR trailing stop (Turtle)
//
// all at their published defaults. Averaging rules whose errors are imperfectly
// correlated reduces variance by the identical arithmetic that made the
// multi-sleeve portfolio the one reproducible result in this repo - it is a
// theorem doing the work, not a forecast.
//
// MECHANICS. prepare() replays each member over the whole series with its own
// VIRTUAL position (opened/closed at bar closes), producing a per-bar boolean
// "this member wants to be long". That series is a pure, causal, idempotent
// function of the candles - member m's vote at bar i reads only bars <= i - so
// onBar() is O(1), never mutates state, and satisfies the replay/live contract
// (causality_check verifies this like any other family). The virtual fills use
// the decision bar's close where the engine fills at the next open; the vote is
// deliberately a signal-level object, not a fill-level one, and the engine
// applies its own honest fill timing to the ENSEMBLE's signal.
//
// The vote uses hysteresis: enter when >= enterVotes members are long, exit
// when <= exitVotes remain. At the defaults (2/1) the position is held while a
// majority agrees and closed once agreement has genuinely collapsed, so a
// single member flickering at its own threshold cannot churn the book.
struct EnsembleParams {
    int enterVotes = 2;  // members that must be long to open
    int exitVotes = 1;   // close when this many or fewer remain long
};

class EnsembleVoteStrategy : public Strategy {
public:
    explicit EnsembleVoteStrategy(EnsembleParams p = {}) : p_(p) {
        members_.push_back(std::make_unique<TsmomStrategy>());
        members_.push_back(std::make_unique<FaberMaStrategy>());
        members_.push_back(std::make_unique<DonchianStrategy>());
        if (p_.exitVotes >= p_.enterVotes) p_.exitVotes = p_.enterVotes - 1;
    }

    std::string name() const override { return "ensemble_vote"; }

    void prepare(const CandleSeries& s) override {
        size_t n = s.size();
        votes_.assign(n, 0);
        for (auto& m : members_) {
            m->prepare(s);
            PositionContext vpos;
            for (size_t i = 0; i < n; ++i) {
                vpos.observe(s.close[i]);
                Signal sig = m->onBar(s, i, vpos);
                if (sig == Signal::Buy && !vpos.inPosition)
                    vpos.open(s.close[i], s.timestamp[i], i);
                else if (sig == Signal::Sell && vpos.inPosition)
                    vpos.close();
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
    EnsembleParams p_;
    std::vector<std::unique_ptr<Strategy>> members_;
    std::vector<int> votes_;
};

} // namespace trader
