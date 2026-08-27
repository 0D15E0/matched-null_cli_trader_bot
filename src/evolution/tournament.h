#pragma once
#include "../backtest/engine.h"
#include "../concurrency/thread_pool.h"
#include "../core/candle.h"
#include "../strategy/hurst_regime_filter.h"
#include "../strategy/zoo/gates.h"
#include "../strategy/zoo/registry.h"
#include "fitness.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace trader::evolution {

// A survival tournament over the whole strategy zoo.
//
// The loop is the plain one: evaluate every candidate, keep the ones whose
// fitness clears a threshold, breed the survivors, discard the rest, repeat.
// What makes it worth building rather than assuming is the three decisions
// underneath it, each of which this repo has previously got wrong and paid
// for:
//
//   1. WHAT "MAKES MORE THAN X" MEANS. Not total return, and not Sharpe.
//      On a decade of an asset that rose 280x, any long/flat rule shows a
//      huge return, and selecting on it selects for market beta. The
//      threshold is applied to EXCESS annualized Sharpe over buy-and-hold,
//      minus a quadratic drawdown penalty, taken as the MINIMUM across every
//      environment - so a candidate is only as good as its worst market and
//      cannot win by specializing. That is evolution::combinedFitness, used
//      unchanged.
//
//   2. WHERE IT IS MEASURED. Evolution only ever sees the training window.
//      The holdout is sliced off before generation zero and touched exactly
//      once, at the end, on the finalists. Every previous search in this repo
//      reported its results on the same candles it had maximized over.
//
//   3. WHAT THE SURVIVORS ARE COMPARED TO. Two control families ride along
//      in the population and are bred like everything else: coin-flip entries
//      and buy-first-bar-never-sell. A tournament in which random entries
//      clear the survival threshold has measured its own selection pressure,
//      not an edge - and with thousands of candidates that is the default
//      expectation, not a remote risk. The deflated Sharpe reported at the
//      end says the same thing analytically.
//
// Replication is deliberately richer than parameter jitter. An individual
// carries, besides its family's parameters, two STRUCTURAL genes: an optional
// regime gate (Hurst, permutation entropy, Hawkes intensity, variance ratio)
// and an optional volatility target. So a surviving Donchian breakout can
// acquire a Hawkes filter and a 15% vol target it was not born with, and the
// search can reach combinations no single family defines.


// Spearman rank correlation, with average ranks for ties.
inline double spearmanRho(const std::vector<double>& x, const std::vector<double>& y) {
    size_t n = std::min(x.size(), y.size());
    if (n < 4) return std::nan("");
    auto ranks = [n](const std::vector<double>& v) {
        std::vector<size_t> idx(n);
        for (size_t i = 0; i < n; ++i) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return v[a] < v[b]; });
        std::vector<double> r(n, 0.0);
        size_t i = 0;
        while (i < n) {
            size_t j = i;
            while (j + 1 < n && v[idx[j + 1]] == v[idx[i]]) ++j;
            double avg = (static_cast<double>(i) + static_cast<double>(j)) / 2.0 + 1.0;
            for (size_t k = i; k <= j; ++k) r[idx[k]] = avg;
            i = j + 1;
        }
        return r;
    };
    auto rx = ranks(x), ry = ranks(y);
    double mx = 0, my = 0;
    for (size_t i = 0; i < n; ++i) { mx += rx[i]; my += ry[i]; }
    mx /= n; my /= n;
    double num = 0, dx = 0, dy = 0;
    for (size_t i = 0; i < n; ++i) {
        num += (rx[i] - mx) * (ry[i] - my);
        dx += (rx[i] - mx) * (rx[i] - mx);
        dy += (ry[i] - my) * (ry[i] - my);
    }
    double den = std::sqrt(dx * dy);
    return den > 0.0 ? num / den : 0.0;
}

// Two-sided permutation p-value for a Spearman correlation.
//
// A t-approximation would be wrong here for two reasons: n is around twenty,
// and the quantities being correlated are themselves the maxima of searches,
// so their marginal distributions are nothing like normal. Shuffling the
// labels makes no distributional assumption at all.
inline double permutationP(const std::vector<double>& x, const std::vector<double>& y,
                            double rho, int iters = 20000, unsigned seed = 11u) {
    if (std::isnan(rho)) return std::nan("");
    std::mt19937 g(seed);
    std::vector<double> shuffled = y;
    int hits = 0;
    for (int it = 0; it < iters; ++it) {
        std::shuffle(shuffled.begin(), shuffled.end(), g);
        if (std::fabs(spearmanRho(x, shuffled)) >= std::fabs(rho)) ++hits;
    }
    return static_cast<double>(hits + 1) / static_cast<double>(iters + 1);
}

enum class GateKind { None, Hurst, PermEntropy, Hawkes, VarianceRatio };

inline const char* gateName(GateKind g) {
    switch (g) {
        case GateKind::Hurst: return "hurst";
        case GateKind::PermEntropy: return "pe";
        case GateKind::Hawkes: return "hawkes";
        case GateKind::VarianceRatio: return "vr";
        default: return "none";
    }
}

// The volatility target is a discrete gene rather than a continuous one.
// Sizing is the highest-leverage knob in the engine, and letting a search
// tune it continuously invites it to find the exact fraction that flatters
// one particular drawdown path. A short ladder makes the choice legible:
// either the candidate needs sizing to survive or it does not.
inline const std::vector<double>& volTargetLadder() {
    static const std::vector<double> kLadder{0.0, 0.10, 0.15, 0.20, 0.30, 0.40};
    return kLadder;
}

struct Individual {
    std::string family;
    std::vector<double> params;       // raw, in the family's own units
    GateKind gate = GateKind::None;
    std::vector<double> gateGenes{0.5, 0.5, 0.5};  // normalized [0,1], decoded per gate
    int volTargetIndex = 0;

    // Bookkeeping, not genetics.
    double fitness = -1e9;
    int bornGeneration = 0;
    int id = 0;
    int parentId = -1;
    std::string origin = "seed";      // seed | mutant | crossover | immigrant | elite

    double volTarget() const { return volTargetLadder()[static_cast<size_t>(volTargetIndex)]; }

    // Genome identity, used both to cache evaluations and to report lineage.
    std::string key() const {
        std::ostringstream os;
        os << family;
        os << std::setprecision(6);
        for (double p : params) os << ':' << p;
        os << '|' << gateName(gate);
        if (gate != GateKind::None) for (double g : gateGenes) os << ':' << g;
        os << "|vt" << volTargetIndex;
        return os.str();
    }

    std::string label() const {
        std::ostringstream os;
        os << family;
        if (gate != GateKind::None) os << '+' << gateName(gate);
        if (volTargetIndex > 0) os << " vt=" << std::fixed << std::setprecision(2) << volTarget();
        return os.str();
    }
};

// Decode a normalized gene onto an inclusive range.
inline double decodeGene(double g, double lo, double hi) {
    return lo + std::clamp(g, 0.0, 1.0) * (hi - lo);
}
inline int decodeGeneInt(double g, int lo, int hi) {
    return static_cast<int>(std::lround(decodeGene(g, lo, hi)));
}

inline std::unique_ptr<Strategy> applyGate(std::unique_ptr<Strategy> inner, const Individual& ind) {
    switch (ind.gate) {
        case GateKind::Hurst:
            return std::make_unique<HurstRegimeFilter>(
                std::move(inner), decodeGeneInt(ind.gateGenes[0], 50, 400),
                decodeGene(ind.gateGenes[1], 0.45, 0.70), HurstEstimator::StructureFunction);
        case GateKind::PermEntropy:
            return std::make_unique<PermutationEntropyGate>(
                std::move(inner), decodeGeneInt(ind.gateGenes[0], 3, 5),
                decodeGeneInt(ind.gateGenes[1], 60, 500), decodeGene(ind.gateGenes[2], 0.5, 1.0));
        case GateKind::Hawkes:
            return std::make_unique<HawkesIntensityGate>(
                std::move(inner), 50, decodeGene(ind.gateGenes[0], 1.5, 4.0),
                decodeGene(ind.gateGenes[1], 5.0, 200.0), 250,
                decodeGene(ind.gateGenes[2], 0.3, 1.0));
        case GateKind::VarianceRatio:
            return std::make_unique<VarianceRatioGate>(
                std::move(inner), decodeGeneInt(ind.gateGenes[0], 100, 600),
                decodeGeneInt(ind.gateGenes[1], 2, 20), decodeGene(ind.gateGenes[2], 0.8, 1.4));
        default:
            return inner;
    }
}

inline std::unique_ptr<Strategy> buildStrategy(const Individual& ind) {
    const auto* fam = zoo::findFamily(ind.family);
    if (!fam) return nullptr;
    auto s = fam->make(ind.params);
    if (!s) return nullptr;
    return applyGate(std::move(s), ind);
}

struct Environment {
    std::string label;        // "BTC_USDT:14400"
    CandleSeries train;       // what evolution is allowed to see
    CandleSeries holdoutFull; // holdout window PLUS a warm-up prefix from train
    int64_t holdoutFrom = 0;  // first timestamp actually scored in the holdout
};

struct TournamentConfig {
    int population = 120;
    int generations = 10;
    double surviveThreshold = 0.0;  // the user's X, on excess-Sharpe fitness
    double trainFraction = 0.70;
    int warmupBars = 400;           // holdout warm-up prefix
    double mutationSigma = 0.18;    // normalized units, per parameter
    double mutationDecay = 0.88;    // sigma shrinks each generation
    double crossoverRate = 0.30;
    double immigrantFraction = 0.15;
    int eliteKeep = 10;
    int seedsPerFamily = 4;         // random draws per family at generation 0
    // Speciation. Without it, whichever family is ahead after one or two
    // generations takes every breeding slot and the tournament reports its
    // dominance as a finding, when what actually happened is that the other
    // families were never given the chance to be tuned at all. With it,
    // breeding capacity is split across the surviving FAMILIES first and by
    // individual rank only within a family - so a family with one marginal
    // survivor still gets to develop, and the comparison at the end is
    // between tuned families rather than between one tuned family and a
    // field of untuned defaults.
    bool speciate = true;
    int eliteKeepPerFamily = 2;

    // TUNING PHASE. For the first `tuneGenerations`, survival is decided
    // WITHIN each family - the top few of every family breed - and the
    // threshold X is not applied at all. Only afterwards does X start
    // discarding.
    //
    // This exists because the first version of this tournament did not have
    // it, and the consequence was severe: on four crypto instruments exactly
    // one family ever cleared X at its untuned defaults, so it took every
    // breeding slot from generation two onward and the other twenty-one
    // families were compared to it while still sitting at their published
    // default parameters. The run then reported that family's dominance,
    // which was really a statement about which family happened to start
    // closest to the threshold. Tuning everything first and applying X to
    // TUNED champions is the comparison the question actually asks for.
    int tuneGenerations = -1;   // -1 = half the run
    int tuneKeepPerFamily = 4;
    bool allowGates = true;
    bool allowVolTarget = true;
    unsigned seed = 20260824u;
    int jobs = 0;                   // 0 = hardware concurrency
    std::vector<std::string> onlyFamilies;
    std::vector<std::string> excludeFamilies;
    // Families whose published cost weight exceeds this are left out of a
    // default tournament, and the command says which and why. Naming one in
    // --only overrides the limit: an explicit request is not a mistake.
    double costLimit = 50.0;
    BacktestConfig backtest;
    FitnessConfig fitness;
};

struct EvalResult {
    double fitness = -1e9;
    std::vector<BacktestReport> reports;
};

class Tournament {
public:
    Tournament(std::vector<Environment> envs, TournamentConfig cfg)
        : envs_(std::move(envs)), cfg_(cfg), rng_(cfg.seed) {}

    // --- evaluation -------------------------------------------------------

    EvalResult evaluate(const Individual& ind, bool onHoldout) const {
        EvalResult out;
        out.reports.reserve(envs_.size());
        for (const auto& env : envs_) {
            auto strategy = buildStrategy(ind);
            if (!strategy) return out;
            BacktestConfig bc = cfg_.backtest;
            bc.volTargetAnnual = ind.volTarget();
            // The deflated Sharpe is meaningless without the number of
            // candidates the reported one was selected from, and is actively
            // misleading with a wrong one - so it is only populated on the
            // holdout evaluations, where that count is known and final.
            bc.searchTrials = onHoldout ? searchTrialsForReport_ : 0;
            const CandleSeries& series = onHoldout ? env.holdoutFull : env.train;
            bc.evaluateFromTimestamp = onHoldout ? env.holdoutFrom : 0;
            BacktestEngine engine(bc);
            BacktestReport r = engine.run(series, *strategy);
            r.symbol = env.label;
            out.reports.push_back(std::move(r));
        }
        out.fitness = combinedFitness(out.reports, cfg_.fitness);
        return out;
    }

    // --- the loop ---------------------------------------------------------

    struct GenerationRecord {
        int generation = 0;
        int evaluated = 0;
        int survivors = 0;
        double bestFitness = -1e9;
        double medianFitness = -1e9;
        std::string bestLabel;
        std::map<std::string, int> familyCensus;   // survivors by family
        int distinctFamilies = 0;
        bool tuning = false;
    };

    struct Outcome {
        std::vector<GenerationRecord> history;
        std::vector<Individual> finalists;             // best distinct genomes, by train fitness
        std::vector<EvalResult> finalistTrain;
        std::vector<EvalResult> finalistHoldout;
        size_t totalCandidates = 0;
        size_t distinctCandidates = 0;

        // The EMPIRICAL NULL. Control genomes are bred and selected exactly
        // like everything else, so the best train fitness any of them reached
        // is a direct measurement of what this tournament's selection
        // pressure extracts from candidates that cannot work. It is the bar a
        // real survivor has to clear, and unlike the deflated Sharpe it makes
        // no assumption that the trials were independent or normal - which,
        // in a population of mutated survivors, they emphatically are not.
        // Best genome each family reached, tuned. This is the table that
        // answers "which of these ideas is worth anything", because every
        // family in it was given the same chance to be adapted.
        std::vector<Individual> familyChampions;
        std::vector<EvalResult> familyChampionHoldout;

        // DOES SELECTION TRANSFER? The rank correlation between what the
        // search maximized and what the holdout delivered, across family
        // champions. Reported twice on purpose:
        //
        //   rhoAll      - over every family. A high value here is easy to
        //                 get and easy to over-read: most of it comes from
        //                 the search correctly identifying which families are
        //                 hopeless, which is real but is not what anybody
        //                 runs a search for.
        //   rhoTopHalf  - over the better half only, i.e. among the
        //                 candidates you would actually consider deploying.
        //                 This is the number that says whether the search can
        //                 pick a winner, and it is usually the one that dies.
        double rhoAll = std::nan(""), pAll = std::nan("");
        double rhoTopHalf = std::nan(""), pTopHalf = std::nan("");
        size_t rankedFamilies = 0;

        // Where the coin-flip control landed on the holdout, among all family
        // champions. Rank 1 means nothing in the pool generalized better than
        // random entries.
        size_t controlHoldoutRank = 0, holdoutRankedCount = 0;

        size_t controlGenomes = 0;
        double bestControlTrainFitness = -1e9;
        std::string bestControlLabel;
        double bestControlHoldoutMin = 0.0;
        bool haveControl = false;
    };

    Outcome run(std::ostream& log) {
        Outcome outcome;
        ThreadPool pool(cfg_.jobs > 0 ? static_cast<size_t>(cfg_.jobs)
                                       : std::max(2u, std::thread::hardware_concurrency()));
        auto population = seedPopulation();
        double sigma = cfg_.mutationSigma;
        const int tuneGens = cfg_.tuneGenerations >= 0
                                 ? std::min(cfg_.tuneGenerations, cfg_.generations)
                                 : cfg_.generations / 2;
        log << "Phase 1 (tune, generations 0.." << (tuneGens - 1)
            << "): every family keeps its own best " << cfg_.tuneKeepPerFamily
            << " and breeds; X is not applied.\n"
            << "Phase 2 (select, generations " << tuneGens << ".." << (cfg_.generations - 1)
            << "): survival requires fitness >= X = " << std::fixed << std::setprecision(3)
            << cfg_.surviveThreshold << ".\n\n";

        for (int gen = 0; gen < cfg_.generations; ++gen) {
            evaluatePopulation(population, pool);

            std::sort(population.begin(), population.end(),
                      [](const Individual& a, const Individual& b) { return a.fitness > b.fitness; });

            GenerationRecord rec;
            rec.generation = gen;
            rec.evaluated = static_cast<int>(population.size());
            rec.bestFitness = population.empty() ? -1e9 : population.front().fitness;
            rec.bestLabel = population.empty() ? "-" : population.front().label();
            rec.medianFitness = population.empty() ? -1e9
                                                    : population[population.size() / 2].fitness;

            std::vector<Individual> survivors;
            rec.tuning = gen < tuneGens;
            if (rec.tuning) {
                // Within-family selection: every family keeps its own best few
                // so every family gets tuned, and X does not apply yet.
                std::map<std::string, int> kept;
                for (const auto& ind : population) {
                    if (kept[ind.family] >= cfg_.tuneKeepPerFamily) continue;
                    if (ind.fitness <= -1e8) continue;   // never evaluated
                    kept[ind.family]++;
                    survivors.push_back(ind);
                }
            } else {
                for (const auto& ind : population)
                    if (ind.fitness >= cfg_.surviveThreshold) survivors.push_back(ind);
            }
            rec.survivors = static_cast<int>(survivors.size());
            for (const auto& s : survivors) rec.familyCensus[s.family]++;
            rec.distinctFamilies = static_cast<int>(rec.familyCensus.size());
            outcome.history.push_back(rec);
            reportGeneration(log, rec, sigma);

            // Track the best distinct genomes seen anywhere in the run, so a
            // strong individual that appears in generation 2 and is later
            // crowded out is still a finalist.
            for (const auto& ind : population) rememberBest(ind);

            if (gen + 1 >= cfg_.generations) break;
            population = breed(survivors, population, sigma, gen + 1);
            sigma *= cfg_.mutationDecay;
        }

        outcome.totalCandidates = totalEvaluations_;
        outcome.distinctCandidates = cache_.size();
        searchTrialsForReport_ = cache_.size();

        std::vector<Individual> best;
        for (auto& [k, v] : bestByKey_) best.push_back(v);
        std::sort(best.begin(), best.end(),
                  [](const Individual& a, const Individual& b) { return a.fitness > b.fitness; });

        // Measure the empirical null before choosing finalists, so it is
        // available even when no control makes the table.
        const Individual* champControl = nullptr;
        for (const auto& ind : best) {
            if (ind.family.rfind("control_", 0) != 0) continue;
            ++outcome.controlGenomes;
            if (!champControl) champControl = &ind;   // `best` is already sorted
        }
        if (champControl) {
            outcome.haveControl = true;
            outcome.bestControlTrainFitness = champControl->fitness;
            outcome.bestControlLabel = champControl->label();
            auto ctrlHold = evaluate(*champControl, true);
            double mn = 1e9;
            for (const auto& r : ctrlHold.reports) mn = std::min(mn, r.excessSharpe);
            outcome.bestControlHoldoutMin = ctrlHold.reports.empty() ? 0.0 : mn;
        }

        // Finalists: the best genome per distinct STRUCTURE. Without this the
        // table fills with twelve near-identical mutants of one winner and
        // says nothing about what else the tournament found - the survivors
        // of a converged population are highly correlated, so listing twelve
        // of them is listing one result twelve times.
        std::set<std::string> seenStructure;
        size_t want = 12;
        for (const auto& ind : best) {
            if (outcome.finalists.size() >= want) break;
            if (!seenStructure.insert(ind.label()).second) continue;
            outcome.finalists.push_back(ind);
        }
        // The best control always gets a row, whether or not it earned one on
        // fitness. A reader must be able to see what luck scored without
        // going looking for it.
        if (champControl) {
            bool present = false;
            for (const auto& f : outcome.finalists)
                if (f.family.rfind("control_", 0) == 0) present = true;
            if (!present) outcome.finalists.push_back(*champControl);
        }
        for (const auto& ind : outcome.finalists) {
            outcome.finalistTrain.push_back(evaluate(ind, false));
            outcome.finalistHoldout.push_back(evaluate(ind, true));
        }

        for (auto& [fam, ind] : bestByFamily_) outcome.familyChampions.push_back(ind);
        std::sort(outcome.familyChampions.begin(), outcome.familyChampions.end(),
                  [](const Individual& a, const Individual& b) { return a.fitness > b.fitness; });
        for (const auto& ind : outcome.familyChampions)
            outcome.familyChampionHoldout.push_back(evaluate(ind, true));

        // Rank-transfer diagnostics over the non-control champions. Genomes
        // sitting on the activity-floor penalty (they barely traded) are
        // excluded: their fitness is -50 by construction and carries no
        // information about ordering, but it would dominate a rank statistic.
        std::vector<double> tr, ho;
        std::vector<std::pair<double, double>> pairs;
        for (size_t i = 0; i < outcome.familyChampions.size(); ++i) {
            const auto& ind = outcome.familyChampions[i];
            if (ind.family.rfind("control_", 0) == 0) continue;
            if (ind.fitness < -10.0) continue;
            double mn = 1e9;
            for (const auto& r : outcome.familyChampionHoldout[i].reports)
                mn = std::min(mn, r.excessSharpe);
            if (outcome.familyChampionHoldout[i].reports.empty()) continue;
            pairs.push_back({ind.fitness, mn});
        }
        std::sort(pairs.begin(), pairs.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& pr : pairs) { tr.push_back(pr.first); ho.push_back(pr.second); }
        outcome.rankedFamilies = tr.size();
        if (tr.size() >= 4) {
            outcome.rhoAll = spearmanRho(tr, ho);
            outcome.pAll = permutationP(tr, ho, outcome.rhoAll);
            size_t half = tr.size() / 2;
            if (half >= 4) {
                std::vector<double> trh(tr.begin(), tr.begin() + half);
                std::vector<double> hoh(ho.begin(), ho.begin() + half);
                outcome.rhoTopHalf = spearmanRho(trh, hoh);
                outcome.pTopHalf = permutationP(trh, hoh, outcome.rhoTopHalf);
            }
        }

        // Control's holdout rank among all champions, controls included.
        std::vector<std::pair<double, bool>> holdoutRank;
        for (size_t i = 0; i < outcome.familyChampions.size(); ++i) {
            if (outcome.familyChampions[i].fitness < -10.0) continue;
            if (outcome.familyChampionHoldout[i].reports.empty()) continue;
            double mn = 1e9;
            for (const auto& r : outcome.familyChampionHoldout[i].reports)
                mn = std::min(mn, r.excessSharpe);
            holdoutRank.push_back({mn, outcome.familyChampions[i].family == "control_random"});
        }
        std::sort(holdoutRank.begin(), holdoutRank.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        outcome.holdoutRankedCount = holdoutRank.size();
        for (size_t i = 0; i < holdoutRank.size(); ++i)
            if (holdoutRank[i].second) { outcome.controlHoldoutRank = i + 1; break; }

        return outcome;
    }

private:
    // --- population construction -----------------------------------------

    std::vector<std::string> activeFamilies() const {
        std::vector<std::string> names;
        for (const auto& f : zoo::families()) {
            if (!cfg_.onlyFamilies.empty() &&
                std::find(cfg_.onlyFamilies.begin(), cfg_.onlyFamilies.end(), f.name) ==
                    cfg_.onlyFamilies.end())
                continue;
            if (std::find(cfg_.excludeFamilies.begin(), cfg_.excludeFamilies.end(), f.name) !=
                cfg_.excludeFamilies.end())
                continue;
            if (cfg_.onlyFamilies.empty() && f.costWeight > cfg_.costLimit) continue;
            names.push_back(f.name);
        }
        return names;
    }

    Individual makeDefault(const std::string& family) {
        Individual ind;
        ind.family = family;
        const auto* f = zoo::findFamily(family);
        for (const auto& p : f->params) ind.params.push_back(p.def);
        ind.id = nextId_++;
        ind.origin = "seed";
        return ind;
    }

    Individual makeRandom(const std::string& family, int gen) {
        Individual ind;
        ind.family = family;
        const auto* f = zoo::findFamily(family);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        for (const auto& p : f->params) {
            double v = p.lo + u(rng_) * (p.hi - p.lo);
            ind.params.push_back(p.isInt ? std::lround(v) : v);
        }
        if (cfg_.allowGates && u(rng_) < 0.35) ind.gate = randomGate();
        for (auto& g : ind.gateGenes) g = u(rng_);
        if (cfg_.allowVolTarget) {
            std::uniform_int_distribution<int> vi(0, static_cast<int>(volTargetLadder().size()) - 1);
            ind.volTargetIndex = vi(rng_);
        }
        ind.id = nextId_++;
        ind.bornGeneration = gen;
        ind.origin = gen == 0 ? "seed" : "immigrant";
        return ind;
    }

    GateKind randomGate() {
        std::uniform_int_distribution<int> d(1, 4);
        return static_cast<GateKind>(d(rng_));
    }

    std::vector<Individual> seedPopulation() {
        std::vector<Individual> pop;
        auto names = activeFamilies();
        // Every family enters at its published defaults. That guarantees the
        // literature's own parameterization is in the tournament and does not
        // depend on a random draw happening to find it.
        for (const auto& n : names) pop.push_back(makeDefault(n));
        for (const auto& n : names)
            for (int k = 0; k < cfg_.seedsPerFamily; ++k) pop.push_back(makeRandom(n, 0));
        while (static_cast<int>(pop.size()) < cfg_.population) {
            std::uniform_int_distribution<size_t> pick(0, names.size() - 1);
            pop.push_back(makeRandom(names[pick(rng_)], 0));
        }
        return pop;
    }

    // --- breeding ---------------------------------------------------------

    void mutateParams(Individual& ind, double sigma) {
        const auto* f = zoo::findFamily(ind.family);
        std::normal_distribution<double> nd(0.0, sigma);
        for (size_t i = 0; i < ind.params.size() && i < f->params.size(); ++i) {
            const auto& spec = f->params[i];
            double span = spec.hi - spec.lo;
            if (span <= 0.0) continue;
            double v = ind.params[i] + nd(rng_) * span;
            // Reflect at the bounds rather than clamping: clamping piles
            // probability mass onto the endpoints, and a search that spends
            // its time at parameter limits is reporting the limits, not an
            // optimum.
            if (v < spec.lo) v = spec.lo + (spec.lo - v);
            if (v > spec.hi) v = spec.hi - (v - spec.hi);
            v = std::clamp(v, spec.lo, spec.hi);
            ind.params[i] = spec.isInt ? std::lround(v) : v;
        }
    }

    void mutateStructure(Individual& ind, double sigma) {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        if (cfg_.allowGates && u(rng_) < 0.15) {
            ind.gate = u(rng_) < 0.35 ? GateKind::None : randomGate();
        }
        std::normal_distribution<double> nd(0.0, sigma);
        for (auto& g : ind.gateGenes) g = std::clamp(g + nd(rng_), 0.0, 1.0);
        if (cfg_.allowVolTarget && u(rng_) < 0.20) {
            std::uniform_int_distribution<int> vi(0, static_cast<int>(volTargetLadder().size()) - 1);
            ind.volTargetIndex = vi(rng_);
        }
    }

    Individual cross(const Individual& a, const Individual& b, int gen) {
        Individual child = a;
        std::uniform_real_distribution<double> u(0.0, 1.0);
        for (size_t i = 0; i < child.params.size() && i < b.params.size(); ++i)
            if (u(rng_) < 0.5) child.params[i] = b.params[i];
        for (size_t i = 0; i < child.gateGenes.size(); ++i)
            if (u(rng_) < 0.5) child.gateGenes[i] = b.gateGenes[i];
        if (u(rng_) < 0.5) child.gate = b.gate;
        if (u(rng_) < 0.5) child.volTargetIndex = b.volTargetIndex;
        child.id = nextId_++;
        child.parentId = a.id;
        child.bornGeneration = gen;
        child.origin = "crossover";
        child.fitness = -1e9;
        return child;
    }

    std::vector<Individual> breed(const std::vector<Individual>& survivors,
                                   const std::vector<Individual>& ranked, double sigma, int gen) {
        std::vector<Individual> next;
        auto names = activeFamilies();
        std::uniform_real_distribution<double> u(0.0, 1.0);

        // Elites carry over unchanged, so a generation can never be worse
        // than the one before it. Under speciation the elite slots are
        // allocated per family, for the same reason breeding slots are.
        if (cfg_.speciate) {
            std::map<std::string, int> keptPerFamily;
            for (const auto& s : survivors) {
                if (static_cast<int>(next.size()) >= cfg_.eliteKeep * 3) break;
                if (keptPerFamily[s.family] >= cfg_.eliteKeepPerFamily) continue;
                keptPerFamily[s.family]++;
                Individual e = s;
                e.origin = "elite";
                next.push_back(e);
            }
        } else {
            for (size_t i = 0; i < survivors.size() && static_cast<int>(i) < cfg_.eliteKeep; ++i) {
                Individual e = survivors[i];
                e.origin = "elite";
                next.push_back(e);
            }
        }

        int immigrants = static_cast<int>(cfg_.immigrantFraction * cfg_.population);
        int breedSlots = cfg_.population - static_cast<int>(next.size()) - immigrants;

        // Survivor indices grouped by family, each group already in rank order
        // because `survivors` is.
        std::map<std::string, std::vector<size_t>> byFamily;
        for (size_t i = 0; i < survivors.size(); ++i) byFamily[survivors[i].family].push_back(i);

        // Which family each breeding slot belongs to.
        std::vector<const std::vector<size_t>*> slotFamily;
        if (cfg_.speciate && !byFamily.empty() && breedSlots > 0) {
            std::vector<const std::vector<size_t>*> groups;
            for (const auto& [fam, idxs] : byFamily) groups.push_back(&idxs);
            for (int k = 0; k < breedSlots; ++k)
                slotFamily.push_back(groups[static_cast<size_t>(k) % groups.size()]);
        }

        if (!survivors.empty() && breedSlots > 0) {
            // Offspring are allocated by rank, not by fitness value. Fitness
            // here is a difference of Sharpes with a drawdown penalty - it can
            // be negative and is not on a ratio scale, so anything
            // proportional to its magnitude would be meaningless.
            std::vector<double> weight(survivors.size());
            for (size_t i = 0; i < survivors.size(); ++i)
                weight[i] = 1.0 / (1.0 + static_cast<double>(i));
            std::discrete_distribution<size_t> pick(weight.begin(), weight.end());
            for (int k = 0; k < breedSlots; ++k) {
                size_t parentIdx;
                if (!slotFamily.empty()) {
                    const auto& group = *slotFamily[static_cast<size_t>(k)];
                    std::vector<double> gw(group.size());
                    for (size_t i = 0; i < group.size(); ++i) gw[i] = 1.0 / (1.0 + static_cast<double>(i));
                    std::discrete_distribution<size_t> gp(gw.begin(), gw.end());
                    parentIdx = group[gp(rng_)];
                } else {
                    parentIdx = pick(rng_);
                }
                const Individual& parent = survivors[parentIdx];
                Individual child;
                if (u(rng_) < cfg_.crossoverRate && survivors.size() > 1) {
                    // Crossover only within a family: parameter vectors of
                    // different families index different quantities, and
                    // mixing them produces a genome that is not a member of
                    // either family while still carrying one of their names.
                    std::vector<size_t> sameFamily;
                    for (size_t i = 0; i < survivors.size(); ++i)
                        if (survivors[i].family == parent.family) sameFamily.push_back(i);
                    if (sameFamily.size() > 1) {
                        std::uniform_int_distribution<size_t> sf(0, sameFamily.size() - 1);
                        child = cross(parent, survivors[sameFamily[sf(rng_)]], gen);
                    } else {
                        child = parent;
                    }
                } else {
                    child = parent;
                }
                if (child.origin != "crossover") {
                    child.id = nextId_++;
                    child.parentId = parent.id;
                    child.bornGeneration = gen;
                    child.origin = "mutant";
                    child.fitness = -1e9;
                }
                mutateParams(child, sigma);
                mutateStructure(child, sigma);
                next.push_back(child);
            }
        }

        // Immigrants keep families that died out reachable. Without them a
        // tournament collapses onto whichever family got lucky early and then
        // reports that family's dominance as a finding.
        while (static_cast<int>(next.size()) < cfg_.population) {
            std::uniform_int_distribution<size_t> pickFam(0, names.size() - 1);
            next.push_back(makeRandom(names[pickFam(rng_)], gen));
        }
        (void)ranked;
        return next;
    }

    // --- plumbing ---------------------------------------------------------

    void evaluatePopulation(std::vector<Individual>& pop, ThreadPool& pool) {
        std::vector<std::future<double>> futures;
        futures.reserve(pop.size());
        for (auto& ind : pop) {
            futures.push_back(pool.submit([this, &ind]() -> double {
                std::string k = ind.key();
                {
                    std::lock_guard<std::mutex> lock(cacheMutex_);
                    auto it = cache_.find(k);
                    if (it != cache_.end()) return it->second;
                }
                double fit = evaluate(ind, false).fitness;
                {
                    std::lock_guard<std::mutex> lock(cacheMutex_);
                    cache_[k] = fit;
                    ++totalEvaluations_;
                }
                return fit;
            }));
        }
        for (size_t i = 0; i < pop.size(); ++i) pop[i].fitness = futures[i].get();
    }

    void rememberBest(const Individual& ind) {
        auto it = bestByKey_.find(ind.key());
        if (it == bestByKey_.end() || ind.fitness > it->second.fitness) bestByKey_[ind.key()] = ind;
        auto fit = bestByFamily_.find(ind.family);
        if (fit == bestByFamily_.end() || ind.fitness > fit->second.fitness)
            bestByFamily_[ind.family] = ind;
    }

    static void reportGeneration(std::ostream& os, const GenerationRecord& rec, double sigma) {
        os << (rec.tuning ? "TUNE " : "SEL  ") << std::setw(2) << rec.generation
           << " | evaluated " << std::setw(4) << rec.evaluated
           << " | survivors " << std::setw(4) << rec.survivors
           << " | families " << std::setw(2) << rec.distinctFamilies
           << " | best " << std::fixed << std::setprecision(3) << std::setw(7) << rec.bestFitness
           << " | median " << std::setw(7) << rec.medianFitness
           << " | sigma " << std::setprecision(3) << sigma
           << " | " << rec.bestLabel << "\n";
        if (!rec.familyCensus.empty()) {
            std::vector<std::pair<std::string, int>> census(rec.familyCensus.begin(),
                                                             rec.familyCensus.end());
            std::sort(census.begin(), census.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            os << "        survivors: ";
            for (size_t i = 0; i < census.size() && i < 10; ++i)
                os << census[i].first << "x" << census[i].second << "  ";
            os << "\n";
        }
        os.flush();
    }

    std::vector<Environment> envs_;
    TournamentConfig cfg_;
    std::mt19937 rng_;
    int nextId_ = 0;
    size_t totalEvaluations_ = 0;
    mutable std::mutex cacheMutex_;
    std::unordered_map<std::string, double> cache_;
    std::map<std::string, Individual> bestByKey_;
    std::map<std::string, Individual> bestByFamily_;
    size_t searchTrialsForReport_ = 0;
};

} // namespace trader::evolution
