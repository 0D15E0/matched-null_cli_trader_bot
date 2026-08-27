#pragma once
#include "../strategy/odiseo_strategy.h"
#include "../backtest/engine.h"
#include "../core/candle.h"
#include "fitness.h"

#include <vector>
#include <random>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>

namespace trader::evolution {

// A Darwinian ("natural selection") parameter search over OdiseoParams.
//
// Rather than tuning a strategy on one instrument at a time (as we did by
// hand for BTC_USDT and ASML.AS separately - see strategy/odiseo_strategy.h
// header comments), this evaluates every candidate genome against BOTH
// environments simultaneously and selects for genomes that survive well
// in *both*, not ones that specialize in either. This directly targets
// generalization instead of per-asset overfitting: a genome that scores
// +5000% on crypto but -80% on equities is a worse "organism" here than
// one that scores +300%/+150% on both, because fitness is dominated by
// the *weaker* of its two environments (a minimax, not an average).
//
// Genetic algorithm mechanics:
//   - Genome: an OdiseoParams (8 genes: dmiWindow, atrWindow, adxThreshold,
//     atrStopMultiple, requireTenkanAboveKijun, tenkanWindow, kijunWindow,
//     spanBWindow).
//   - Fitness: run BacktestEngine on every provided CandleSeries with the
//     same genome, score each environment (see evolution/fitness.h) and
//     take the minimum across them - the "weakest link" determines fitness,
//     so a genome is only as fit as its worst environment. The per-
//     environment score is EXCESS annualized Sharpe over buy-and-hold minus
//     a quadratic drawdown penalty, not the raw t-statistic the original
//     used: scoring raw performance on assets that rose 15x-280x rewarded
//     market beta the strategy did not create, and the t-statistic scaled
//     with sample length so the minimum across environments was really
//     picking out whichever environment had the fewest bars.
//
//   - IMPORTANT: fitness is computed on whatever series it is handed. It is
//     the caller's job (see `evolve --train-end` / `--wf-folds` in main.cpp)
//     to hand it TRAINING data only and to report the winner on data it
//     never saw. Evaluating the selected genome on the same candles that
//     selected it, as this tool used to do, produces a number with no
//     predictive content whatsoever.
//   - Selection: tournament selection (pick k random genomes, keep the
//     fittest) - simple, doesn't require sorting the whole population,
//     naturally tunable selection pressure via tournament size.
//   - Crossover: uniform crossover (each gene independently inherited from
//     one parent or the other).
//   - Mutation: each gene independently has a chance to be perturbed
//     within its allowed bounds (small random walk) or, for the boolean
//     gene, flipped.
//   - Elitism: the single best genome is always carried over unchanged, so
//     fitness is monotonically non-decreasing across generations.
struct GenomeBounds {
    int dmiWindowMin = 7, dmiWindowMax = 28;
    int atrWindowMin = 7, atrWindowMax = 28;
    double adxThresholdMin = 10.0, adxThresholdMax = 35.0;
    double atrStopMultipleMin = 1.0, atrStopMultipleMax = 8.0;
    int tenkanWindowMin = 5, tenkanWindowMax = 15;
    int kijunWindowMin = 18, kijunWindowMax = 40;
    int spanBWindowMin = 40, spanBWindowMax = 80;
};

struct FitnessResult {
    double fitness = -1e9;
    std::vector<BacktestReport> reports;
};

class GeneticOptimizer {
public:
    // N environments (any number of markets and/or regime windows), stored by
    // value: the old version held references to caller-owned series, which
    // made it trivially easy to dangle when environments are built from
    // sliced temporaries.
    explicit GeneticOptimizer(std::vector<CandleSeries> series,
                               BacktestConfig backtestConfig = {}, GenomeBounds bounds = {},
                               FitnessConfig fitness = {},
                               unsigned seed = std::random_device{}())
        : series_(std::move(series)), backtestConfig_(backtestConfig),
          bounds_(bounds), fitness_(fitness), rng_(seed) {}

    GeneticOptimizer(const CandleSeries& seriesA, const CandleSeries& seriesB,
                      BacktestConfig backtestConfig = {}, GenomeBounds bounds = {},
                      FitnessConfig fitness = {},
                      unsigned seed = std::random_device{}())
        : series_{seriesA, seriesB}, backtestConfig_(backtestConfig),
          bounds_(bounds), fitness_(fitness), rng_(seed) {}

    static size_t trialCount(int populationSize, int generations) {
        return static_cast<size_t>(populationSize) * static_cast<size_t>(generations);
    }

    // Runs the evolutionary loop for `generations` generations with a
    // population of `populationSize`, printing the best genome's fitness
    // each generation. Returns the best genome found.
    OdiseoParams evolve(int populationSize = 40, int generations = 30,
                         int tournamentSize = 4, double mutationRate = 0.15) {
        std::vector<OdiseoParams> population;
        population.reserve(populationSize);
        for (int i = 0; i < populationSize; ++i) population.push_back(randomGenome());

        OdiseoParams best;
        double bestFitness = -1e9;

        for (int gen = 0; gen < generations; ++gen) {
            std::vector<FitnessResult> results;
            results.reserve(population.size());
            for (auto& genome : population) results.push_back(evaluate(genome));

            // Track the best genome seen so far (elitism across generations).
            for (size_t i = 0; i < population.size(); ++i) {
                if (results[i].fitness > bestFitness) {
                    bestFitness = results[i].fitness;
                    best = population[i];
                }
            }

            std::cout << "Generation " << (gen + 1) << "/" << generations
                       << " - best fitness so far: " << std::fixed << std::setprecision(3)
                       << bestFitness << "\n";

            // Build next generation: elitism (keep the best genome as-is),
            // then fill the rest via tournament-selected parents +
            // crossover + mutation.
            std::vector<OdiseoParams> next;
            next.reserve(population.size());
            next.push_back(best);

            while (next.size() < population.size()) {
                const OdiseoParams& parentA = tournamentSelect(population, results, tournamentSize);
                const OdiseoParams& parentB = tournamentSelect(population, results, tournamentSize);
                OdiseoParams child = crossover(parentA, parentB);
                mutate(child, mutationRate);
                next.push_back(child);
            }
            population = std::move(next);
        }

        return best;
    }

    // Evaluates a single genome against every environment; exposed so
    // callers can print full reports for the final best genome.
    FitnessResult evaluate(const OdiseoParams& genome) const {
        return evaluateOn(genome, series_, backtestConfig_);
    }

    // Score a genome against environments it was not selected on.
    FitnessResult evaluateOn(const OdiseoParams& genome,
                              const std::vector<CandleSeries>& series,
                              const BacktestConfig& config) const {
        FitnessResult result;
        BacktestEngine engine(config);
        result.reports.reserve(series.size());
        for (const auto& s : series) {
            OdiseoStrategy strat(genome);
            result.reports.push_back(engine.run(s, strat));
        }
        result.fitness = combinedFitness(result.reports, fitness_);
        return result;
    }

    const FitnessConfig& fitnessConfig() const { return fitness_; }

private:
    OdiseoParams randomGenome() {
        OdiseoParams p;
        p.dmiWindow = randInt(bounds_.dmiWindowMin, bounds_.dmiWindowMax);
        p.atrWindow = randInt(bounds_.atrWindowMin, bounds_.atrWindowMax);
        p.adxThreshold = randDouble(bounds_.adxThresholdMin, bounds_.adxThresholdMax);
        p.atrStopMultiple = randDouble(bounds_.atrStopMultipleMin, bounds_.atrStopMultipleMax);
        p.requireTenkanAboveKijun = randDouble(0.0, 1.0) > 0.5;
        p.tenkanWindow = randInt(bounds_.tenkanWindowMin, bounds_.tenkanWindowMax);
        p.kijunWindow = randInt(bounds_.kijunWindowMin, bounds_.kijunWindowMax);
        p.spanBWindow = randInt(bounds_.spanBWindowMin, bounds_.spanBWindowMax);
        return p;
    }

    const OdiseoParams& tournamentSelect(const std::vector<OdiseoParams>& population,
                                          const std::vector<FitnessResult>& results,
                                          int tournamentSize) {
        int bestIdx = randInt(0, static_cast<int>(population.size()) - 1);
        for (int i = 1; i < tournamentSize; ++i) {
            int idx = randInt(0, static_cast<int>(population.size()) - 1);
            if (results[idx].fitness > results[bestIdx].fitness) bestIdx = idx;
        }
        return population[bestIdx];
    }

    OdiseoParams crossover(const OdiseoParams& a, const OdiseoParams& b) {
        OdiseoParams child;
        child.dmiWindow = coinFlip() ? a.dmiWindow : b.dmiWindow;
        child.atrWindow = coinFlip() ? a.atrWindow : b.atrWindow;
        child.adxThreshold = coinFlip() ? a.adxThreshold : b.adxThreshold;
        child.atrStopMultiple = coinFlip() ? a.atrStopMultiple : b.atrStopMultiple;
        child.requireTenkanAboveKijun = coinFlip() ? a.requireTenkanAboveKijun : b.requireTenkanAboveKijun;
        child.tenkanWindow = coinFlip() ? a.tenkanWindow : b.tenkanWindow;
        child.kijunWindow = coinFlip() ? a.kijunWindow : b.kijunWindow;
        child.spanBWindow = coinFlip() ? a.spanBWindow : b.spanBWindow;
        return child;
    }

    void mutate(OdiseoParams& genome, double mutationRate) {
        if (chance(mutationRate)) genome.dmiWindow = clampInt(genome.dmiWindow + randInt(-2, 2), bounds_.dmiWindowMin, bounds_.dmiWindowMax);
        if (chance(mutationRate)) genome.atrWindow = clampInt(genome.atrWindow + randInt(-2, 2), bounds_.atrWindowMin, bounds_.atrWindowMax);
        if (chance(mutationRate)) genome.adxThreshold = clampDouble(genome.adxThreshold + randDouble(-3.0, 3.0), bounds_.adxThresholdMin, bounds_.adxThresholdMax);
        if (chance(mutationRate)) genome.atrStopMultiple = clampDouble(genome.atrStopMultiple + randDouble(-0.75, 0.75), bounds_.atrStopMultipleMin, bounds_.atrStopMultipleMax);
        if (chance(mutationRate)) genome.requireTenkanAboveKijun = !genome.requireTenkanAboveKijun;
        if (chance(mutationRate)) genome.tenkanWindow = clampInt(genome.tenkanWindow + randInt(-1, 1), bounds_.tenkanWindowMin, bounds_.tenkanWindowMax);
        if (chance(mutationRate)) genome.kijunWindow = clampInt(genome.kijunWindow + randInt(-2, 2), bounds_.kijunWindowMin, bounds_.kijunWindowMax);
        if (chance(mutationRate)) genome.spanBWindow = clampInt(genome.spanBWindow + randInt(-3, 3), bounds_.spanBWindowMin, bounds_.spanBWindowMax);
    }

    int randInt(int lo, int hi) {
        std::uniform_int_distribution<int> dist(lo, hi);
        return dist(rng_);
    }
    double randDouble(double lo, double hi) {
        std::uniform_real_distribution<double> dist(lo, hi);
        return dist(rng_);
    }
    bool coinFlip() { return randDouble(0.0, 1.0) > 0.5; }
    bool chance(double p) { return randDouble(0.0, 1.0) < p; }
    static int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
    static double clampDouble(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

    std::vector<CandleSeries> series_;
    BacktestConfig backtestConfig_;
    GenomeBounds bounds_;
    FitnessConfig fitness_;
    std::mt19937 rng_;
};

} // namespace trader::evolution
