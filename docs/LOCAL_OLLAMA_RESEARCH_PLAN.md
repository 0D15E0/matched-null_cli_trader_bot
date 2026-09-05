# Local Ollama Research Supervisor Plan

## Purpose

Build a continuously running, local-only research supervisor that uses Ollama
to propose and review trading hypotheses while the repository's deterministic
backtester performs the actual experiments.

The supervisor should not stop after finding one attractive result. It should
maintain a durable record of the current best candidates, continue exploring
new hypotheses, learn from killed candidates, and periodically re-evaluate the
frontier under the existing research protocol.

The LLM is the research assistant. It is not the source of truth for metrics,
causality, dates, fills, or deployment decisions.

## Current machine and repository context

Observed on the development Mac:

- Apple model: `Mac16,8`
- CPU: 12 cores
- Memory: 51.2 GB
- Ollama client: `0.32.14`
- Ollama server: installed but not currently running
- Repository binary: `build/cli_trader`
- Existing registry: `experiments/hypotheses.json`
- Existing protocol: `experiments/research_protocol.py`
- Existing fixed folds: 2018-2019, 2020-2021, and 2022-2023
- Frozen development boundary: `2023-12-31`
- Holdout/forward data: `2024-01-01` onward

The worktree contains unrelated current changes. The implementation phase must
inspect and preserve them rather than assuming a clean branch.

## Non-negotiable rules

### Local-only execution

All model inference must use the local Ollama HTTP service:

```text
http://127.0.0.1:11434
```

The supervisor must not call OpenAI, Anthropic, Google, hosted inference,
remote embeddings, or any other cloud model. It should fail closed if the
configured Ollama endpoint is not loopback or if an unexpected network client
is introduced.

The backtester may use local data only. Data fetching is a separate explicit
operation and is not part of the autonomous research loop.

### Holdout isolation

The autonomous loop may use only data ending at `2023-12-31`. It must not be
given a command-line date, data directory, or arbitrary shell access that could
reach 2024+ data.

The 2024+ period is reserved for a human-authorized validation run. The local
supervisor must have no command that performs that validation.

### Deterministic evaluation

The LLM must never calculate or invent performance metrics. Every reported
metric must come from a completed deterministic command, with:

- causal warm-up bars;
- next-open fills;
- configured fees and slippage;
- the fixed chronological development folds;
- the equal-weight buy-and-hold benchmark;
- an exact command record;
- a captured binary version and source revision;
- a registry record that can be replayed.

### No direct deployment

The supervisor must not edit `deploy/pi/bot.env`, live state, systemd files,
or exchange credentials. It must not place orders. A human must explicitly
review and authorize any holdout test or deployment.

### Endless does not mean uncontrolled

The process may run indefinitely, but every iteration must have a timeout,
checkpoint, resource limit, and durable result. A crash or restart must resume
from the registry without losing the incumbent or re-running an experiment
under a different interpretation.

## Target architecture

```text
                    localhost only

  Ollama generator model  <---->  Research supervisor
          |                              |
          | structured proposal          | validates and schedules
          v                              v
      proposal.json              isolated candidate workspace
                                         |
                                         v
                              C++ build + causality check
                                         |
                                         v
                              fixed development evaluator
                                         |
                                         v
                         result.json + stdout/stderr + manifest
                                         |
                                         v
                              registry and Pareto frontier
                                         |
                                         v
                             Ollama reviewer model
```

The supervisor should be a small Python process using the standard library
where practical. It should call Ollama's local `/api/chat` endpoint over HTTP,
invoke only allowlisted repository commands, and write atomic JSON artifacts.
The research engine remains the existing C++ executable and Python protocol.

## Ollama setup

### Installation and service

Ollama is already installed on this Mac, but the server is not running. The
initial setup should be performed manually and verified before automation:

```sh
ollama serve
```

Keep the server bound to loopback. Do not expose port `11434` to the LAN or
the public internet. The supervisor should verify `/api/tags` before starting
and record the selected model name and Ollama version in every run manifest.

### Model strategy

Use two local model roles rather than one oversized model for every iteration:

1. **Generator:** a smaller coding/reasoning model that proposes one
   structured hypothesis per iteration. It should be fast enough to keep the
   experiment queue occupied without exhausting memory.
2. **Reviewer:** a larger local model used only for survivors, repeated
   failures, novelty checks, and frontier reviews.

Do not hard-code a model name into the protocol until it has been benchmarked
on this Mac. Test installed or locally pullable open-weight models in the
roughly 7B-14B range first, then compare a larger quantized model for review.
The useful measurement is proposals per hour, valid-schema rate, duplicate
rate, and reviewer agreement with deterministic classification, not model
benchmark scores.

Model licenses must be checked separately. “Runs through Ollama” does not by
itself mean that every model has the same redistribution or commercial-use
terms.

### Resource policy

The 51.2 GB machine can support a local model and native backtests, but memory
should not be treated as unlimited:

- Keep one generation model loaded during normal operation.
- Load the reviewer only for review batches, or use a separate bounded queue.
- Start with one LLM request and one backtest worker at a time.
- Increase backtest concurrency only after measuring memory pressure and
  thermal throttling.
- Set Ollama keep-alive deliberately so an idle loop does not consume memory
  forever.
- Record process duration and peak resident memory where available.
- Do not run simultaneous large-model inference and a large tournament until a
  measured resource budget supports it.

The first objective is reliable continuous research, not maximum parallelism.

## Immutable research mission

Create a versioned mission document or JSON file that the supervisor loads at
startup and refuses to modify. It should contain:

```yaml
mission_id: local-trend-discovery-v1
data_policy:
  development_end: 2023-12-31
  holdout_start: 2024-01-01
  allowed_data_dirs: [data, data/derived]
  forbidden_data_patterns: ["2024", "2025", "2026", "holdout", "state_live"]
universe:
  symbols: [BTC_USDT, ETH_USDT, XRP_USDT, LTC_USDT, DOGE_USDT, TRX_USDT, ADA_USDT, SOL_USDT]
  periods: [1800, 3600, 7200, 14400, 86400]
evaluation:
  folds:
    - [2018-01-01, 2019-12-31]
    - [2020-01-01, 2021-12-31]
    - [2022-01-01, 2023-12-31]
  warmup_bars: 80
  primary_metric: mean_excess_sharpe
  required_metrics: [cagr, sharpe, excess_sharpe, sortino, max_drawdown]
limits:
  max_parameters: 12
  max_candidate_runtime_seconds: 900
  max_invalid_proposals_in_a_row: 20
  max_duplicate_proposals_in_a_row: 20
permissions:
  allow_holdout: false
  allow_deployment: false
  allow_source_edits: false
```

The actual format may be JSON or YAML, but the supervisor must validate it at
startup and include its content hash in every candidate manifest. Any mission
change creates a new mission ID and a new experiment lineage.

## Candidate contract

The LLM should produce exactly one JSON proposal per iteration. It must not
produce shell commands, Python source, arbitrary file paths, or prose in place
of required fields.

Example:

```json
{
  "proposal_id": "generated-by-supervisor",
  "hypothesis": "A slower breakout with a volatility-ranked entry gate improves worst-fold excess Sharpe without increasing drawdown.",
  "strategy": "donchian",
  "sparams": "entryWindow=80,exitWindow=25,atrWindow=20,atrStopMult=3.0",
  "vol_target": 0.20,
  "weights": "equal",
  "vol_lookback": 120,
  "rebalance": 30,
  "timeframes": [14400],
  "reasoning": "The candidate changes one structural idea from the current frontier and remains within the Donchian family bounds.",
  "expected_failure_mode": "It may enter too late and miss fast reversals.",
  "novelty_key": "donchian:slower-entry:wider-stop"
}
```

Validation must reject:

- unknown strategy families;
- unknown or out-of-range parameters;
- more than the mission's parameter limit;
- dates or data directories from the holdout;
- shell commands or paths in model output;
- missing hypothesis, failure mode, or novelty key;
- duplicate candidates or semantically equivalent parameter sets;
- bespoke flags mixed with registry `sparams`;
- candidates that change the benchmark or fill model;
- strategies that cannot pass the causality tool.

The supervisor, not the model, constructs the executable command from the
validated fields.

## Prompt design

Every generator prompt should contain compact machine-produced context:

- immutable mission ID and rules;
- current incumbent and its complete fold metrics;
- Pareto frontier members;
- recently killed candidates and exact kill reasons;
- strategy families and legal parameter bounds;
- recent unexplored family/timeframe combinations;
- resource and runtime limits;
- the instruction to propose exactly one JSON object.

Do not send the entire repository or the entire conversation history on every
iteration. Retrieve focused source snippets or family metadata when needed.
Long histories encourage the model to repeat its own assumptions and increase
local inference cost.

Use a second prompt for review. The reviewer receives the candidate manifest,
deterministic fold outputs, complexity information, and comparable frontier
members. It may annotate the result, but it cannot override the protocol's
classification.

## Durable state

The registry should eventually move from a mutable, human-oriented JSON file to
a small SQLite database, while preserving JSON export for inspection. The
database should have at least these tables:

### `missions`

- mission ID and content hash;
- development boundary and fold definition;
- allowed data directories;
- created timestamp;
- model and supervisor versions.

### `candidates`

- candidate ID;
- normalized proposal JSON and hash;
- hypothesis text;
- strategy and parameters;
- mission ID;
- status: `proposed`, `running`, `killed`, `survives_development`,
  `frontier`, `review_pending`, or `invalid`;
- parent candidate IDs, if any;
- created and completed timestamps.

### `runs`

- candidate ID and fold;
- exact argv array;
- source revision and binary hash;
- stdout/stderr artifact paths;
- exit code and timeout;
- parsed metrics;
- data-store manifest;
- start/end timestamps.

### `frontier`

- candidate ID;
- mean excess Sharpe;
- worst-fold excess Sharpe;
- worst drawdown;
- CAGR;
- complexity;
- correlation to the incumbent, when measured;
- reason for entering or leaving the frontier.

### `events`

Append-only supervisor events such as proposal rejection, process restart,
Ollama failure, candidate timeout, registry checkpoint, and human pause.

Every state transition should be atomic. A candidate marked `running` without a
completed result must be recoverable as `interrupted` after a restart, never
silently treated as successful.

## Endless search loop

The supervisor loop should follow this lifecycle:

1. Acquire a single-instance lock.
2. Verify the mission hash and local Ollama health.
3. Recover interrupted candidates and stale leases.
4. Summarize the incumbent, frontier, failures, and unexplored search space.
5. Ask the generator for one structured proposal.
6. Validate and canonicalize the proposal.
7. Reject invalid or duplicate proposals without running them.
8. Register the candidate as `running` with a lease and exact manifest.
9. Run build, causality, and fixed development folds in an isolated workspace.
10. Parse metrics only from known output formats.
11. Apply deterministic kill rules.
12. Ask the reviewer only for candidates that survive or reveal a repeated
    failure pattern.
13. Update the incumbent and Pareto frontier.
14. Checkpoint all state and append an event.
15. Continue to the next proposal.

Pseudocode:

```python
while not shutdown_requested:
    recover_interrupted_runs()
    verify_mission_and_ollama()

    context = registry.compact_context(
        incumbent=True,
        frontier=True,
        recent_failures=50,
        unexplored_regions=True,
    )
    proposal = ollama_generate_json(mission, context)

    candidate = validate_and_canonicalize(proposal, mission)
    if candidate.invalid:
        registry.record_invalid(candidate)
        continue
    if registry.is_duplicate(candidate):
        registry.record_duplicate(candidate)
        continue

    run = evaluator.run_development_only(candidate, mission)
    classification = protocol.classify(run)
    registry.commit(candidate, run, classification)

    if classification in {"survives_development", "frontier_candidate"}:
        review = ollama_review_json(candidate, run, frontier)
        registry.record_review(review)

    checkpoint()
```

The loop should support graceful shutdown after the current candidate and a
resume command after restart. It should not require an active chat session.

## Search strategy for the LLM

The generator should not always optimize the current best score. Use a staged
mixture of search modes, recorded in the registry:

- **Family exploration:** try registered families not recently tested.
- **Local mutation:** change one or two parameters around a surviving
  candidate, creating a new hypothesis ID.
- **Structural alternatives:** compare equal versus inverse-volatility
  construction, timeframes, and regime gates under predeclared rules.
- **Robustness search:** seek simpler candidates with similar performance.
- **Diversification search:** seek candidates whose returns are weakly
  correlated with the incumbent, not merely candidates with a higher Sharpe.
- **Failure-directed search:** use recurring kill reasons to avoid known dead
  regions or test a clearly different mechanism.
- **Control proposals:** periodically run always-long, random-entry, and
  buy-and-hold controls to detect evaluation drift.

The supervisor should enforce quotas, for example 40% family exploration, 25%
local mutation, 20% robustness/diversification, and 15% controls. The LLM may
choose within a quota, but it should not spend forever mutating one apparent
winner.

## Incumbent and Pareto frontier

Do not keep only one “best strategy.” Maintain a frontier across at least:

- mean excess Sharpe;
- worst-fold excess Sharpe;
- CAGR;
- maximum drawdown;
- complexity;
- correlation to the incumbent.

A candidate can be valuable because it has lower drawdown, simpler rules, or
low correlation to the current strategy even if its raw Sharpe is lower.

The incumbent is a research incumbent, not a live deployment recommendation.
The live strategy remains unchanged until a separate human-controlled process
reviews development evidence and authorizes a frozen forward test.

## Kill and promotion rules

The deterministic protocol remains the authority. A baseline first version can
use the existing registry rules:

- minimum positive-fold count;
- minimum mean excess Sharpe;
- minimum worst-fold excess Sharpe;
- maximum worst-fold drawdown;
- successful build and causality check.

Add these autonomous-loop rules:

- invalid schema: kill immediately;
- duplicate normalized candidate: do not run;
- timeout or crash: kill the run and retain the failure artifact;
- no improvement after a predeclared number of mutations: return to family
  exploration;
- too many repeated invalid proposals: pause and require operator review;
- any holdout access attempt: stop the supervisor and create a security event.

“Survives development” means eligible for review only. It does not permit
holdout access, deployment, or replacing the incumbent.

## Isolation and process controls

The supervisor should use:

- a dedicated local user or macOS launch agent where practical;
- a repository worktree or temporary candidate directory per run;
- an allowlisted executable command builder;
- no `shell=True` subprocess calls;
- fixed environment variables and explicit working directories;
- per-process timeouts;
- bounded stdout/stderr capture;
- atomic result writes;
- a lock file or SQLite lease to prevent duplicate supervisors;
- automatic cleanup only after artifacts are committed;
- a read-only copy of the mission and holdout policy.

The first implementation should not let the LLM edit C++ files. It should use
existing strategy families and parameters only. Source-generating candidates
can be added later in isolated worktrees after the structured-search loop is
proven reliable.

## Observability

Each iteration should produce a human-readable event line and a machine-readable
manifest. Track:

- iteration number;
- candidate ID and proposal hash;
- Ollama model, version, and latency;
- prompt/schema validation outcome;
- command, source revision, binary hash, and data manifest;
- per-fold metrics and kill reason;
- memory/runtime information;
- frontier changes;
- restart and recovery events.

Provide a read-only status command that shows:

- current supervisor heartbeat;
- current candidate and lease expiry;
- incumbent and frontier;
- candidates completed, killed, invalid, and duplicated;
- last Ollama error;
- last checkpoint;
- holdout policy status.

The status command must not trigger experiments.

## Phased implementation

### Phase 0: manual local model check

Goal: prove Ollama works locally before writing a daemon.

Tasks:

1. Start `ollama serve` bound to loopback.
2. Pull or load one suitable local open-weight model.
3. Call `/api/tags` and `/api/chat` with a tiny JSON-schema proposal task.
4. Measure response latency, valid JSON rate, and memory behavior.
5. Record the model name and license in a local operator note.

Exit condition: 20 consecutive valid structured responses with no network
request outside loopback.

### Phase 1: proposal-only supervisor

Goal: let the LLM propose candidates without executing them.

Tasks:

1. Define the immutable mission file.
2. Build proposal validation and canonicalization.
3. Add duplicate detection.
4. Store proposals and rejection reasons durably.
5. Generate a compact frontier context from the existing registry.

Exit condition: the supervisor can restart and continue without losing state.

### Phase 2: one-candidate evaluator

Goal: run one validated proposal deterministically.

Tasks:

1. Build commands only from validated fields.
2. Use the existing fixed development folds.
3. Capture exact artifacts and parse metrics.
4. Apply existing kill rules.
5. Add timeout, lease recovery, and atomic registry updates.

Exit condition: a manually selected candidate produces the same result through
the supervisor and through a direct command.

### Phase 3: bounded continuous loop

Goal: run unattended for hours or days.

Tasks:

1. Add one-candidate-at-a-time looping.
2. Add quotas across exploration modes.
3. Add heartbeat and status output.
4. Add graceful shutdown and restart recovery.
5. Keep the reviewer disabled or restricted to survivors.

Exit condition: a 24-hour local run has no lost candidates, duplicate state
corruption, holdout access, or uncontrolled memory growth.

### Phase 4: reviewer and frontier analysis

Goal: use a second local model to interpret deterministic evidence.

Tasks:

1. Ask for structured review, not a replacement classification.
2. Add complexity and novelty assessments.
3. Add correlation-to-incumbent analysis.
4. Test whether reviewer comments predict later deterministic failures.

Exit condition: reviewer annotations are useful metadata but never override
protocol decisions.

### Phase 5: human-controlled forward validation

Goal: evaluate a small number of frozen survivors on 2024+ data.

This phase must be outside the autonomous loop. A human freezes the candidate
ID, parameters, binary revision, and evaluation command, then runs the forward
test separately. The result is imported as read-only evidence. No model gets
permission to alter the candidate after holdout access.

## Practical first milestone

The first useful implementation should be deliberately modest:

```text
one local Ollama model
one generator prompt
one JSON proposal schema
existing registered strategies only
one candidate at a time
fixed 2018-2023 development folds
existing hypotheses registry plus append-only artifacts
no source edits
no holdout access
no deployment access
```

Run it for 24 hours, inspect its invalid/duplicate rate and frontier quality,
then decide whether adding a reviewer model or source-generating candidates is
worth the additional complexity.

## Success criteria

The project is successful when the supervisor can run unattended and still
answer these questions exactly after a restart:

1. What mission and data boundary governed this candidate?
2. Which local model proposed it, and what did it output?
3. What exact executable command evaluated it?
4. Which source revision and binary were used?
5. Which folds passed or failed?
6. Why was it killed or added to the frontier?
7. What was the last best candidate at the time?
8. Did the loop ever access holdout data or deployment files?

If those answers cannot be reconstructed from local artifacts, the loop is not
ready to run forever.
