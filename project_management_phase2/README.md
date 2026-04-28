# Project Management Pack — Phase 2 (Desktop Demo Integration)

This folder contains an execution package for the **post-batch integration gap**:
shipping a runnable Win32 desktop map wallpaper demo on Windows 11 25H2.

## Why this exists

The original 5-batch program delivered subsystem slices and gate artifacts, but
the repository still needs final integration to produce a **single runnable desktop
host executable** that renders the processed map assets live on desktop.

## Contents

- `tickets/ticket_board.csv`
  - Import-ready ticket board for Phase 2.
- `tickets/ticket_board.json`
  - Structured board payload for automation.
- `agents/agent_registry.json`
  - Agent ownership and deliverables for Phase 2.
- `agents/prompts.md`
  - Launch-ready prompts for each agent.
- `automation/cloud_agent_batches.json`
  - Parallel execution batches and gate checks.
- `automation/execution_dag.mmd`
  - Mermaid dependency graph for Phase 2.
- `automation/runbook.md`
  - Operator runbook for launch/merge/validation.
- `automation/orchestrator.py`
  - Batch readiness helper (same interface as Phase 1).

## Phase 2 objective

Deliver a **watchable desktop demo**:

1. Pipeline assets complete and validated.
2. Win32 app entrypoint (`wWinMain`) exists and boots runtime graph.
3. Desktop host attach works on Windows 11 25H2.
4. 3D map + overlays render from generated assets.
5. Input + IPC controls work without arbitration jitter.
6. Demo runbook supports repeatable live demonstration.
