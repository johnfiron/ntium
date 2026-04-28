# Event Snapshot Binary Contract v1

Status: normative  
Audience: producer/parser/renderer implementations for Windows 11 25H2 wallpaper overlays

This document defines the binary wire format for snapshot v1 used to deliver overlay events in an event-driven pipeline.

## 1) Encoding rules

1. All multi-byte integers are **little-endian**.
2. No implicit padding is allowed. Sizes and offsets are exact.
3. `MUST`, `MUST NOT`, `SHOULD`, `MAY` are normative.
4. A snapshot is atomic: parse + validate fully, then apply; never apply partially.

### Primitive types

- `u8/u16/u32/u64`: unsigned integer, 8/16/32/64-bit
- `i16`: signed integer, 16-bit two's complement

## 2) Snapshot layout (v1)

```
+----------------------+ 0
| SnapshotHeader (64B) |
+----------------------+ 64
| Record[0]            |
| ...                  |
| Record[n-1]          |
+----------------------+ 64 + records_bytes
```

Total snapshot bytes MUST equal `header_size + records_bytes`.

## 3) Snapshot header (fixed 64 bytes)

| Offset | Size | Field | Type | Rules |
|---:|---:|---|---|---|
| 0 | 4 | magic | u32 | ASCII `"EVS1"` (`0x31535645`) |
| 4 | 2 | format_version | u16 | MUST be `1` |
| 6 | 2 | header_size | u16 | MUST be `64` |
| 8 | 1 | snapshot_kind | u8 | `1=FULL_STATE`, `2=DELTA` |
| 9 | 1 | header_flags | u8 | MUST be `0` in v1 |
| 10 | 2 | reserved0 | u16 | MUST be `0` |
| 12 | 8 | sequence | u64 | Strictly monotonic increasing per stream |
| 20 | 8 | base_sequence | u64 | See section 7 |
| 28 | 8 | generated_unix_ms | u64 | Producer wall-clock ms at snapshot creation |
| 36 | 2 | canvas_width_px | u16 | `1..16384` |
| 38 | 2 | canvas_height_px | u16 | `1..16384` |
| 40 | 4 | record_count | u32 | `0..4096` |
| 44 | 4 | records_bytes | u32 | Sum of all record sizes |
| 48 | 4 | header_crc32c | u32 | CRC-32C over header with bytes `48..55` zeroed |
| 52 | 4 | snapshot_crc32c | u32 | CRC-32C over full snapshot with bytes `52..55` zeroed |
| 56 | 8 | reserved1 | u64 | MUST be `0` |

## 4) Record envelope (fixed 36 bytes)

Each record is:

```
RecordEnvelope (36B) + Payload (payload_size bytes)
```

| Offset | Size | Field | Type | Rules |
|---:|---:|---|---|---|
| 0 | 2 | record_size | u16 | MUST equal `36 + payload_size` |
| 2 | 1 | record_type | u8 | See section 5 |
| 3 | 1 | record_flags | u8 | Bit 0 = OPTIONAL (`1`), others MUST be `0` |
| 4 | 1 | schema_version | u8 | MUST be `1` for v1 known types |
| 5 | 1 | reserved0 | u8 | MUST be `0` |
| 6 | 2 | reserved1 | u16 | MUST be `0` |
| 8 | 8 | event_id | u64 | Event identity (upsert key) for non-clear types |
| 16 | 8 | created_unix_ms | u64 | Producer wall-clock creation time |
| 24 | 4 | ttl_ms | u32 | `1..86400000` (24h max), see section 8 |
| 28 | 2 | payload_size | u16 | Must match type minimum/fixed sizes below |
| 30 | 2 | reserved2 | u16 | MUST be `0` |
| 32 | 4 | payload_crc32c | u32 | CRC-32C of payload bytes only |

### Record bounds

- `record_size` MUST be `36..4096`.
- `payload_size` MUST be `0..4060`.
- Total of all `record_size` values MUST equal header `records_bytes`.
- Parsed record count MUST equal header `record_count`.

## 5) Record types and payloads

### Type IDs

- `0x01` = `SCAN_RING`
- `0x02` = `SCAN_ARC`
- `0x03` = `POINT_ALERT`
- `0x04` = `AREA_ALERT`
- `0x05` = `CLEAR_EVENT`

All coordinates are in canvas pixel space.

---

### 5.1 SCAN_RING (`record_type=0x01`, payload 16 bytes)

| Offset | Size | Field | Type | Rules |
|---:|---:|---|---|---|
| 0 | 2 | center_x_px | u16 | `0..canvas_width_px-1` |
| 2 | 2 | center_y_px | u16 | `0..canvas_height_px-1` |
| 4 | 2 | radius_px | u16 | `1..16384` |
| 6 | 2 | stroke_px | u16 | `1..1024` |
| 8 | 2 | phase_deg_x100 | u16 | `0..35999` |
| 10 | 2 | angular_velocity_deg_x100 | i16 | `-36000..36000` |
| 12 | 4 | color_argb | u32 | ARGB8888 |

### 5.2 SCAN_ARC (`record_type=0x02`, payload 20 bytes)

| Offset | Size | Field | Type | Rules |
|---:|---:|---|---|---|
| 0 | 2 | center_x_px | u16 | `0..canvas_width_px-1` |
| 2 | 2 | center_y_px | u16 | `0..canvas_height_px-1` |
| 4 | 2 | radius_px | u16 | `1..16384` |
| 6 | 2 | stroke_px | u16 | `1..1024` |
| 8 | 2 | start_deg_x100 | u16 | `0..35999` |
| 10 | 2 | sweep_deg_x100 | u16 | `1..36000` |
| 12 | 2 | angular_velocity_deg_x100 | i16 | `-36000..36000` |
| 14 | 2 | reserved | u16 | MUST be `0` |
| 16 | 4 | color_argb | u32 | ARGB8888 |

### 5.3 POINT_ALERT (`record_type=0x03`, payload 16 bytes)

| Offset | Size | Field | Type | Rules |
|---:|---:|---|---|---|
| 0 | 2 | x_px | u16 | `0..canvas_width_px-1` |
| 2 | 2 | y_px | u16 | `0..canvas_height_px-1` |
| 4 | 2 | size_px | u16 | `1..1024` |
| 6 | 2 | style_id | u16 | Producer-defined style map key |
| 8 | 2 | pulse_period_ms | u16 | `16..60000` |
| 10 | 1 | severity | u8 | `0..5` |
| 11 | 1 | z_index | u8 | `0..31` |
| 12 | 4 | color_argb | u32 | ARGB8888 |

### 5.4 AREA_ALERT (`record_type=0x04`, payload 16 bytes)

| Offset | Size | Field | Type | Rules |
|---:|---:|---|---|---|
| 0 | 2 | x_min_px | u16 | `< x_max_px` |
| 2 | 2 | y_min_px | u16 | `< y_max_px` |
| 4 | 2 | x_max_px | u16 | `<= canvas_width_px` |
| 6 | 2 | y_max_px | u16 | `<= canvas_height_px` |
| 8 | 2 | border_px | u16 | `0..1024` |
| 10 | 1 | fill_alpha | u8 | `0..255` |
| 11 | 1 | severity | u8 | `0..5` |
| 12 | 4 | color_argb | u32 | ARGB8888 |

### 5.5 CLEAR_EVENT (`record_type=0x05`, payload 12 bytes)

`CLEAR_EVENT` applies an immediate removal operation and is **not persisted as an active event**.

| Offset | Size | Field | Type | Rules |
|---:|---:|---|---|---|
| 0 | 1 | clear_scope | u8 | `0=ALL`, `1=BY_TYPE`, `2=BY_EVENT_ID` |
| 1 | 1 | target_type | u8 | Required only for `BY_TYPE`; else `0` |
| 2 | 2 | reserved | u16 | MUST be `0` |
| 4 | 8 | target_event_id | u64 | Required only for `BY_EVENT_ID`; else `0` |

`CLEAR_EVENT` constraints:
- `event_id` in envelope MUST be `0`.
- `ttl_ms` MUST be `1`.
- `created_unix_ms` MUST be `generated_unix_ms` of containing snapshot.

## 6) Maximum bounds (hard limits)

Implementations MUST enforce:

- `MAX_SNAPSHOT_BYTES = 1,048,576`
- `MAX_RECORD_COUNT = 4096`
- `MAX_RECORD_BYTES = 4096`
- `MAX_TTL_MS = 86,400,000` (24 hours)
- `canvas_width_px`, `canvas_height_px` in `1..16384`

Any bound violation is an invalid snapshot (section 10).

## 7) FULL_STATE and DELTA semantics

### FULL_STATE (`snapshot_kind=1`)

- `base_sequence` MUST be `0`.
- Snapshot defines the full active overlay set after apply.
- Apply algorithm:
  1. Start with empty active set.
  2. Apply records in wire order.
  3. Upsert non-clear events by `event_id`.
  4. Execute clear operations when encountered.
  5. Drop events already expired at `generated_unix_ms` (section 8).

### DELTA (`snapshot_kind=2`)

- `base_sequence` MUST equal local `last_applied_sequence`.
- If base mismatch, snapshot MUST be rejected with reason `BASE_SEQUENCE_MISMATCH` (no partial apply).
- Apply algorithm:
  1. Start from current active set.
  2. Apply records in wire order with same upsert/clear behavior.
  3. Remove events expired at apply time (section 8).

### Superseding behavior

- If `sequence <= last_applied_sequence`, parser MUST ignore snapshot (`STALE_SEQUENCE`).
- A valid newer `FULL_STATE` MAY supersede missed intermediates immediately.
- A `DELTA` never bridges gaps; it only applies to its exact base sequence.

## 8) TTL semantics

For non-clear events:

- Absolute expiry: `expires_unix_ms = created_unix_ms + ttl_ms`.
- If `expires_unix_ms <= generated_unix_ms`, event is expired-at-source and MUST NOT be active after apply.
- Runtime renderers MUST drop an event once local wall clock reaches `expires_unix_ms`.

For `CLEAR_EVENT`, TTL does not represent persistence; it is fixed by section 5.5.

## 9) CRC rules

Algorithm for all CRC fields: **CRC-32C (Castagnoli)**, reflected, init `0xFFFFFFFF`, xorout `0xFFFFFFFF`.

1. `header_crc32c`:
   - Input: header bytes `[0..63]` with bytes `[48..55]` zeroed.
2. `snapshot_crc32c`:
   - Input: full snapshot bytes `[0..(header_size + records_bytes - 1)]` with bytes `[52..55]` zeroed.
3. `payload_crc32c`:
   - Input: payload bytes only for that record.

CRC mismatches are hard failures (section 10).

## 10) Parser validation order and failure policy

Validation MUST run in this order:

1. **Frame bounds**: size `>=64`, `<=MAX_SNAPSHOT_BYTES`.
2. **Header decode**: magic/version/header size/kind/reserved fields.
3. **Header bounds**: canvas, count, `records_bytes`, total byte equality.
4. **Header CRC** then **Snapshot CRC**.
5. **Sequence gate**:
   - stale sequence check
   - FULL_STATE/DELTA base-sequence rule
6. **Record walk** (for each record in order):
   - envelope size and bounds
   - reserved fields and schema version
   - payload size exactness for type
   - payload CRC
   - type-specific field/range checks
7. **Atomic apply**.

Failure policy:

- Structural/CRC/type/range failure => `REJECT_SNAPSHOT`; active state unchanged.
- Base mismatch => `REJECT_DELTA`; active state unchanged.
- Stale sequence => `IGNORE_SNAPSHOT`; active state unchanged.
- Unknown `record_type`:
  - if `OPTIONAL` flag set: skip record
  - else: `REJECT_SNAPSHOT`

## 11) Compatibility guarantees

1. v1 readers/writers guarantee exact behavior for fields and types defined here.
2. `format_version` major bump is required for breaking changes.
3. Forward-compatible additive changes under v1:
   - New record types MAY be introduced as OPTIONAL records.
   - For existing types, payload MAY append trailing bytes; v1 readers MUST parse known prefix and ignore tail if `payload_size >= known_min`.
4. Reserved fields must remain zero in v1; non-zero reserved fields are invalid to preserve deterministic behavior.

## 12) Examples

### Example A: FULL_STATE snapshot

- Header:
  - `snapshot_kind=FULL_STATE`
  - `sequence=120`
  - `base_sequence=0`
  - `record_count=2`
- Records:
  1. `SCAN_RING` with `event_id=9001`, `ttl_ms=5000`
  2. `AREA_ALERT` with `event_id=9002`, `ttl_ms=3000`
- Result: active set is exactly `{9001, 9002}` minus any source-expired records.

### Example B: DELTA snapshot

- Precondition: `last_applied_sequence=120`
- Header:
  - `snapshot_kind=DELTA`
  - `sequence=121`
  - `base_sequence=120`
  - `record_count=2`
- Records (wire order):
  1. `CLEAR_EVENT(BY_EVENT_ID, target_event_id=9001)`
  2. `POINT_ALERT(event_id=9010, ttl_ms=2000, severity=4)`
- Result: remove event `9001`, upsert `9010`, keep all other non-expired events.

### Example C: Out-of-order DELTA

- Local `last_applied_sequence=120`
- Incoming DELTA has `sequence=123`, `base_sequence=122`
- Result: reject with `BASE_SEQUENCE_MISMATCH`; wait for compatible DELTA or any newer FULL_STATE.
