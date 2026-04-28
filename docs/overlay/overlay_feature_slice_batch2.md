# Overlay Feature Slice Foundation — Batch 2 (O1-403 / O1-404 / O1-405)

This document defines the initial overlay algorithm slice added in Batch 2:

- `src/overlay/EventStore.h/.cpp`
- `src/overlay/DirtyRegionGenerator.h/.cpp`
- `src/overlay/OverlayThrottle.h/.cpp`

The implementation is intentionally lightweight (C++17 + STL only) so it compiles in non-Windows CI and can be integrated into render/ingest slices incrementally.

## 1) O1-403 — Event Store with TTL heap

### 1.1 Core behavior

`overlay::EventStore` tracks live overlay events keyed by `event_id` with:

- upsert semantics (`Upsert`)
- per-key clear semantics (`Clear`)
- global clear barrier (`ClearAll`)
- TTL expiry (`Expire(now)`)

Each mutation is evaluated against deterministic `event_sequence` conflict rules.

### 1.2 Deterministic sequence/conflict model

For a given `event_id`, incoming operations are accepted only when:

`incoming_sequence > max(global_clear_sequence, live_record_sequence, tombstone_sequence)`

Out-of-order/stale operations are ignored via `EventStoreResult::kIgnoredStale`.

This handles races such as:

- stale upsert arriving after clear
- stale clear arriving after newer upsert
- stale upsert after global clear

### 1.3 TTL heap model

Expiry scheduling uses a min-heap (`std::priority_queue` with reversed comparator):

- Heap node: `{expires_at, event_id, sequence}`
- Upsert pushes one heap node
- Expire pops while `expires_at <= now`
- Stale heap entries are skipped by validating current record `{sequence, expires_at}`

Complexity:

- Upsert: `O(log N)` heap insertion
- Expire: amortized `O(K log N)` for `K` expired nodes
- Clear/ClearAll: map operations; ClearAll is linear in live records

### 1.4 Bounded memory tombstones

Per-key tombstones are retained to reject stale writes after delete/expire.

`EventStoreOptions::max_tombstones` caps retained tombstone keys; oldest superseded entries are pruned deterministically. `0` disables tombstone retention.

## 2) O1-404 — Dirty region generation from old/new bounds

### 2.1 Delta input model

`overlay::DirtyRegionDelta` describes one transition:

- `old_bounds` (optional)
- `new_bounds` (optional)
- `event_sequence`
- `padding_px`
- `force_redraw_if_unchanged`

`DirtyRegionGenerator::Generate` handles:

- add: `old=null, new=valid` -> dirty(new)
- remove/expire: `old=valid, new=null` -> dirty(old)
- modify (disjoint): dirty(old) + dirty(new)
- modify (overlap): dirty(old \cap not new) U dirty(new \cap not old) via subtraction fragments
- unchanged: empty (or full old/new rect when `force_redraw_if_unchanged=true`)

### 2.2 Padding and normalization

All generated rects are padded (`padding_px`) and int32-clamped.

Touching/overlapping rects are merged to reduce present fragmentation.

Output ordering is deterministic (`top`, then `left`, then `right`, then `bottom`).

### 2.3 Batch ordering

`GenerateForBatch` sorts deltas by `(event_sequence, event_id)` before computing and merging, preserving deterministic output for identical input streams.

## 3) O1-405 — Overlay throttle gate (`<= 10 Hz`)

### 3.1 Contract

`overlay::OverlayThrottle` enforces present cadence for overlay-only bursts:

- Default min interval: `100ms` (10Hz cap)
- Coalesces pending overlay work to latest pending sequence
- Allows immediate present for urgent/bypass contexts

### 3.2 API behavior

- `RecordOverlayInvalidation(seq)` updates pending latest overlay sequence
  - stale/duplicate sequences are rejected
- `Evaluate(now, overlay_only_work, bypass_overlay_throttle)` returns `Decision`
  - allow now if bypass or not overlay-only
  - allow now if throttle interval elapsed
  - else defer with `next_deadline`
- `OnPresented(now, seq)` updates `last_present_*` and clears pending covered work

This provides bounded-latency coalescing with no queue growth inside the throttle component.

## 4) Integration notes

- All three components are independent and have no renderer/OS-specific dependencies.
- Namespace is `overlay` to match existing foundational slices (`render`, `scene`, etc.).
- Sequence-based ordering is the primary determinism axis across store, dirty generation, and throttling.
