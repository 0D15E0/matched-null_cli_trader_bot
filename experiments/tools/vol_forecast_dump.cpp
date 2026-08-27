// Dump, for every bar of a store: the CAUSAL volatility forecast for the next
// H bars, and the volatility that was actually realized over those H bars.
//
// This exists so the "does our forecast beat implied volatility?" test scores
// the forecaster the repo actually ships (indicators/vol_forecast.h) rather
// than a reimplementation of it. forecastVolatility() guarantees out[i] uses
// only close[0..i]; that property was verified with 6,624 truncation checks
// plus a deliberately planted look-ahead that the test caught.
//
// Both columns are ANNUALIZED using the series' MEASURED bars-per-year, so they
// are directly comparable to a quoted implied vol like Deribit's DVOL.
//
// The realized column is a FORWARD window and is therefore NOT causal by
// construction - it is the regression's dependent variable, i.e. the thing
// being predicted. The last H bars have no realized value and are emitted as
// empty.
//
//   usage: vol_forecast_dump <store.ctc> <horizonBars> [model]
//          model = trailing | har | fractional   (default fractional)
//   output: CSV  timestamp,forecast_annual,realized_annual
#include "core/candle_store.h"
#include "indicators/vol_forecast.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace trader;

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: vol_forecast_dump <store.ctc> <horizonBars> [trailing|har|fractional]\n");
        return 1;
    }
    CandleStore store(argv[1]);
    if (!store.isValidStore()) { fprintf(stderr, "invalid store: %s\n", argv[1]); return 1; }
    CandleSeries s = store.load();
    if (s.size() < 1000) { fprintf(stderr, "need >= 1000 bars\n"); return 1; }

    const int H = atoi(argv[2]);
    if (H < 1) { fprintf(stderr, "horizonBars must be >= 1\n"); return 1; }
    const char* model = argc > 3 ? argv[3] : "fractional";

    indicators::VolForecastConfig cfg;
    cfg.horizonBars = H;
    if (!strcmp(model, "trailing"))        cfg.model = indicators::VolModel::Trailing;
    else if (!strcmp(model, "har"))        cfg.model = indicators::VolModel::HAR;
    else if (!strcmp(model, "fractional")) cfg.model = indicators::VolModel::FractionalHAR;
    else { fprintf(stderr, "unknown model '%s'\n", model); return 1; }

    std::vector<double> fc = indicators::forecastVolatility(s.close, cfg);

    // Log returns, and the annualization factor measured from the data.
    const double ann = std::sqrt(s.barsPerYear());
    std::vector<double> r(s.size(), std::nan(""));
    for (size_t i = 1; i < s.size(); ++i)
        if (s.close[i] > 0.0 && s.close[i - 1] > 0.0)
            r[i] = std::log(s.close[i] / s.close[i - 1]);

    printf("timestamp,forecast_annual,realized_annual\n");
    for (size_t i = 0; i < s.size(); ++i) {
        // Realized per-bar stddev over bars i+1 .. i+H, about zero mean (the
        // convention a variance swap and DVOL both use: it is the second
        // moment that gets paid, not the variance about a fitted drift).
        double ss = 0.0; size_t n = 0;
        for (size_t k = i + 1; k <= i + (size_t)H && k < s.size(); ++k)
            if (std::isfinite(r[k])) { ss += r[k] * r[k]; ++n; }
        bool full = (i + (size_t)H) < s.size() && n == (size_t)H;
        double rv = full ? std::sqrt(ss / (double)n) * ann : std::nan("");

        printf("%lld,", (long long)s.timestamp[i]);
        if (std::isfinite(fc[i])) printf("%.8f,", fc[i] * ann); else printf(",");
        if (std::isfinite(rv))    printf("%.8f\n", rv);        else printf("\n");
    }
    return 0;
}
