#!/usr/bin/env python3
"""Performance gate checker for Batch 3 scaffolding.

Consumes either JSON or CSV-like metrics and evaluates required gates.

Input expectations:
- JSON:
  - list of objects with at least: scenario, metric, value
  - or dict of scenarios containing metric/value pairs
- CSV:
  - columns: scenario, metric, value

Exit codes:
- 0: all gates pass
- 1: one or more gates fail or required metrics missing
- 2: input/usage/parsing error
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple


REQUIRED_SCENARIOS = ("idle", "active", "burst")


@dataclass(frozen=True)
class GateRule:
    scenario: str
    metric: str
    operator: str
    threshold: float


GATE_RULES: Tuple[GateRule, ...] = (
    # Idle near-zero gates
    GateRule("idle", "idle_cpu_percent_total", "<=", 1.0),
    GateRule("idle", "idle_render_present_rate_hz", "<=", 0.5),
    GateRule("idle", "idle_pipeline_activity_per_sec", "<=", 3.0),
    # Active responsiveness gates
    GateRule("active", "active_p95_frame_ms", "<=", 16.7),
    GateRule("active", "active_p99_frame_ms", "<=", 33.3),
    GateRule("active", "ingest_queue_peak_depth", "<=", 8.0),
    GateRule("active", "control_loop_p95_ms", "<=", 4.0),
    # Burst throttling gates
    GateRule("burst", "burst_input_events", ">=", 1.0),
    GateRule("burst", "burst_throttle_ratio", ">=", 0.60),
    GateRule("burst", "ingest_queue_peak_depth", "<=", 16.0),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check perf metrics against static gates.")
    parser.add_argument("--input", required=True, help="Path to metrics file (.json or .csv)")
    parser.add_argument(
        "--output-json",
        default="",
        help="Optional path to write detailed check results as JSON",
    )
    parser.set_defaults(fail_on_missing_scenario=True)
    parser.add_argument(
        "--fail-on-missing-scenario",
        dest="fail_on_missing_scenario",
        action="store_true",
        help="Fail if a required scenario is missing (default behavior)",
    )
    parser.add_argument(
        "--allow-missing-scenario",
        dest="fail_on_missing_scenario",
        action="store_false",
        help="Do not fail solely because a required scenario is missing",
    )
    return parser.parse_args()


def to_float(value: object) -> float:
    if isinstance(value, bool):
        raise ValueError("Boolean is not a valid numeric metric")
    if isinstance(value, (int, float)):
        as_float = float(value)
    elif isinstance(value, str):
        as_float = float(value.strip())
    else:
        raise ValueError(f"Unsupported metric value type: {type(value)}")

    if math.isnan(as_float) or math.isinf(as_float):
        raise ValueError("Metric value must be finite")
    return as_float


def parse_json_metrics(path: Path) -> Dict[str, Dict[str, float]]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    by_scenario: Dict[str, Dict[str, float]] = {}

    if isinstance(raw, list):
        for entry in raw:
            if not isinstance(entry, dict):
                raise ValueError("JSON list entries must be objects")
            scenario = str(entry["scenario"])
            metric = str(entry["metric"])
            value = to_float(entry["value"])
            by_scenario.setdefault(scenario, {})[metric] = value
        return by_scenario

    if isinstance(raw, dict):
        for scenario, metrics in raw.items():
            if not isinstance(metrics, dict):
                raise ValueError(
                    "JSON object form must map scenario -> {metric: value, ...}"
                )
            parsed_metrics: Dict[str, float] = {}
            for metric, value in metrics.items():
                parsed_metrics[str(metric)] = to_float(value)
            by_scenario[str(scenario)] = parsed_metrics
        return by_scenario

    raise ValueError("Unsupported JSON structure")


def parse_csv_metrics(path: Path) -> Dict[str, Dict[str, float]]:
    by_scenario: Dict[str, Dict[str, float]] = {}

    with path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        required_cols = {"scenario", "metric", "value"}
        if not reader.fieldnames or not required_cols.issubset(set(reader.fieldnames)):
            raise ValueError("CSV must include columns: scenario,metric,value")

        for row in reader:
            scenario = str(row["scenario"])
            metric = str(row["metric"])
            value = to_float(row["value"])
            by_scenario.setdefault(scenario, {})[metric] = value

    return by_scenario


def load_metrics(path: Path) -> Dict[str, Dict[str, float]]:
    if not path.exists():
        raise FileNotFoundError(f"Metrics input not found: {path}")

    suffix = path.suffix.lower()
    if suffix == ".json":
        return parse_json_metrics(path)
    if suffix == ".csv":
        return parse_csv_metrics(path)

    raise ValueError("Unsupported input extension (expected .json or .csv)")


def compute_derived_metrics(metrics: Dict[str, Dict[str, float]]) -> None:
    """Populate optional derived values in-place when source counters are present."""

    burst = metrics.get("burst", {})
    if "burst_throttle_ratio" not in burst:
        input_events = burst.get("burst_input_events")
        throttled_events = burst.get("burst_throttled_events")
        if input_events is not None and throttled_events is not None and input_events > 0:
            burst["burst_throttle_ratio"] = throttled_events / input_events


def compare(actual: float, operator: str, threshold: float) -> bool:
    if operator == "<=":
        return actual <= threshold
    if operator == ">=":
        return actual >= threshold
    raise ValueError(f"Unsupported operator: {operator}")


def check_gates(
    metrics: Dict[str, Dict[str, float]],
    fail_on_missing_scenario: bool,
) -> Dict[str, object]:
    failures: List[Dict[str, object]] = []
    checks: List[Dict[str, object]] = []

    for scenario in REQUIRED_SCENARIOS:
        if scenario not in metrics and fail_on_missing_scenario:
            failures.append(
                {
                    "scenario": scenario,
                    "metric": "*",
                    "actual": None,
                    "operator": "exists",
                    "threshold": 1,
                    "reason": "required scenario missing",
                }
            )

    for rule in GATE_RULES:
        scenario_metrics = metrics.get(rule.scenario)
        if scenario_metrics is None:
            is_failure = fail_on_missing_scenario
            checks.append(
                {
                    "scenario": rule.scenario,
                    "metric": rule.metric,
                    "operator": rule.operator,
                    "threshold": rule.threshold,
                    "actual": None,
                    "pass": not is_failure,
                    "reason": (
                        "scenario missing"
                        if is_failure
                        else "scenario missing (ignored)"
                    ),
                }
            )
            continue

        actual = scenario_metrics.get(rule.metric)
        if actual is None:
            failure = {
                "scenario": rule.scenario,
                "metric": rule.metric,
                "actual": None,
                "operator": rule.operator,
                "threshold": rule.threshold,
                "reason": "required metric missing",
            }
            failures.append(failure)
            checks.append({**failure, "pass": False})
            continue

        passed = compare(actual, rule.operator, rule.threshold)
        check = {
            "scenario": rule.scenario,
            "metric": rule.metric,
            "actual": actual,
            "operator": rule.operator,
            "threshold": rule.threshold,
            "pass": passed,
        }
        checks.append(check)

        if not passed:
            failures.append(
                {
                    "scenario": rule.scenario,
                    "metric": rule.metric,
                    "actual": actual,
                    "operator": rule.operator,
                    "threshold": rule.threshold,
                    "reason": "metric outside threshold",
                }
            )

    return {
        "overall_pass": len(failures) == 0,
        "checks": checks,
        "failures": failures,
    }


def print_summary(result: Dict[str, object]) -> None:
    checks = result["checks"]
    assert isinstance(checks, list)

    print("Performance gate evaluation")
    print("===========================")

    for check in checks:
        if not isinstance(check, dict):
            continue
        status = "PASS" if check.get("pass") else "FAIL"
        scenario = check.get("scenario")
        metric = check.get("metric")
        actual = check.get("actual")
        op = check.get("operator")
        threshold = check.get("threshold")
        reason = check.get("reason")

        if actual is None and reason:
            print(f"[{status}] {scenario}.{metric}: {reason}")
        else:
            print(f"[{status}] {scenario}.{metric}: {actual} {op} {threshold}")

    failures = result["failures"]
    assert isinstance(failures, list)
    print("")
    print(f"Overall: {'PASS' if result['overall_pass'] else 'FAIL'}")
    print(f"Failures: {len(failures)}")


def main() -> int:
    args = parse_args()

    try:
        metrics = load_metrics(Path(args.input))
        compute_derived_metrics(metrics)
        result = check_gates(metrics, fail_on_missing_scenario=args.fail_on_missing_scenario)
    except Exception as exc:  # pragma: no cover - scaffold error path
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print_summary(result)

    if args.output_json:
        out_path = Path(args.output_json)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(result, indent=2), encoding="utf-8")

    return 0 if result["overall_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
