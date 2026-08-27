#pragma once
#include "../strategy/emergent_strategy.h"
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

// A genetic-PROGRAMMING search: unlike GeneticOptimizer (evolution/
// genetic_optimizer.h), which tunes the numeric parameters of a fixed,
// hand-designed strategy formula (OdiseoStrategy), StrategyEvolver evolves
// the RULES themselves - which indicators to look at, how to compare them,
// how many rules to combine, and the entry/exit thresholds. The strategy
// "emerges" from selection pressure rather than being designed up front.
//
// Genome: EmergentGenome (see strategy/emergent_strategy.h) - a variable-
// length list of Rule (IF featureA cmp (featureB|constant) THEN weight)
// plus buy/sell thresholds.
//
// Fitness is identical in spirit to GeneticOptimizer: evaluate every
// genome against BOTH provided instruments simultaneously and take the
// MINIMUM of their scores, so evolution selects for genomes that survive
// well in both environments rather than overfitting either one.
//
// Structural operators (this is what makes it genetic PROGRAMMING rather
// than plain parameter tuning):
//   - randomGenome(): random rule COUNT (within bounds), each rule fully
//     random (feature choice, comparator, constant-or-feature, weight).
//   - crossover(): rule-by-rule uniform crossover up to the shorter
//     parent's length, then a coin-flip chance to append the longer
//     parent's remaining tail rules - so offspring can be longer or
//     shorter than either parent (genome length itself evolves).
//   - mutate(): per-rule field perturbation (weight/constant/comparator/
//     feature swap) PLUS structural mutation - a chance to add a brand
//     new random rule, or delete an existing one, each generation.
//   - Elitism: best genome always carried over unchanged.
struct StrategyGenomeBounds {
    int minRules = 2;
    int maxRules = 10;
    double weightMin = -2.0, weightMax = 2.0;
};

struct StrategyFitnessResult {
    double fitness = -1e9;
        std::vector<BacktestReport> reports;
};

class StrategyEvolver {
public:
        StrategyEvolver(std::vector<CandleSeries> series,
                                         BacktestConfig backtestConfig = {}, StrategyGenomeBounds bounds = {},
                                         FitnessConfig fitness = {},
                                         unsigned seed = std::random_device{}())
                : series_(std::move(series)), backtestConfig_(backtestConfig),
                    bounds_(bounds), fitness_(fitness), rng_(seed) {}

    // How many distinct genomes this search will evaluate. Needed to deflate
    // the winner's Sharpe: the best of 2,400 random draws against one price
    // series posts an impressive Sharpe by luck alone, and without this count
    // there is no way to tell that apart from an edge.
    static size_t trialCount(int populationSize, int generations) {
        return static_cast<size_t>(populationSize) * static_cast<size_t>(generations);
    }

    EmergentGenome evolve(int populationSize = 60, int generations = 40,
                          int tournamentSize = 4, double mutationRate = 0.2) {
        std::vector<EmergentGenome> population;
        population.reserve(populationSize);
        for (int i = 0; i < populationSize; ++i) population.push_back(randomGenome());

        EmergentGenome best = population[0];
        double bestFitness = -1e9;

        for (int gen = 0; gen < generations; ++gen) {
            std::vector<StrategyFitnessResult> results;
            results.reserve(population.size());
            for (auto& genome : population) results.push_back(evaluate(genome));

            for (size_t i = 0; i < population.size(); ++i) {
                if (results[i].fitness > bestFitness) {
                    bestFitness = results[i].fitness;
                    best = population[i];
                }
            }

            std::cout << "Generation " << (gen + 1) << "/" << generations
                       << " - best fitness so far: " << std::fixed << std::setprecision(3)
                       << bestFitness << " (" << best.rules.size() << " rules)\n";

            std::vector<EmergentGenome> next;
            next.reserve(population.size());
            next.push_back(best);

            while (next.size() < population.size()) {
                const EmergentGenome& parentA = tournamentSelect(population, results, tournamentSize);
                const EmergentGenome& parentB = tournamentSelect(population, results, tournamentSize);
                EmergentGenome child = crossover(parentA, parentB);
                mutate(child, mutationRate);
                next.push_back(std::move(child));
            }
            population = std::move(next);
        }

        return best;
    }

    StrategyFitnessResult evaluate(const EmergentGenome& genome) const {
        return evaluateOn(genome, series_, backtestConfig_);
    }

    // Evaluate a genome against an arbitrary set of environments - used to
    // score the winner on data it was never selected against (walk-forward
    // validation), which is the only measurement here with any predictive
    // claim on live results.
    StrategyFitnessResult evaluateOn(const EmergentGenome& genome,
                                      const std::vector<CandleSeries>& series,
                                      const BacktestConfig& config) const {
        StrategyFitnessResult result;
        BacktestEngine engine(config);
        result.reports.reserve(series.size());
        for (const auto& s : series) {
            EmergentStrategy strat(genome);
            result.reports.push_back(engine.run(s, strat));
        }
        result.fitness = combinedFitness(result.reports, fitness_);
        return result;
    }

    const FitnessConfig& fitnessConfig() const { return fitness_; }

private:
    Rule randomRule() {
        Rule r;
        r.featureA = randInt(0, kFeatureCount - 1);
        r.useConstant = coinFlip();
        r.featureB = randInt(0, kFeatureCount - 1);
        double lo, hi;
        constantRangeFor(r.featureA, lo, hi);
        r.constant = randDouble(lo, hi);
        r.greaterThan = coinFlip();
        r.weight = randDouble(bounds_.weightMin, bounds_.weightMax);
        return r;
    }

    EmergentGenome randomGenome() {
        EmergentGenome g;
        int count = randInt(bounds_.minRules, bounds_.maxRules);
        for (int i = 0; i < count; ++i) g.rules.push_back(randomRule());
        g.buyThreshold = randDouble(0.5, 3.0);
        g.sellThreshold = randDouble(-3.0, -0.5);
        return g;
    }

    const EmergentGenome& tournamentSelect(const std::vector<EmergentGenome>& population,
                                           const std::vector<StrategyFitnessResult>& results,
                                           int tournamentSize) {
        int bestIdx = randInt(0, static_cast<int>(population.size()) - 1);
        for (int i = 1; i < tournamentSize; ++i) {
            int idx = randInt(0, static_cast<int>(population.size()) - 1);
            if (results[idx].fitness > results[bestIdx].fitness) bestIdx = idx;
        }
        return population[bestIdx];
    }

    // Rule-by-rule uniform crossover over the shorter parent's length,
    // then possibly append the longer parent's remaining tail - lets
    // genome length itself evolve rather than being fixed.
    EmergentGenome crossover(const EmergentGenome& a, const EmergentGenome& b) {
        EmergentGenome child;
        size_t shortLen = std::min(a.rules.size(), b.rules.size());
        for (size_t i = 0; i < shortLen; ++i)
            child.rules.push_back(coinFlip() ? a.rules[i] : b.rules[i]);

        if (coinFlip()) {
            const EmergentGenome& longer = a.rules.size() > b.rules.size() ? a : b;
            for (size_t i = shortLen; i < longer.rules.size(); ++i)
                child.rules.push_back(longer.rules[i]);
        }
        if (child.rules.empty()) child.rules.push_back(randomRule());
        if (static_cast<int>(child.rules.size()) > bounds_.maxRules)
            child.rules.resize(bounds_.maxRules);

        child.buyThreshold = coinFlip() ? a.buyThreshold : b.buyThreshold;
        child.sellThreshold = coinFlip() ? a.sellThreshold : b.sellThreshold;
        return child;
    }

    void mutate(EmergentGenome& genome, double mutationRate) {
        for (auto& r : genome.rules) {
            if (chance(mutationRate)) r.featureA = randInt(0, kFeatureCount - 1);
            if (chance(mutationRate)) r.useConstant = !r.useConstant;
            if (chance(mutationRate)) r.featureB = randInt(0, kFeatureCount - 1);
            if (chance(mutationRate)) {
                double lo, hi;
                constantRangeFor(r.featureA, lo, hi);
                r.constant = clampDouble(r.constant + randDouble(-(hi - lo) * 0.2, (hi - lo) * 0.2), lo, hi);
            }
            if (chance(mutationRate)) r.greaterThan = !r.greaterThan;
            if (chance(mutationRate))
                r.weight = clampDouble(r.weight + randDouble(-0.5, 0.5), bounds_.weightMin, bounds_.weightMax);
        }

        // Structural mutation: occasionally grow or shrink the rule list,
        // so the number of rules - the strategy's "complexity" - is also
        // subject to selection, not just fixed at random-init time.
        if (chance(mutationRate * 0.5) && static_cast<int>(genome.rules.size()) < bounds_.maxRules)
            genome.rules.push_back(randomRule());
        if (chance(mutationRate * 0.5) && static_cast<int>(genome.rules.size()) > bounds_.minRules)
            genome.rules.erase(genome.rules.begin() + randInt(0, static_cast<int>(genome.rules.size()) - 1));

        if (chance(mutationRate)) genome.buyThreshold = clampDouble(genome.buyThreshold + randDouble(-0.5, 0.5), 0.1, 5.0);
        if (chance(mutationRate)) genome.sellThreshold = clampDouble(genome.sellThreshold + randDouble(-0.5, 0.5), -5.0, -0.1);
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
    static double clampDouble(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

    std::vector<CandleSeries> series_;
    BacktestConfig backtestConfig_;
    StrategyGenomeBounds bounds_;
    FitnessConfig fitness_;
    std::mt19937 rng_;
};

} // namespace trader::evolution
