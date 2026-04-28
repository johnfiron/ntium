# Reliability exit report template (Batch 4: Q1-901/Q1-902/Q1-903)

Use this template to produce a release-readiness reliability report that maps to:

- Q1-901 Snapshot Parser Fuzz Harness
- Q1-902 IPC Fuzz and Spam Harness
- Q1-903 Long Soak and Chaos Suite

Status labels:

- `PASS` = gate met with evidence attached
- `FAIL` = gate violated
- `BLOCKED` = test not run or incomplete evidence

---

## 1) Report metadata

- Release/build id:
- Branch/commit:
- Report date (UTC):
- Owner:
- Environment (OS/build/VM/hardware):
- Test window covered:

---

## 2) Evidence index

Attach or link the exact run artifacts:

### Snapshot fuzz (Q1-901)

- Run id:
- Command used:
- `tests/fuzz/snapshot/out/.../summary.json`
- `tests/fuzz/snapshot/out/.../failures.jsonl`
- Minimized repro corpus (if any):

### IPC fuzz/spam (Q1-902)

- Run id:
- Command used:
- `tests/fuzz/ipc/out/.../summary.json`
- `tests/fuzz/ipc/out/.../failures.jsonl`
- Frame stream reproductions (if any):

### Soak/chaos (Q1-903)

- Run id:
- Command used:
- `artifacts/soak/.../summary.json`
- `artifacts/soak/.../soak.log`
- Runtime logs:

---

## 3) Gate results (must be deterministic)

## 3.1 Q1-901 Snapshot parser fuzz

Expected from `tests/fuzz/snapshot/README.md`:

- `crash_count == 0`
- `timeout_count == 0`
- `asan_ubsan_violation_count == 0` (when enabled)
- `nondeterminism_count == 0`
- `unexpected_apply_count == 0`
- `validation_order_violation_count == 0`

Result:

- Status: `PASS | FAIL | BLOCKED`
- Summary:
- Notes/exceptions:

## 3.2 Q1-902 IPC fuzz/spam

Expected from `tests/fuzz/ipc/README.md`:

- `crash_count == 0`
- `hang_count == 0`
- `nondeterminism_count == 0`
- `unexpected_response_count == 0`
- `close_policy_violation_count == 0`
- `rate_limit_bypass_count == 0`

Result:

- Status: `PASS | FAIL | BLOCKED`
- Summary:
- Notes/exceptions:

## 3.3 Q1-903 Soak/chaos (24h + 72h)

Expected from `tests/soak/chaos_plan.md` and `tests/soak/run_soak.ps1`:

- `unplanned_exit_count == 0`
- `hang_count == 0`
- `event_injection_fail_count == 0`
- `health_check_fail_count == 0`
- `state_restore_fail_count == 0`
- `max_recovery_sec <= recovery_sla_sec`
- `required_event_coverage_ok == true`

Result:

- 24h status: `PASS | FAIL | BLOCKED`
- 72h status: `PASS | FAIL | BLOCKED`
- Summary:
- Notes/exceptions:

---

## 4) Chaos coverage checklist

Per soak row, confirm all required classes were executed:

- [ ] `CHAOS-EXP-RESTART` (explorer restart)
- [ ] `CHAOS-SESSION-RESET` (session churn/reset)
- [ ] `CHAOS-DISPLAY-RESET` (display/topology reset)
- [ ] `CHAOS-DEVICE-RESET` (device reset)

Coverage status:

- Status: `PASS | FAIL | BLOCKED`
- Missing events:

---

## 5) Defect and risk summary

Open defects discovered in Batch 4 runs:

| ID | Ticket/area | Severity | Repro available | Status | Owner |
|---|---|---|---|---|---|
| | | | | | |

Residual risks (if any):

1.
2.

---

## 6) Final exit decision

Overall reliability gate:

- Status: `PASS | FAIL | BLOCKED`

Decision notes:

- 

Approvals:

- Reliability owner:
- Runtime owner:
- Program owner:

