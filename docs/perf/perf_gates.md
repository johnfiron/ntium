# Performance Gates for Batch 3 (T1-801, T1-802)

- **Tickets:** T1-801, T1-802
- **Purpose:** Define measurable gate criteria for render/ingest/control pipeline behavior and near-zero idle guarantees.
- **Scope:** ETW/WPR capture runs from `tools/perf/capture_wpr.ps1` and threshold checks from `tools/perf/check_thresholds.py`.

## 1) Metric model

The performance harness uses scenario-level aggregate metrics generated from ETW traces:

- `idle_cpu_percent_total` (float)
  - Process CPU usage while no input/content changes are occurring.
- `idle_render_present_rate_hz` (float)
  - Effective present cadence while idle.
- `idle_pipeline_activity_per_sec` (float)
  - Aggregate render+ingest+control activity event rate while idle.
- `active_p95_frame_ms` (float)
  - End-to-end frame latency p95 for active scenario.
- `active_p99_frame_ms` (float)
  - End-to-end frame latency p99 for active scenario.
- `burst_input_events` (int)
  - Number of control/input events injected in burst window.
- `burst_throttled_events` (int)
  - Number of events intentionally coalesced/deferred by burst throttling.
- `burst_throttle_ratio` (float)
  - Optional direct ratio; if omitted it is computed as `burst_throttled_events / burst_input_events`.
- `ingest_queue_peak_depth` (int)
  - Maximum ingest queue depth during active/burst.
- `control_loop_p95_ms` (float)
  - p95 duration of control loop tick processing.

## 2) Required scenarios

All gates are evaluated per scenario label in the metrics input:

1. **baseline**
   - Short warm baseline capture to verify instrumentation and parser integrity.
2. **idle**
   - No workload/input changes; validates near-zero idle behavior.
3. **active**
   - Sustained updates and render invalidations; validates frame latency and control stability.
4. **burst**
   - High-frequency control/input spike; validates throttling/coalescing behavior.

## 3) Gate criteria (initial scaffold values)

These values are initial defaults intended to be tightened with empirical baselines.

### 3.1 Idle near-zero gates (must pass)

- `idle_cpu_percent_total <= 1.0`
- `idle_render_present_rate_hz <= 0.5`
- `idle_pipeline_activity_per_sec <= 3.0`

Interpretation:

- Idle rendering should be event-driven only.
- Any persistent periodic present cadence at idle should fail.

### 3.2 Active responsiveness gates (must pass)

- `active_p95_frame_ms <= 16.7`
- `active_p99_frame_ms <= 33.3`
- `ingest_queue_peak_depth <= 8`
- `control_loop_p95_ms <= 4.0`

### 3.3 Burst throttling gates (must pass)

- `burst_input_events >= 1` (sanity)
- `burst_throttle_ratio >= 0.60`
  - If `burst_throttle_ratio` absent, compute from event counters.
- `ingest_queue_peak_depth <= 16` during burst

Interpretation:

- At least 60% of burst events should be coalesced/throttled.
- Queue depth must remain bounded to avoid latency collapse.

## 4) Pass/fail semantics

- A run **passes** only if all required metrics for required scenarios exist and all thresholds pass.
- Missing required metrics are treated as failures.
- Tooling should emit human-readable failures and optional JSON for CI artifact consumption.

## 5) Output contract for gate checker

`tools/perf/check_thresholds.py` should produce:

- Terminal summary of pass/fail per metric and scenario
- Non-zero exit code on failure
- Optional JSON result file with structure:

```json
{
  "overall_pass": false,
  "failures": [
    {
      "scenario": "idle",
      "metric": "idle_cpu_percent_total",
      "actual": 1.42,
      "operator": "<=",
      "threshold": 1.0,
      "reason": "metric above threshold"
    }
  ]
}
```

## 6) Evolution guidance

Follow-up tuning tickets should:

- Replace scaffold defaults with percentile-based budgets from production-like traces.
- Split thresholds by hardware class where justified.
- Version the gate table if metric definitions change.
