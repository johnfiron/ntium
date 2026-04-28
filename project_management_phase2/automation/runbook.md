# Phase 2 Runbook — First Watchable Desktop Demo

This runbook executes the **Phase 2** implementation pack that converts the
current subsystem-complete codebase into a runnable desktop wallpaper host
demonstration on Windows 11 25H2.

## 1) Preconditions

- Base branch synced and green.
- Phase 1/Batch 0..4 code merged.
- `project_management_phase2/tickets/ticket_board.json` is source of truth.
- Data assets available or generating:
  - `data/raw/aoi_4states.geojson`
  - `data/raw/buildings.gpkg`
  - `data/raw/dem.tif`

## 2) Batch launch order

Follow `automation/cloud_agent_batches.json` strictly:

1. `p2-batch-0-data-hardening`
2. `p2-batch-1-app-bootstrap`
3. `p2-batch-2-render-asset-integration`
4. `p2-batch-3-overlay-ipc-input-integration`
5. `p2-batch-4-desktop-demo-and-gates`

Do not start a batch until its dependency gate is satisfied.

## 3) Branch and PR policy

- Branch format: `cursor/<topic>-2e4d`
- Commit per logical ticket group.
- Push continuously (`git push -u origin <branch>`).
- One PR per branch; draft by default.
- Include ticket IDs and evidence in PR body.

## 4) Evidence required per gate

### P2-G0-data-ready
- Successful non-dry data pipeline run.
- Output files present:
  - `data/processed/4states/buildings_target.gpkg`
  - `data/processed/4states/dem_target.tif`
  - `data/processed/4states/building_height.tif`
  - `data/processed/4states/surface_dsm.tif`
  - `data/processed/4states/out/tile_manifest.json`
- QC report and topology reject stats.

### P2-G1-bootstrap
- Buildable desktop host app entrypoint (`wWinMain`).
- Message loop and clean startup/shutdown path.
- Runtime graph init order validation log.

### P2-G2-visible-frame
- First visible desktop frame captured on Windows host.
- Asset loader reads manifest and binds data successfully.
- Dirty-present path active (no continuous free-running loop).

### P2-G3-control-paths
- Input orbit/dolly working live.
- Pipe command ACK/ERROR validation transcripts.
- Arbitration stability proof under contention.

### P2-G4-demo-exit
- Demo checklist pass.
- Session/display churn smoke pass.
- Idle/event-driven behavior capture attached.

## 5) Demo checklist (must pass)

1. Launch `wallpaper_host.exe`.
2. Desktop attach succeeds (25H2).
3. 3D map visible.
4. Mouse orbit + dolly zoom works.
5. Pipe command changes camera/style and returns ACK.
6. Overlay event appears and updates with throttling.
7. Idle window shows no continuous redraw loop.
8. Graceful exit restores icon state ownership.

## 6) Blocking conditions

Release/demo is blocked if any are true:

- No `wWinMain` executable target exists.
- Data pipeline fails to produce required outputs.
- Desktop attach fails repeatedly on 25H2.
- Input/IPC arbitration causes control jitter/deadlock.
- Idle loop still continuously presents.

