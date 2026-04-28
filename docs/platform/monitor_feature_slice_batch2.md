# Monitor Feature Slice Batch 2 (M1-601 / M1-602)

## Scope

This batch introduces:

- `src/platform/win32/MonitorManager.h/.cpp`
- `src/scene/ProjectionPolicy.h/.cpp`

The slice establishes a deterministic monitor-topology model and a
scale-preserving projection policy surface for span/mirror workflows.

## 1) Monitor topology model (M1-601)

### API surface

`ntium::platform::win32::MonitorManager` provides:

- `Initialize()` / `Shutdown()`
- `RefreshTopology(reason)`
- lifecycle hooks:
  - `OnDisplayChange(...)`
  - `OnDpiChange()`
  - `OnDeviceChange()`
- `SetChangeCallback(...)`
- `topology_snapshot()`

### Topology data contract

`MonitorTopologySnapshot` contains:

- `generation` (monotonic revision)
- `primary_monitor_id`
- `monitors[]` of `MonitorTopologyEntry`

Each entry carries:

- stable monitor id (`stable_monitor_id`)
- stable identity fields (`adapter_luid_*`, `target_id`, `monitor_key`)
- geometry (`left/top/right/bottom`, `width/height`, orientation)
- scale (`dpi_x`, `dpi_y`, normalized `scale_x/scale_y`)
- `is_primary`

### Stable ID strategy

- A deterministic identity key is built from
  `{adapter_luid_low, adapter_luid_high, target_id, monitor_key}`.
- `MonitorId` is generated from FNV-1a hash of that identity key.
- Collisions are resolved deterministically by incrementing to an unused id.
- Mapping is retained in `stable_ids_by_identity_key_` for process lifetime.

### Change notifications

`MonitorTopologyChangeNotification` emits:

- full post-change snapshot
- `MonitorTopologyDelta` containing:
  - reason (`kInitial`, `kDisplayChange`, `kDpiChange`, `kDeviceChange`,
    `kManualRefresh`)
  - generation
  - sorted `added`, `removed`, and `updated` monitor id sets

Updates are deterministic because snapshot enumeration is sorted by identity key
before diffing.

### `_WIN32` and stubs

- `_WIN32`: uses `EnumDisplayMonitors`, monitor geometry, orientation, and DPI
  probing (`GetDpiForMonitor`).
- non-Windows: returns a deterministic empty snapshot with no monitor records.

This keeps API behavior compilable and testable on non-Windows toolchains while
preserving real topology integration on Win32 builds.

## 2) Projection + span/mirror policy (M1-602)

### API surface

`scene::ProjectionPolicy` introduces three policy contracts:

- `ISpanPolicy`
  - `SelectSpan(topology, request) -> SpanSelection`
- `IMirrorPolicy`
  - `SelectMirror(topology, request) -> MirrorSelection`
- `IProjectionPolicy`
  - `Evaluate(topology, span_request, mirror_request, config)`

Factory helpers:

- `CreateDefaultSpanPolicy()`
- `CreateDefaultMirrorPolicy()`
- `CreateProjectionPolicy(...)`

### Span policy behavior

Supported modes:

- `kSingle`
- `kAll`
- `kSubset`

Default policy guarantees:

- invalid/empty subset requests fall back to primary monitor
- output monitor ids are sorted and de-duplicated
- primary resolution order:
  1. requested preferred primary (if selected)
  2. topology primary (if selected)
  3. first selected monitor

### Mirror policy behavior

Supported modes:

- `kDisabled`
- `kEnabled`

Default policy guarantees:

- source monitor falls back to topology primary if invalid/unset
- target ids are sorted, de-duplicated, validated, and exclude source
- produces `mirrored_to_source` map for downstream projection routing

### Scale-preserving density model

`IProjectionPolicy::Evaluate` computes per-monitor projection parameters while
preserving world scale:

1. Determine active native projection set from span selection.
2. Select **reference monitor** as highest resolution (pixel area, then DPI).
3. Compute reference density:
   - `reference_density = reference_height_px / reference_dpi`
4. For each output monitor:
   - use mirrored source monitor when mirror mode is active
   - compute source density:
     - `source_density = source_height_px / source_dpi`
   - compute `density_scale = source_density / reference_density`
   - derive monitor FOV:
     - `vertical_fov = clamp(base_fov * density_scale, min_fov, max_fov)`
   - derive `world_units_per_pixel` at unit depth from vertical FOV

The result preserves geographic/world scale relationships across mixed
resolution and mixed DPI outputs while anchoring density to the highest-res
native monitor.

## 3) Determinism and integration notes

- Topology snapshots and deltas are stable for equivalent monitor sets.
- Projection outputs are sorted by `monitor_id`.
- Mirror targets reuse source projection parameters while preserving target
  monitor metadata for output mapping.
- This slice is API-centric; render thread wiring and runtime command plumbing
  are intentionally deferred.

## 4) Validation checklist

### Compile sanity

1. Build non-Windows configuration:
   - verify stubs compile and link.
2. Build Windows configuration:
   - verify Win32 monitor enumeration/DPI code compiles.

### Functional checks

1. **Hotplug loop**
   - Added/removed monitor ids match expected deterministic deltas.
2. **DPI change loop**
   - Changed monitor ids appear in `updated_monitor_ids`.
3. **Span subset + primary preference**
   - Primary selection follows documented precedence.
4. **Mirror enable/disable transitions**
   - Source/target mapping stays valid and deterministic.
5. **Mixed resolution + DPI span**
   - Highest-resolution monitor selected as reference.
   - Relative world scale remains consistent across outputs.
