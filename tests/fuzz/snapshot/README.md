# Snapshot parser fuzz harness (Q1-901)

This document defines an executable fuzz/scenario plan for `src/ingest/SnapshotParser.cpp`
aligned to `docs/contracts/event_snapshot_v1.md`.

Goal: malformed snapshot input must never crash, hang, or overrun bounds, and must
produce deterministic parser dispositions/errors.

## Scope and contract alignment

Target parser entrypoint:

- `ntium::ingest::ParseSnapshotV1(const uint8_t*, size_t, const SnapshotParseContext&)`

Expected parser safety behavior:

- No process crash.
- No undefined behavior (run under sanitizers when available).
- No partial apply on rejects (`kRejectSnapshot` / `kRejectDelta` / `kIgnoreSnapshot`).
- Deterministic first-failure error code from `SnapshotParseError`.

Contract checkpoints are from:

- `docs/contracts/event_snapshot_v1.md` section 10 (validation order + failure policy)
- `docs/ingest/ingest_foundation.md` section 2/3 (parser order + deterministic error model)

## Directory layout (scaffold)

This harness plan assumes the following structure:

```text
tests/fuzz/snapshot/
  README.md                    # this file
  sample_mutators.md           # mutation catalog
  corpus/
    valid_full_state.bin       # canonical valid frame
    valid_delta.bin            # canonical valid delta frame
  out/
    run_<timestamp>/
      summary.json
      failures.jsonl
      crashing_inputs/
```

## Harness contract (adapter-friendly)

Implement or adapt a runner that supports:

```text
snapshot_fuzz_runner --seed <u64> --iterations <N> --time-budget-sec <S> \
  --corpus-dir tests/fuzz/snapshot/corpus \
  --out-dir tests/fuzz/snapshot/out/run_<timestamp> \
  --max-input-bytes 1048576
```

Runner requirements:

1. Start from valid corpus and mutate bytes/fields.
2. For each input:
   - invoke `ParseSnapshotV1(...)` with fixed context (`last_applied_sequence=100`)
   - capture disposition + error + failure record index
3. Record crashes, sanitizer faults, timeouts, and mismatches to expected deterministic outcomes.
4. Persist JSON summary with pass/fail gates.

If no custom runner exists yet, a minimal adapter can be written around existing parser APIs.

## Deterministic pass/fail gates

Run-level exit conditions:

- PASS (exit `0`) only if all are true:
  - `crash_count == 0`
  - `timeout_count == 0`
  - `asan_ubsan_violation_count == 0` (when sanitizers enabled)
  - `nondeterminism_count == 0` (same input -> same disposition/error across replay)
  - `unexpected_apply_count == 0` for clearly-invalid mutated inputs
  - `validation_order_violation_count == 0` for ordered stage tests
- FAIL (exit `1`) if any gate above fails.
- HARNESS_ERROR (exit `2`) for runner/config/corpus issues.

### Determinism check

For every saved failing candidate and 1% sample of accepted cases:

- replay input 5x with identical context
- require identical tuple:
  - `disposition`
  - `error`
  - `failure_record_index`

Any drift increments `nondeterminism_count`.

## Required test classes

1. **Structural truncation/overflow**
   - frame smaller than 64 bytes
   - frame larger than `kMaxSnapshotBytes`
   - record envelope truncation
2. **Header contract violations**
   - magic/version/header size/snapshot kind/reserved bits
3. **CRC corruption**
   - header CRC mismatch
   - snapshot CRC mismatch
   - payload CRC mismatch
4. **Sequence gate**
   - stale sequence -> `kIgnoreSnapshot` + `kStaleSequence`
   - full-state nonzero base -> reject snapshot
   - delta base mismatch -> `kRejectDelta`
5. **Record type/range violations**
   - required payload size too small
   - nonzero reserved fields
   - out-of-range type fields
6. **Unknown type semantics**
   - unknown mandatory record -> reject
   - unknown optional record -> parse succeeds with skip

Use the concrete mutator recipes in `sample_mutators.md`.

## Example execution plan

### Quick PR gate (deterministic smoke)

```bash
snapshot_fuzz_runner \
  --seed 901 \
  --iterations 20000 \
  --time-budget-sec 120 \
  --corpus-dir tests/fuzz/snapshot/corpus \
  --out-dir tests/fuzz/snapshot/out/run_quick
```

Expected:

- Exit `0`
- `summary.json.pass == true`

### Nightly reliability gate (ticket acceptance)

```bash
snapshot_fuzz_runner \
  --seed 901 \
  --iterations 5000000 \
  --time-budget-sec 86400 \
  --corpus-dir tests/fuzz/snapshot/corpus \
  --out-dir tests/fuzz/snapshot/out/run_24h
```

Expected:

- Exit `0`
- no crashes/timeouts/nondeterminism
- invalid inputs never produce unintended `kApply`

## Summary JSON schema (minimum)

```json
{
  "ticket": "Q1-901",
  "seed": 901,
  "iterations": 5000000,
  "duration_sec": 86400,
  "crash_count": 0,
  "timeout_count": 0,
  "asan_ubsan_violation_count": 0,
  "nondeterminism_count": 0,
  "unexpected_apply_count": 0,
  "validation_order_violation_count": 0,
  "top_errors": [
    {"error": "header_crc_mismatch", "count": 120000}
  ],
  "pass": true
}
```

## CI integration suggestion

- Add `fuzz-24h-parser` job to nightly reliability workflow.
- Upload:
  - `summary.json`
  - `failures.jsonl`
  - minimized repro inputs (`crashing_inputs/` and `nondeterministic_inputs/`)
- Gate job on exit code and `pass=true`.

