# Snapshot fuzz mutator catalog (Q1-901)

This catalog maps mutators to deterministic parser outcomes from:

- `src/ingest/SnapshotParser.h` (`SnapshotDisposition`, `SnapshotParseError`)
- `src/ingest/SnapshotParser.cpp` (`ToString`)
- `docs/contracts/event_snapshot_v1.md`

Use `last_applied_sequence = 100` unless a case states otherwise.

## Usage notes

1. Start from a known-good seed in `tests/fuzz/snapshot/corpus/`.
2. Apply exactly one mutator at a time for deterministic assertions.
3. Recompute CRCs when the case says `recompute_crc=yes`; keep old CRCs when `no`.
4. Replay each generated input 5x and assert identical output tuple:
   - `disposition`
   - `error`
   - `failure_record_index`

## Mutators

| ID | Mutation | Stage target | recompute_crc | Expected disposition | Expected error token |
|---|---|---|---|---|---|
| SNAP-M001 | Truncate frame to 63 bytes | Frame bounds | no | `kRejectSnapshot` | `frame_too_small` |
| SNAP-M002 | Expand frame to `kMaxSnapshotBytes + 1` | Frame bounds | no | `kRejectSnapshot` | `frame_too_large` |
| SNAP-M010 | Corrupt header magic (`EVS1` -> random) | Header decode | no | `kRejectSnapshot` | `invalid_magic` |
| SNAP-M011 | Set `format_version=2` | Header decode | no | `kRejectSnapshot` | `unsupported_format_version` |
| SNAP-M012 | Set `header_size=63` | Header decode | no | `kRejectSnapshot` | `invalid_header_size` |
| SNAP-M013 | Set `snapshot_kind=3` | Header decode | no | `kRejectSnapshot` | `invalid_snapshot_kind` |
| SNAP-M014 | Set non-zero `header_flags` | Header decode | no | `kRejectSnapshot` | `non_zero_header_flags` |
| SNAP-M020 | Keep bytes, flip header CRC field | Header CRC | no | `kRejectSnapshot` | `header_crc_mismatch` |
| SNAP-M021 | Keep bytes, flip snapshot CRC field | Snapshot CRC | no | `kRejectSnapshot` | `snapshot_crc_mismatch` |
| SNAP-M022 | Flip payload byte, keep payload CRC | Payload CRC | no | `kRejectSnapshot` | `payload_crc_mismatch` |
| SNAP-M030 | Set `sequence=100` (equal to last applied) | Sequence gate | yes | `kIgnoreSnapshot` | `stale_sequence` |
| SNAP-M031 | FULL_STATE with `base_sequence=99` | Sequence gate | yes | `kRejectSnapshot` | `invalid_full_state_base_sequence` |
| SNAP-M032 | DELTA with `base_sequence=99` | Sequence gate | yes | `kRejectDelta` | `base_sequence_mismatch` |
| SNAP-M040 | Set first record `record_size=20` | Record envelope | yes | `kRejectSnapshot` | `record_size_out_of_range` |
| SNAP-M041 | Set `record_size != 36 + payload_size` | Record envelope | yes | `kRejectSnapshot` | `record_size_payload_mismatch` |
| SNAP-M042 | Set record reserved field non-zero | Record envelope | yes | `kRejectSnapshot` | `record_reserved1_non_zero` |
| SNAP-M043 | Set `schema_version=2` | Record envelope | yes | `kRejectSnapshot` | `record_schema_version_invalid` |
| SNAP-M050 | Unknown record type, mandatory flag | Type handling | yes | `kRejectSnapshot` | `unknown_mandatory_record_type` |
| SNAP-M051 | Unknown record type, OPTIONAL flag | Type handling | yes | `kApply` | `none` |
| SNAP-M060 | SCAN_RING center_x out of canvas | Type ranges | yes | `kRejectSnapshot` | `scan_ring_center_out_of_range` |
| SNAP-M061 | SCAN_ARC sweep set to 0 | Type ranges | yes | `kRejectSnapshot` | `scan_arc_sweep_out_of_range` |
| SNAP-M062 | POINT_ALERT severity=9 | Type ranges | yes | `kRejectSnapshot` | `point_alert_severity_out_of_range` |
| SNAP-M063 | AREA_ALERT x_min >= x_max | Type ranges | yes | `kRejectSnapshot` | `area_alert_bounds_invalid` |
| SNAP-M070 | CLEAR_EVENT with envelope event_id != 0 | Clear rules | yes | `kRejectSnapshot` | `clear_event_envelope_event_id_non_zero` |
| SNAP-M071 | CLEAR_EVENT with ttl_ms != 1 | Clear rules | yes | `kRejectSnapshot` | `clear_event_envelope_ttl_invalid` |
| SNAP-M072 | CLEAR_EVENT `created_unix_ms != generated_unix_ms` | Clear rules | yes | `kRejectSnapshot` | `clear_event_created_timestamp_invalid` |
| SNAP-M073 | CLEAR_EVENT invalid scope=9 | Clear rules | yes | `kRejectSnapshot` | `clear_event_scope_invalid` |

## Atomic-apply guard mutators

Use a two-record snapshot with one valid early record and one invalid late record.
Expected result: parser returns reject, and caller-owned state remains unchanged.

| ID | Mutation | Expected |
|---|---|---|
| SNAP-A001 | Record 0 valid, record 1 has bad payload CRC | `kRejectSnapshot` + no partial apply |
| SNAP-A002 | Record 0 valid, record 1 has reserved field violation | `kRejectSnapshot` + no partial apply |

## Stage-order assertions

To verify contract-ordered failure, use these paired inputs:

1. `SNAP-O001`: invalid magic + broken CRC -> expect `invalid_magic` first.
2. `SNAP-O002`: valid header decode, broken header CRC -> expect `header_crc_mismatch`.
3. `SNAP-O003`: valid header/CRCs, stale sequence -> expect `stale_sequence` before record errors.
4. `SNAP-O004`: valid gate values, record payload mismatch -> expect record-stage error.

If any paired case reports a later-stage token before an earlier-stage violation,
count `validation_order_violation_count += 1`.

