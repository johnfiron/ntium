# Ingest Foundation (Batch 1: E1-401, E1-402)

This document defines the initial ingest foundation produced in Batch 1:

- `src/ingest/SnapshotSchema.h`
- `src/ingest/SnapshotParser.h/.cpp`
- `src/ingest/SnapshotWatcher.h/.cpp`

The implementation aligns with `docs/contracts/event_snapshot_v1.md` and
provides deterministic parser error codes and watcher coalescing behavior.

## 1) Snapshot schema alignment

`SnapshotSchema.h` mirrors the contract's v1 binary layout:

- Header constants:
  - `kSnapshotMagic = 0x31535645` (`EVS1`)
  - `kSnapshotHeaderSize = 64`
  - `kSnapshotFormatVersion = 1`
- Record envelope constant:
  - `kRecordEnvelopeSize = 36`
- Hard bounds:
  - `kMaxSnapshotBytes = 1,048,576`
  - `kMaxRecordCount = 4096`
  - `kMaxRecordBytes = 4096`
  - `kMaxPayloadBytes = 4060`
  - `kMaxTtlMs = 86,400,000`

Packed wire structs are provided for:

- `SnapshotHeaderWire`
- `RecordEnvelopeWire`
- Known payload prefixes:
  - `ScanRingPayloadPrefixWire`
  - `ScanArcPayloadPrefixWire`
  - `PointAlertPayloadPrefixWire`
  - `AreaAlertPayloadPrefixWire`
  - `ClearEventPayloadPrefixWire`

`static_assert` checks pin each struct to contract byte sizes.

## 2) Parser validation order (normative)

`ParseSnapshotV1(...)` validates in contract order and returns:

- `SnapshotDisposition`:
  - `kApply`
  - `kIgnoreSnapshot`
  - `kRejectSnapshot`
  - `kRejectDelta`
- `SnapshotParseError`: deterministic code for the first failure point.

Validation stages:

1. **Frame bounds**
   - Require `snapshot_size >= 64`
   - Require `snapshot_size <= MAX_SNAPSHOT_BYTES`
2. **Header decode**
   - Validate magic, format version, header size, snapshot kind
   - Validate zero-valued header flags/reserved fields
3. **Header bounds**
   - Canvas extents in `1..16384`
   - Record count and records byte constraints
   - `header_size + records_bytes == frame_size`
4. **CRC validation**
   - Header CRC first (`[48..55]` zeroed)
   - Snapshot CRC second (`[52..55]` zeroed)
5. **Sequence gate**
   - `sequence <= last_applied_sequence` => `kIgnoreSnapshot` + `kStaleSequence`
   - FULL_STATE requires `base_sequence == 0`
   - DELTA requires `base_sequence == last_applied_sequence`
6. **Record walk**
   - Envelope size/bounds
   - Flags/schema/reserved field checks
   - Payload size/type minimum checks
   - Payload CRC
   - Type-specific field/range checks
   - Unknown record type handling:
     - optional flag set: skip safely
     - otherwise reject
7. **Atomic apply boundary**
   - Parser returns success only after full walk succeeds.
   - No partial state mutation is performed by parser.

## 3) Deterministic parser error model

`SnapshotParseError` is intentionally granular and stable for telemetry/tests:

- Header errors: `kInvalidMagic`, `kInvalidHeaderSize`, ...
- CRC errors: `kHeaderCrcMismatch`, `kSnapshotCrcMismatch`, `kPayloadCrcMismatch`
- Sequence errors: `kStaleSequence`, `kBaseSequenceMismatch`, ...
- Envelope/type errors: `kRecordSizeOutOfRange`, `kUnknownMandatoryRecordType`, ...
- Payload field range errors: per-type code families (`kScanRing*`, `kScanArc*`, ...)
- CLEAR_EVENT rule errors: dedicated `kClearEvent*` codes

`ToString(SnapshotParseError)` exposes deterministic text tokens for logging.

## 4) Watcher API and coalescing model

`SnapshotWatcher` models the intended watcher workflow:

- Start/stop lifecycle with explicit `SnapshotWatcherError`.
- Generation-based update signaling:
  - File notifications are translated into monotonically increasing generation IDs.
  - Consumer calls `ClaimNextGeneration()` to fetch pending work.

### 4.1 Windows behavior (IOCP + ReadDirectoryChangesW model)

On Windows (`_WIN32`):

1. Open directory handle (`CreateFileW`, `FILE_FLAG_OVERLAPPED`).
2. Bind handle to completion port (`CreateIoCompletionPort`).
3. Arm async notifications (`ReadDirectoryChangesW`).
4. Background worker loops on `GetQueuedCompletionStatus`.
5. Notification bursts are coalesced into latest generation for consumer pull.
6. Re-arm `ReadDirectoryChangesW` after each completion.

Optional filename filtering is supported through `snapshot_filename_filter`.

### 4.2 Non-Windows stub behavior

On non-Windows platforms:

- `Start()` returns `kUnsupportedPlatform`.
- No worker thread or notifications are started.
- `ClaimNextGeneration()` remains empty (no generated work).

This provides deterministic cross-platform behavior for tests/tooling while
preserving the Windows-first ingest architecture.

## 5) Latest-only coalescing semantics

The coalescing contract is:

- Producer side advances `latest_generation`.
- Consumer side tracks `claimed_generation`.
- `ClaimNextGeneration()` returns only the newest unclaimed generation.
- Intermediate generations are treated as coalesced.

This gives latest-state scheduling under bursty filesystem activity and prevents
unbounded work queue growth from many near-duplicate notifications.
