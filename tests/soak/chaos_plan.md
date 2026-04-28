# Soak + chaos reliability plan (Q1-903)

This plan defines long-run reliability validation for runtime/session/render stability.

Ticket: `Q1-903`  
Dependencies referenced by ticket: `T1-802`, `X1-703`, `R1-205`

## Goals

1. Validate **24h and 72h stability** under realistic idle/active cycling.
2. Inject required chaos events and verify deterministic recovery:
   - explorer restart
   - session reset/churn (lock/unlock, disconnect/reconnect)
   - display reset/topology churn
   - graphics device reset
3. Ensure **zero unplanned exits** and bounded recovery time.

## Required artifacts and scripts

- `tests/soak/run_soak.ps1` (or adapted equivalent) executes matrix rows and emits `summary.json`.
- Host/build under test must expose a health check command and log output path.
- Optional event injectors can be provided as scripts and passed to runner switches.

## Soak matrix

### 24h matrix (must-pass)

| Row | Duration | Workload profile | Chaos cadence | Purpose |
|---|---:|---|---|---|
| SOAK-24-A | 8h | idle dominant (90% idle, 10% interaction) | every 45 min | long idle drift / leak detection |
| SOAK-24-B | 8h | mixed (60% idle, 40% active camera/input) | every 30 min | transition robustness |
| SOAK-24-C | 8h | active bursts (30% idle, 70% active) | every 20 min | peak churn with frequent recovery |

### 72h matrix (must-pass for release gate)

| Row | Duration | Workload profile | Chaos cadence | Purpose |
|---|---:|---|---|---|
| SOAK-72-A | 24h | idle + periodic control commands | every 60 min | sustained baseline |
| SOAK-72-B | 24h | mixed usage + monitor interactions | every 30 min | session/display resilience |
| SOAK-72-C | 24h | stress profile + command burst windows | every 15 min | worst-case sustained chaos |

## Chaos event catalog (required)

Each matrix row must include all event types; cadence controls frequency.

| Event id | Event | Injection examples | Expected recovery |
|---|---|---|---|
| CHAOS-EXP-RESTART | Explorer restart | `Stop-Process explorer -Force` then `Start-Process explorer.exe` | host/session recovers without unplanned exit |
| CHAOS-SESSION-RESET | Session churn | lock/unlock, RDP disconnect/reconnect, synthetic logon transitions in lab | `SessionManager` transitions valid; active state restored |
| CHAOS-DISPLAY-RESET | Display topology reset | disable/enable display adapter path or re-enumerate display config | display binding re-established, no stuck black output |
| CHAOS-DEVICE-RESET | D3D device reset | TDR simulation / device restart tooling | renderer recovers; no crash loop |

## Deterministic pass/fail gates

Pass (exit `0`) if all true:

- `unplanned_exit_count == 0`
- `hang_count == 0`
- `event_injection_fail_count == 0`
- `health_check_fail_count == 0`
- `max_recovery_sec <= recovery_sla_sec` (default SLA `120`)
- `state_restore_fail_count == 0` after restart/session/device events
- `required_event_coverage_ok == true` (every event type executed at least once per row)

Fail (exit `1`) if any gate above fails.  
Harness error (exit `2`) for setup/config/script failures.

## Recovery assertions by event class

### Explorer restart

- wallpaper host process remains running or restarts per policy without crash loop
- desktop rebind action succeeds
- user-visible rendering resumes inside SLA

### Session reset/churn

- transitions are legal relative to `SessionManager` state model
- state persist/restore hooks complete
- runtime resumes after unlock/reconnect

### Display reset

- monitor re-enumeration succeeds
- span/mirror state remains consistent or re-applies deterministically
- no orphaned swapchain/device resources

### Device reset

- device-loss path triggers deterministic recovery sequence
- render recovers with no repeated hard failure
- command/IPC loop remains responsive

## Execution cadence and observability

Minimum telemetry/check cadence:

- health check every 30 seconds
- process liveness every 10 seconds
- memory/CPU snapshot every 60 seconds (optional but recommended)
- explicit event markers before/after each chaos injection

Minimum logs:

- soak runner log (`soak.log`)
- app/runtime log
- event injection log
- summary JSON

## Output schema (summary.json minimum)

```json
{
  "ticket": "Q1-903",
  "mode": "soak-24h",
  "rows": [
    {
      "id": "SOAK-24-A",
      "duration_sec": 28800,
      "chaos_events": {
        "CHAOS-EXP-RESTART": 10,
        "CHAOS-SESSION-RESET": 10,
        "CHAOS-DISPLAY-RESET": 10,
        "CHAOS-DEVICE-RESET": 10
      },
      "unplanned_exit_count": 0,
      "hang_count": 0,
      "health_check_fail_count": 0,
      "max_recovery_sec": 41
    }
  ],
  "totals": {
    "unplanned_exit_count": 0,
    "hang_count": 0,
    "event_injection_fail_count": 0,
    "health_check_fail_count": 0,
    "state_restore_fail_count": 0
  },
  "recovery_sla_sec": 120,
  "required_event_coverage_ok": true,
  "pass": true
}
```

## Suggested command lines

24h:

```powershell
pwsh ./tests/soak/run_soak.ps1 `
  -Mode soak-24h `
  -OutDir .\artifacts\soak\run_24h `
  -RecoverySlaSec 120 `
  -HealthCheckCommand ".\tools\health\check_runtime.ps1"
```

72h:

```powershell
pwsh ./tests/soak/run_soak.ps1 `
  -Mode soak-72h `
  -OutDir .\artifacts\soak\run_72h `
  -RecoverySlaSec 120 `
  -HealthCheckCommand ".\tools\health\check_runtime.ps1"
```

## Release gate mapping

- `soak-24h` job consumes SOAK-24-* rows.
- `soak-72h` job consumes SOAK-72-* rows.
- `chaos-matrix` coverage gate validates required event execution and recovery SLA.

