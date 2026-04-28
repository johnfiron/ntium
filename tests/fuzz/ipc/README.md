# IPC fuzz + spam harness (Q1-902)

This plan defines reliability hardening for the control pipe protocol:

- Contract: `docs/contracts/ipc_pipe_v1.md`
- Foundation: `docs/ipc/ipc_foundation.md`
- Code surface: `src/ipc/PipeProtocol.cpp`, `src/ipc/PipeServer.cpp`, `src/ipc/PipeSecurity.cpp`

Goal: malformed frames and high-rate spam must remain bounded and deterministic
with correct ACK/ERROR/disconnect behavior.

## Scope

Two complementary lanes are required:

1. **Frame fuzz lane** (`TryParseFrame` / validation helpers)
   - corrupt headers, CRCs, payloads, reserved bits, command bodies
2. **Spam/rate lane** (session/server integration)
   - frame-rate and byte-rate saturation
   - in-flight command pressure
   - deterministic close vs continue behavior under protocol violations

Use `sample_cases.md` for the canonical case list.

## Harness contract (executable scaffold)

Runner command shape:

```text
pipe_fuzz_runner --seed <u64> --duration-sec <S> \
  --mode malformed|spam|mixed \
  --pipe-name "\\.\pipe\ntium.wallpaper.ctrl.v1" \
  --out-dir tests/fuzz/ipc/out/run_<timestamp>
```

Recommended runner capabilities:

- deterministic RNG via `--seed`
- explicit frame generator for all message/command IDs
- mutation operators for header/payload/CRC/range/reserved violations
- expected-outcome assertions per case id
- structured output:
  - `summary.json`
  - `failures.jsonl`
  - `repro_frames/` for minimized failing inputs

## Deterministic pass/fail gates

PASS (exit `0`) only if all are true:

- `crash_count == 0`
- `hang_count == 0`
- `nondeterminism_count == 0` (same frame stream -> same server/parser outcome)
- `unexpected_response_count == 0` (ACK vs ERROR vs disconnect mismatch)
- `close_policy_violation_count == 0` (immediate close vs close-after-send vs continue)
- `rate_limit_bypass_count == 0`

FAIL (exit `1`) if any gate fails.  
HARNESS_ERROR (exit `2`) for runner setup/config problems.

### Determinism tuple

For each replayed case, require stable tuple:

- `error_code` (or ACK)
- `detail_a`, `detail_b`
- `handling`
- `send_error_frame` (where observable in parser-level lane)
- connection outcome (`open` / `closed`)

## Contract behaviors that must be asserted

### Immediate close (no ERROR frame)

- `ERR_BAD_HEADER_CRC` (`0x00000003`)

### ERROR then close

- `ERR_UNSUPPORTED_VERSION`
- `ERR_BAD_HEADER_LEN`
- `ERR_PAYLOAD_TOO_LARGE`
- `ERR_PROTOCOL_STATE`
- `ERR_AUTH_SID_MISMATCH`
- `ERR_AUTH_SESSION_MISMATCH`
- `ERR_REMOTE_CLIENT_REJECTED`
- `ERR_INTERNAL`

### ERROR then continue

- `ERR_BAD_PAYLOAD_CRC`
- `ERR_UNKNOWN_MSG_TYPE`
- `ERR_UNKNOWN_COMMAND`
- `ERR_UNSUPPORTED_COMMAND_VERSION`
- `ERR_MALFORMED_PAYLOAD`
- `ERR_RATE_LIMITED`
- `ERR_TOO_MANY_IN_FLIGHT`

## Spam strategy (boundedness checks)

Traffic profiles:

1. **Frame-rate spam:** bursts above `MAX_INBOUND_FRAMES_PER_SEC` (120 fps window)
2. **Byte-rate spam:** payload-heavy traffic above `MAX_INBOUND_PAYLOAD_BYTES_PER_SEC`
   (524,288 bytes/sec)
3. **In-flight pressure:** command stream with delayed ACK consumption to exceed
   `MAX_IN_FLIGHT_COMMANDS` (64)
4. **Mixed adversarial:** interleave valid commands with malformed frames to ensure
   parser state does not desynchronize.

Required expectations:

- server remains responsive (no deadlock/stall)
- violations produce deterministic protocol errors
- connection close behavior follows contract for each error class

## Example commands

Quick PR smoke:

```bash
pipe_fuzz_runner \
  --seed 902 \
  --duration-sec 180 \
  --mode mixed \
  --pipe-name "\\.\pipe\ntium.wallpaper.ctrl.v1" \
  --out-dir tests/fuzz/ipc/out/run_quick
```

Nightly reliability gate:

```bash
pipe_fuzz_runner \
  --seed 902 \
  --duration-sec 86400 \
  --mode mixed \
  --pipe-name "\\.\pipe\ntium.wallpaper.ctrl.v1" \
  --out-dir tests/fuzz/ipc/out/run_24h
```

Expected for both:

- exit `0`
- `summary.json.pass == true`

## Summary JSON schema (minimum)

```json
{
  "ticket": "Q1-902",
  "seed": 902,
  "mode": "mixed",
  "duration_sec": 86400,
  "cases_executed": 1200000,
  "crash_count": 0,
  "hang_count": 0,
  "nondeterminism_count": 0,
  "unexpected_response_count": 0,
  "close_policy_violation_count": 0,
  "rate_limit_bypass_count": 0,
  "error_histogram": [
    {"code": "ERR_MALFORMED_PAYLOAD", "count": 8200}
  ],
  "pass": true
}
```

## CI integration suggestion

- Add jobs:
  - `pipe-fuzz-24h`
  - `rate-limit-check`
- Upload outputs:
  - `summary.json`
  - `failures.jsonl`
  - reproduced frame streams
- Gate on exit code and `pass=true`.

