# Release Exit Gate Consolidation (G5-001)

This document is the final release-gate consolidation for Batch 4 ticket **G5-001**.
It unifies gate criteria **G0..G5**, defines evidence and signoff requirements, and
provides deterministic release-blocking rules.

## 1) Release policy (non-negotiable)

Release is allowed only when all of the following are true:

1. Every gate **G0, G1, G2, G3, G4, G5** is marked **PASS**.
2. All required evidence artifacts for each gate are present and reviewed.
3. All required owner signoffs are present.
4. No deterministic blocker rule in Section 5 is triggered.

If any single item above is false, release state is **BLOCKED**.

---

## 2) Gate-to-ticket and automation mapping

| Gate | Scope | Must-pass checks (summary) | Ticket IDs (traceability) | Automation hooks |
|---|---|---|---|---|
| G0 | Contracts freeze | Contracts frozen, versioned, and validated | C0-001, C0-002, C0-003 | `project_management/automation/orchestrator.py` (`G0-contracts`) |
| G1 | Core vertical slice | Desktop attach/render/camera/ingest/ipc foundation healthy | H1-101, R1-202, S1-302, E1-402, I1-502 | `orchestrator.py` (`G1-foundation`) |
| G2 | Overlay vertical slice | Snapshot->overlay updates, dirty-rect path, no-snapshot fallback | R1-203, O1-403, O1-404, O1-405, S1-303, U1-504, M1-602 | `orchestrator.py` (`G2-live-overlay`) |
| G3 | Control + interaction | IPC ACK/ERROR control and user-vs-IPC arbitration stable | I1-501, I1-502, U1-503, U1-504, O1-405 | `orchestrator.py` (ticket done checks), gate-review checklist |
| G4 | Platform robustness | Session/display churn and device-loss recovery clean | H1-103, R1-205, M1-601, M1-602, X1-703 | `orchestrator.py` (`G3-operational-readiness`) + soak/chaos outputs |
| G5 | Perf + reliability exit | Near-zero idle, throttle limits, fuzz/soak pass, final review | T1-801, T1-802, Q1-901, Q1-902, Q1-903, O1-405, M1-602, G5-001 | `tools/perf/check_thresholds.py`, `orchestrator.py` (`G5-release-exit`), soak/fuzz reports |

Notes:
- `orchestrator.py` is the source for dependency-ready and `required_tickets_done` gate checks from `cloud_agent_batches.json`.
- The perf checker consumes metrics artifacts and emits fail-fast pass/fail output plus JSON details.

---

## 3) Explicit pass/fail checklists (G0..G5)

Each gate must include exactly one outcome: **PASS** or **FAIL**.

### G0 — Contracts

**Gate result:** [ ] PASS  [ ] FAIL

- [ ] C0-001 done: `docs/contracts/event_snapshot_v1.md` frozen with version + limits.
- [ ] C0-002 done: `docs/contracts/ipc_pipe_v1.md` frozen with ACK/ERROR semantics.
- [ ] C0-003 done: `docs/contracts/render_invalidation_api.md` frozen with threading ownership.
- [ ] Contract test vectors/negative cases executed and attached.

### G1 — Core vertical slice

**Gate result:** [ ] PASS  [ ] FAIL

- [ ] Static globe render path is functional (host attach + swapchain + camera baseline).
- [ ] No continuous render loop at idle under foundation scenario.
- [ ] Snapshot parser and IPC foundation are operational.
- [ ] Required tickets done: H1-101, R1-202, S1-302, E1-402, I1-502.

### G2 — Overlay vertical slice

**Gate result:** [ ] PASS  [ ] FAIL

- [ ] Snapshot ingest updates overlay deterministically.
- [ ] Dirty-rect updates are used for incremental updates.
- [ ] No snapshot file fallback: static globe path remains operational.
- [ ] Burst overlay updates respect coalescing/throttle policy (`<= 10 Hz` present behavior).
- [ ] Required tickets done: R1-203, O1-403, O1-404, O1-405, S1-303, U1-504, M1-602.

### G3 — Control + interaction

**Gate result:** [ ] PASS  [ ] FAIL

- [ ] Named pipe commands apply with deterministic ACK/ERROR responses.
- [ ] User input and IPC arbitration is stable (no jitter/focus deadlock).
- [ ] Security checks for same-user/same-session enforcement are validated.
- [ ] Required tickets done: I1-501, I1-502, U1-503, U1-504, O1-405.

### G4 — Platform robustness

**Gate result:** [ ] PASS  [ ] FAIL

- [ ] Session transitions (lock/unlock/logoff/logon/suspend/resume) recover correctly.
- [ ] Monitor topology churn (hotplug/rotation/DPI change) recovers without corruption.
- [ ] Device removed/reset recovery path is validated and bounded.
- [ ] Required tickets done: H1-103, R1-205, M1-601, M1-602, X1-703.

### G5 — Performance + reliability exit

**Gate result:** [ ] PASS  [ ] FAIL

- [ ] `tools/perf/check_thresholds.py` returns exit code `0`.
- [ ] Idle near-zero thresholds pass (10-minute window):
  - `idle_cpu_percent_total <= 1.0`
  - `idle_render_present_rate_hz <= 0.5`
  - `idle_pipeline_activity_per_sec <= 3.0`
- [ ] Active responsiveness thresholds pass:
  - `active_p95_frame_ms <= 16.7`
  - `active_p99_frame_ms <= 33.3`
  - `ingest_queue_peak_depth <= 8`
  - `control_loop_p95_ms <= 4.0`
- [ ] Burst throttling thresholds pass:
  - `burst_input_events >= 1`
  - `burst_throttle_ratio >= 0.60`
  - `ingest_queue_peak_depth <= 16`
- [ ] Fuzz suites pass: Q1-901 + Q1-902.
- [ ] Soak + chaos suites pass: Q1-903 (24h and 72h campaigns).
- [ ] Required tickets done: Q1-901, Q1-902, Q1-903, O1-405, M1-602, G5-001.

---

## 4) Required evidence artifacts and owner signoff

| Gate | Required evidence artifacts | Required signoff owners |
|---|---|---|
| G0 | `docs/contracts/event_snapshot_v1.md`, `docs/contracts/ipc_pipe_v1.md`, `docs/contracts/render_invalidation_api.md`, contract validation notes | **A0** (primary), plus contract contributors (**A2/A4/A5/A6/A7**) |
| G1 | `docs/platform/desktop_host_foundation.md`, `docs/render/render_foundation.md`, `docs/scene/camera_foundation.md`, `docs/ingest/ingest_foundation.md`, `docs/ipc/ipc_foundation.md`, foundation test log bundle | **A1, A2, A3, A5, A6** |
| G2 | `docs/render/render_feature_slice_batch2.md`, `docs/overlay/overlay_feature_slice_batch2.md`, `docs/input/input_feature_slice_batch2.md`, `docs/platform/monitor_feature_slice_batch2.md`, feature slice test bundle | **A2, A3, A4, A7, A8** |
| G3 | IPC protocol validation logs, arbitration stress results, ACK/ERROR transcript samples | **A6, A7** (A4 co-sign for throttle behavior) |
| G4 | `docs/runtime/resilience_feature_slice_batch3.md`, session/display transition matrix results, device-loss recovery logs, monitor churn logs | **A1, A2, A8, A9** |
| G5 | `docs/perf/perf_gates.md`, `artifacts/perf/metrics.(json\|csv)`, `artifacts/perf/gate_results.json`, fuzz reports (Q1-901/Q1-902), soak/chaos reports (Q1-903), final gate review checklist | **A10, A11**, final release authority **A0** |

Minimum artifact expectations for reliability evidence:

- Fuzz: corpus summary, crash count, leak/sanitizer status, repro seeds.
- Soak: 24h and 72h run summaries, unplanned exits count, resource trend summaries.
- Chaos: explorer restart, display changes, session cycles, device reset injections with pass/fail matrix.

---

## 5) Deterministic release blocking rule (threshold regressions)

Release is **automatically blocked** when any rule below is true.

### 5.1 Hard gate failures

1. Any gate checklist item in Section 3 is unchecked or marked FAIL.
2. Any required artifact in Section 4 is missing.
3. Any required signoff is missing.
4. `orchestrator.py` indicates any gate's `required_tickets_done` is unsatisfied.

### 5.2 Performance threshold regression rule

Given current run metrics `M_current` and approved baseline `M_base` for the same scenario/metric:

- For `<=` metrics (upper bound):
  - Block if `M_current > threshold`, or
  - Block if `M_current > M_base * 1.10` (more than 10% regression), even if threshold is not yet crossed.
- For `>=` metrics (lower bound):
  - Block if `M_current < threshold`, or
  - Block if `M_current < M_base * 0.90` (more than 10% regression), even if threshold is not yet crossed.

Deterministic tie-breaking:

- Missing metric, NaN/inf, or missing scenario is treated as FAIL.
- If both threshold and baseline checks are available, both must pass.
- Comparator semantics are exact (`<=` and `>=` inclusive).

### 5.3 Reliability regression rule

Block release if any of the following are true:

- Any unplanned exit in required soak windows (24h or 72h).
- Any reproducible crash in fuzz campaigns without approved mitigation waiver.
- Unbounded resource trend (monotonic growth without plateau) flagged in soak report.

---

## 6) Gate execution order and operator hooks

1. Run dependency readiness:
   - `python3 project_management/automation/orchestrator.py --format json`
2. Run perf checker with generated metrics:
   - `python3 tools/perf/check_thresholds.py --input artifacts/perf/metrics.json --output-json artifacts/perf/gate_results.json`
3. Collect reliability outputs:
   - Fuzz outputs (Q1-901/Q1-902)
   - Soak/chaos outputs (Q1-903)
4. Complete gate checklist/signoff and attach evidence bundle.
5. Issue final readiness declaration.

---

## 7) Final readiness declaration template

```markdown
# Release Readiness Declaration

- Release candidate: <version-or-tag>
- Commit SHA: <sha>
- Date (UTC): <yyyy-mm-ddThh:mm:ssZ>
- Environment: <lab/ci profile>

## Gate outcomes
- G0: PASS | FAIL
- G1: PASS | FAIL
- G2: PASS | FAIL
- G3: PASS | FAIL
- G4: PASS | FAIL
- G5: PASS | FAIL

## Evidence bundle
- Orchestrator output: <path-or-link>
- Perf metrics input: <path-or-link>
- Perf gate results (`gate_results.json`): <path-or-link>
- Fuzz report bundle: <path-or-link>
- Soak 24h report: <path-or-link>
- Soak 72h report: <path-or-link>
- Chaos matrix report: <path-or-link>

## Deterministic blocker check
- Threshold regressions >10% vs approved baseline: YES | NO
- Any threshold hard fail: YES | NO
- Missing required artifacts/signoffs: YES | NO

## Signoffs
- A10 (Performance): <name/date>
- A11 (Reliability): <name/date>
- A0 (Final release authority): <name/date>

## Declaration
I confirm all required release gates (G0..G5) are PASS, all evidence is attached,
and no deterministic blocker condition is active.
```

