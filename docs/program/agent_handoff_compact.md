# Compact Handoff for Next Agents (Low-Token Context)

This file is the shortest reliable restart context for future agents.

## 1) Current branch and PR

- Branch: `cursor/pm-automation-pack-2e4d`
- Base: `main`
- PR: `https://github.com/johnfiron/ntium/pull/6`

## 2) What is completed

### Program/Foundation

- Phase 1 foundation + contracts + subsystem scaffolds are already in repo.
- Live validation demo exists and prior intersection bug fix is landed.

### Data pipeline (Phase 2 Batch 0) — completed

- `D2-001` done: geometry hardening path in `tools/data_pipeline/run_pipeline.py`
  (make-valid / skip-failure behavior and robust clipping flow).
- `D2-002` done: non-dry artifact generation path validated.
- Produced/expected outputs for 4-state path:
  - `data/processed/4states/buildings_target.gpkg`
  - `data/processed/4states/dem_target.tif`
  - `data/processed/4states/building_height.tif`
  - `data/processed/4states/surface_dsm.tif`
  - `data/processed/4states/out/tile_manifest.json`

### App bootstrap (Phase 2 Batch 1) — completed

- `A2-001` done: `apps/wallpaper_host/main_win32.cpp` entrypoint scaffolded.
- `A2-002` done: `src/app/AppHost.*` + `src/app/RuntimeGraph.*` deterministic bootstrap/shutdown graph added.

### Merge/conflict maintenance — completed

- Fetched latest `origin/main`.
- Merged `origin/main` into feature branch.
- Resolved conflict in `tools/data_pipeline/run_pipeline.py` by keeping the
  branch's hardened implementation.
- Stopped stale looping pipeline tmux session (`pipeline-4state-full`).

## 3) What was started but not finished

These must be treated as **in-progress**, not fully closed:

1. **Phase 2 Batch 2+ full completion** is not done:
   - Remaining functional tickets include `R2-002`, controls/IPC, overlay,
     resilience, demo hardening, and final gate `G6-001`.
2. **Tracking inconsistency to resolve early next run**:
   - `project_management_phase2/tickets/ticket_board.csv` and
     `project_management_phase2/tickets/ticket_board.json` are not fully
     synchronized on some Batch 2 statuses.
   - Use JSON as source of truth (as runbook states), then reconcile CSV.
3. **Uncommitted integration edits currently present** (local WIP state):
   - `docs/program/phase2_bootstrap.md`
   - `project_management_phase2/automation/runbook.md`
   - `project_management_phase2/tickets/ticket_board.csv`
   - `project_management_phase2/tickets/ticket_board.json`
   - `src/app/AppHost.cpp`
   - `src/app/AppHost.h`

## 4) Parallel-agent execution log (important)

Recent parallel jobs were mostly data pipeline runs/watchdogs. Final observed:

- Completed successfully:
  - DEM fetch/mosaic/clip runner
  - buildings merge runner
  - one final full pipeline run (exit 0)
  - multiple watchdog loops (exit 0)
- Started but failed/interrupted:
  - multiple earlier full pipeline runs exited non-zero during hardening
    iterations before later fixes.
- No active looping data job remains after cleanup.

## 5) Maryland-first restart guidance (do this on fresh install)

For first-time setup on a fresh machine, **start with Maryland only** to reduce
disk/time and verify end-to-end quickly before 4-state expansion.

Use:

- `tools/data_pipeline/config/pipeline_config.local.example.maryland.json`
- Commands in `tools/data_pipeline/README.md` under "Maryland-first quick start"

Why this is preferred first:

- smaller OSM + DEM footprint
- faster pipeline iteration
- easier debug loop for render integration
- keeps workspace cache usage lower

After Maryland success, expand to 4-state path.

## 6) Suggested next-agent first actions

1. Reconcile `ticket_board.json` and `ticket_board.csv` status mismatch.
2. Commit current uncommitted Batch 2 WIP files as a clean checkpoint commit.
3. Finish `R2-002` (event-driven dirty-present scheduling) with explicit tests.
4. Move to Batch 3 (controls + overlay), then Batch 4 (resilience/demo gate).
5. Keep PR #6 updated with evidence artifacts per runbook gates.
