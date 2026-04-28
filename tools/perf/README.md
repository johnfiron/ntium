# Perf Harness Scaffolding (Batch 3)

This directory contains scaffolding for T1-801/T1-802 performance instrumentation and gating.

## Files

- `capture_wpr.ps1`
  - PowerShell harness to capture ETW traces for baseline/idle/active/burst scenarios.
- `check_thresholds.py`
  - Threshold checker that evaluates metrics JSON/CSV against gate criteria.

See also:

- `docs/perf/etw_event_schema.md`
- `docs/perf/perf_gates.md`

## 1) Capturing ETW traces with WPR

### Prerequisites

- Windows machine with WPR (`wpr.exe`) available.
- Application runtime instrumented with ETW events described in `etw_event_schema.md`.

### Example commands

Capture all scenarios (interactive prompts):

```powershell
./tools/perf/capture_wpr.ps1 `
  -Scenario all `
  -OutputDir .\\artifacts\\perf `
  -Profile GeneralProfile `
  -SessionTag local_run
```

Capture burst only, non-interactive:

```powershell
./tools/perf/capture_wpr.ps1 `
  -Scenario burst `
  -OutputDir .\\artifacts\\perf `
  -SkipUserPrompt `
  -BurstDurationSec 30
```

Outputs:

- One `.etl` per scenario: `wpr_<scenario>_<timestamp>_<tag>.etl`
- `capture_manifest.json` containing trace metadata

## 2) Producing metrics

An ETW parser (not included in this scaffold) should convert traces into one of:

- JSON metrics
- CSV metrics

Accepted normalized formats for `check_thresholds.py`:

### JSON option A (scenario map)

```json
{
  "idle": {
    "idle_cpu_percent_total": 0.7,
    "idle_render_present_rate_hz": 0.1,
    "idle_pipeline_activity_per_sec": 1.2
  },
  "active": {
    "active_p95_frame_ms": 14.3,
    "active_p99_frame_ms": 25.8,
    "ingest_queue_peak_depth": 4,
    "control_loop_p95_ms": 2.8
  },
  "burst": {
    "burst_input_events": 200,
    "burst_throttled_events": 150,
    "ingest_queue_peak_depth": 10
  }
}
```

### JSON option B (row list)

```json
[
  {"scenario": "idle", "metric": "idle_cpu_percent_total", "value": 0.7},
  {"scenario": "idle", "metric": "idle_render_present_rate_hz", "value": 0.1}
]
```

### CSV

```csv
scenario,metric,value
idle,idle_cpu_percent_total,0.7
idle,idle_render_present_rate_hz,0.1
burst,burst_input_events,200
burst,burst_throttled_events,150
```

## 3) Running threshold checks

```bash
python3 tools/perf/check_thresholds.py \
  --input artifacts/perf/metrics.json \
  --output-json artifacts/perf/gate_results.json
```

Behavior:

- Exit code `0` on pass
- Exit code `1` on threshold/missing-metric failure
- Exit code `2` on parsing/usage errors

## 4) Gate focus in Batch 3

- **Near-zero idle:** CPU, present cadence, and pipeline activity stay close to zero.
- **Burst throttling:** Burst input is meaningfully coalesced (`>= 60%` throttled) while queue depth remains bounded.

Gate values are defined in `docs/perf/perf_gates.md` and can be tuned in follow-up tickets.
