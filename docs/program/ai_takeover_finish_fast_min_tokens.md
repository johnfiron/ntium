# AI Takeover Script (Fast Finish, Minimal Tokens)

Copy/paste this entire block to a new AI agent.

---

You are resuming a partially completed Windows 11 25H2 live-wallpaper project.

## Output policy (STRICT)
- Keep every status update under 120 tokens.
- Format every update as:
  1) `Done:` (max 3 bullets)
  2) `Next:` (1 line)
  3) `Blockers:` (`none` or 1 line)
- Do not restate background/history unless asked.

## Goal
Finish Phase 2 to a watchable desktop demo quickly using existing code/artifacts.

## Hard constraints
- Native Win32/C++ only.
- Event-driven render path (no idle free-running loop).
- Use existing project scaffolds; do not redesign architecture.
- Use original full GIS/GDAL pipeline for MD+DE+VA+WV (not Maryland-only).
- No API keys in repo/files; env vars only.

## Repo truth sources (only these)
- `project_management_phase2/tickets/ticket_board.json` (primary status source)
- `project_management_phase2/automation/cloud_agent_batches.json` (execution order)
- `project_management_phase2/automation/runbook.md` (gate evidence)
- `docs/program/agent_handoff_compact.md` (latest compact context)

## Start sequence (exact)
1. `git checkout cursor/pm-automation-pack-2e4d`
2. `git fetch origin main && git merge origin/main`
3. `git status --short --branch`
4. If local WIP exists, create one checkpoint commit before new work.
5. Reconcile ticket status mismatch between JSON and CSV (JSON wins; then sync CSV).

## Original full GIS/GDAL pipeline (required)
Use these only if outputs are missing/stale:

1. Ensure 4-state local config:
   - `cp tools/data_pipeline/config/pipeline_config.local.example.4states.json tools/data_pipeline/config/pipeline_config.local.json` (if missing)
2. Build merged buildings:
   - `python3 tools/data_pipeline/build_4state_buildings.py --osm-dir data/raw/osm_states --out-gpkg data/raw/buildings.gpkg`
3. Fetch/clip DEM:
   - `python3 tools/data_pipeline/fetch_dem_3dep.py --aoi data/raw/aoi_4states.geojson --out-dem data/raw/dem.tif`
4. Run full pipeline:
   - `python3 tools/data_pipeline/run_pipeline.py --config tools/data_pipeline/config/pipeline_config.local.json`
5. Verify required outputs exist:
   - `data/processed/4states/buildings_target.gpkg`
   - `data/processed/4states/dem_target.tif`
   - `data/processed/4states/building_height.tif`
   - `data/processed/4states/surface_dsm.tif`
   - `data/processed/4states/out/tile_manifest.json`

## Remaining implementation order (fast path)
Execute in dependency order with minimal scope:
1. `R2-002` (dirty-present event scheduling; no idle loop)
2. `C2-001`, `C2-002`, `C2-003` (input + IPC + arbitration)
3. `O2-001`, `O2-002` (snapshot->overlay wiring + <=10Hz throttle)
4. `X2-001`, `X2-002` (session/power + device/monitor churn recovery)
5. `V2-001`, `V2-002` (demo runbook/script + perf sanity evidence)
6. `G6-001` (final watchable demo gate)

## Definition of done
- Desktop host launches on Windows path and renders first watchable 3D frame.
- Input orbit/dolly works.
- Pipe commands apply and return deterministic ACK/ERROR.
- Overlay updates render with throttle <=10Hz under burst.
- Idle behavior is event-driven (no continuous present loop).
- Session/device/monitor churn smoke tests pass.
- Runbook evidence updated and gate ticket marked done.

## Git discipline (STRICT)
- Commit by logical ticket group.
- Push after each commit: `git push -u origin cursor/pm-automation-pack-2e4d`
- Update or create PR after each iteration.
- Do not force-push.

## Anti-waste rules
- Do not broad-explore repo.
- Do not rerun expensive data pipeline if outputs already valid.
- Do not run full test suites when targeted tests exist.
- Do not write long narratives; ship code + evidence.

---

