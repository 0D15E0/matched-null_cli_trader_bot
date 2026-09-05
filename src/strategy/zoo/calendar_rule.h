#pragma once

#include "../strategy.h"
#include <ctime>
#include <string>

namespace trader {

// A deliberately simple control family for calendar hypotheses. Weekdays use
// UTC tm_wday numbering: Sunday=0, Monday=1, ..., Saturday=6. The rule is
// long/flat and acts on the candle timestamp; the engine still applies its
// normal next-open fill, fees, and slippage.
struct CalendarRuleParams {
    int buyWeekday = 3;  // Wednesday
    int sellWeekday = 1; // Monday
};

class CalendarRuleStrategy : public Strategy {
public:
    explicit CalendarRuleStrategy(CalendarRuleParams p = {}) : p_(p) {}

    std::string name() const override { return "calendar_rule"; }

    void prepare(const CandleSeries&) override {}

    Signal onBar(const CandleSeries& series, size_t i, const PositionContext& position) override {
        if (i >= series.timestamp.size()) return Signal::Hold;
        int weekday = utcWeekday(series.timestamp[i]);
        if (position.inPosition && weekday == p_.sellWeekday) return Signal::Sell;
        if (!position.inPosition && weekday == p_.buyWeekday) return Signal::Buy;
        return Signal::Hold;
    }

private:
    static int utcWeekday(int64_t timestamp) {
        std::time_t raw = static_cast<std::time_t>(timestamp);
        std::tm utc{};
#if defined(_WIN32)
        gmtime_s(&utc, &raw);
#else
        gmtime_r(&raw, &utc);
#endif
        return utc.tm_wday;
    }

    CalendarRuleParams p_;
};

} // namespace trader