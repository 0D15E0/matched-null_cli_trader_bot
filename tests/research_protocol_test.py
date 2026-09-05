import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "research_protocol", ROOT / "experiments" / "research_protocol.py"
)
research = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(research)


class ResearchProtocolTests(unittest.TestCase):
    def test_valid_spec_has_no_errors(self):
        spec = {
            "id": "donchian-vt020",
            "hypothesis": "Donchian trend following improves portfolio risk-adjusted return.",
            "model": {
                "strategy": "donchian", "sparams": "entryWindow=55,exitWindow=20",
                "vol_target": 0.2, "weights": "equal", "vol_lookback": 120, "rebalance": 30,
                "hurst_filter": None, "hurst_estimator": "sf", "hurst_window": 100,
            },
            "primary_metric": "mean_excess_sharpe",
            "kill_rules": {
                "min_positive_folds": 2,
                "min_mean_excess_sharpe": 0.1,
                "min_worst_excess_sharpe": -0.2,
                "max_worst_drawdown_pct": 40.0,
            },
        }
        self.assertEqual(research.validate_spec(spec), [])

    def test_explicit_construction_and_filter_are_valid(self):
        spec = {
            "id": "tsmom-invvol-hurst",
            "hypothesis": "Momentum improves when exposure is inverse-volatility weighted and trend gated.",
            "model": {
                "strategy": "tsmom", "sparams": "lookbackWindow=120",
                "vol_target": 0.2, "weights": "invvol", "vol_lookback": 120, "rebalance": 30,
                "hurst_filter": 0.55, "hurst_estimator": "sf", "hurst_window": 200,
            },
            "primary_metric": "mean_excess_sharpe",
            "kill_rules": {
                "min_positive_folds": 2,
                "min_mean_excess_sharpe": 0.1,
                "min_worst_excess_sharpe": -0.2,
                "max_worst_drawdown_pct": 40.0,
            },
        }
        self.assertEqual(research.validate_spec(spec), [])

    def test_kill_rules_kill_a_weak_candidate(self):
        results = [
            {"excess_sharpe": 0.20, "portfolio_max_drawdown_pct": 20.0},
            {"excess_sharpe": -0.30, "portfolio_max_drawdown_pct": 25.0},
            {"excess_sharpe": -0.10, "portfolio_max_drawdown_pct": 22.0},
        ]
        status, reasons, summary = research.classify(
            results,
            {
                "min_positive_folds": 2,
                "min_mean_excess_sharpe": 0.1,
                "min_worst_excess_sharpe": -0.2,
                "max_worst_drawdown_pct": 40.0,
            },
        )
        self.assertEqual(status, "killed")
        self.assertGreaterEqual(len(reasons), 2)
        self.assertEqual(summary["positive_folds"], 1)

    def test_kill_rules_allow_a_survivor(self):
        results = [
            {"excess_sharpe": 0.20, "portfolio_max_drawdown_pct": 20.0},
            {"excess_sharpe": 0.15, "portfolio_max_drawdown_pct": 25.0},
            {"excess_sharpe": 0.05, "portfolio_max_drawdown_pct": 22.0},
        ]
        status, reasons, _ = research.classify(
            results,
            {
                "min_positive_folds": 2,
                "min_mean_excess_sharpe": 0.1,
                "min_worst_excess_sharpe": -0.2,
                "max_worst_drawdown_pct": 40.0,
            },
        )
        self.assertEqual(status, "survives_development")
        self.assertEqual(reasons, [])


if __name__ == "__main__":
    unittest.main()