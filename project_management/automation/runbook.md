# Cloud Agent Execution Runbook

This runbook defines how to execute the program using parallel cloud agents, merge
results safely, and enforce technical gates.

## 1) Preconditions

- Feature branch exists for orchestration work.
- Contracts are not yet frozen.
- Ticket board imported from `../tickets/ticket_board.csv` or JSON equivalent.

## 2) Launch Order

Follow batches from `cloud_agent_batches.json`:

1. `batch-0-contract-freeze`
2. `batch-1-foundation`
3. `batch-2-feature-slices`
4. `batch-3-resilience-and-performance`
5. `batch-4-reliability-exit`

Do not launch a batch until all required dependencies listed in that file are
satisfied.

## 3) Branching Convention

- Branch prefix: `cursor/`
- Required suffix: `-2e4d`
- Example:
  - `cursor/a2-render-core-2e4d`
  - `cursor/a5-ingest-parser-2e4d`

Each agent owns one branch per epic unless split is necessary.

## 4) PR Rules

- One PR per active branch.
- Draft PR by default.
- PR must include:
  - Tickets resolved
  - Test evidence
  - Gate impact summary
- No merge if contract docs changed without version bump note.

## 5) Merge Strategy

1. Merge contracts first (C0) after explicit approval.
2. Merge core slices next:
   - H1, R1, S1, E1, I1
3. Merge integration slices:
   - O1, U1, M1, X1
4. Merge validation/hardening:
   - T1, Q1
5. Run final release gate review.

Use linear history if possible; otherwise rebase branch onto latest integration
branch before final merge.

## 6) Technical Gates

### Gate G0 (Contracts)
- C0-001, C0-002, C0-003 complete.

### Gate G1 (Core Vertical Slice)
- Static globe renders.
- No continuous render loop at idle.
- Basic input path works.

### Gate G2 (Overlay Vertical Slice)
- Snapshot ingest updates overlay.
- Dirty-rect updates only.
- No snapshot file -> static globe still works.

### Gate G3 (Control + Interaction)
- Named pipe commands apply with ACK/ERROR.
- User input and IPC arbitration stable.

### Gate G4 (Platform Robustness)
- Session transitions and monitor churn recover cleanly.
- Device-loss recovery validated.

### Gate G5 (Performance + Reliability Exit)
- Near-zero idle target over 10-minute window.
- Burst throttling <=10Hz presents.
- Soak/fuzz suites green.

## 7) Suggested Automation Hooks

- Trigger batch launches from your orchestration script or CI pipeline.
- Parse `cloud_agent_batches.json` for dependency checks.
- Use `ticket_board.json` as source of truth for ticket status transitions.
- Fail pipeline on gate rejection.
- Use `orchestrator.py` to list ready batches and produce launch payloads.

## 8) Reporting Template

For each batch completion report:

- Batch ID
- Agents completed
- Tickets moved to done
- Blockers
- Test evidence links
- Next batch readiness

