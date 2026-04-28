# IPC fuzz and spam sample cases (Q1-902)

This casebook defines deterministic expectations for IPC fuzzing.

References:

- `docs/contracts/ipc_pipe_v1.md` sections 8/11
- `src/ipc/PipeProtocol.h` (`PipeErrorCode`, limits)
- `src/ipc/PipeProtocol.cpp` validation logic

## Case format

Each case captures:

- input setup
- expected wire-level behavior
- expected connection handling

## Protocol correctness cases

| ID | Input mutation / scenario | Expected result |
|---|---|---|
| IPC-C001 | Valid `HELLO` (24 bytes reserved=0) | `HELLO_ACK` accepted; session active |
| IPC-C002 | First frame is `COMMAND` before `HELLO` | `ERR_PROTOCOL_STATE`, `handling=close_after_send`, then disconnect |
| IPC-C003 | Header CRC mismatch | immediate disconnect, no `ERROR` frame (`send_error_frame=false`) |
| IPC-C004 | Payload CRC mismatch with otherwise valid header | `ERR_BAD_PAYLOAD_CRC`, `handling=continue`, connection stays open |
| IPC-C005 | Unknown `msg_type=0x99` | `ERR_UNKNOWN_MSG_TYPE`, continue |
| IPC-C006 | `payload_len > 65536` | `ERR_PAYLOAD_TOO_LARGE`, close-after-send |
| IPC-C007 | `header_len != 32` | `ERR_BAD_HEADER_LEN`, close-after-send |
| IPC-C008 | Unsupported version major/minor | `ERR_UNSUPPORTED_VERSION`, close-after-send |
| IPC-C009 | `HELLO.reserved != 0` | `ERR_MALFORMED_PAYLOAD` (`field=hello.reserved`, rule reserved non-zero), continue |
| IPC-C010 | Unknown `command_id` | `ERR_UNKNOWN_COMMAND`, continue |
| IPC-C011 | Known command id, `command_version != 1` | `ERR_UNSUPPORTED_COMMAND_VERSION`, continue |
| IPC-C012 | Command envelope reserved non-zero | `ERR_MALFORMED_PAYLOAD` (`command_envelope.reserved`), continue |

## Command body range/reserved cases

| ID | Command mutation | Expected deterministic error |
|---|---|---|
| IPC-C100 | `CMD_ZOOM.anchor_x_norm = NaN` | `ERR_MALFORMED_PAYLOAD` field `zoom.anchor_x_norm`, rule `not finite` |
| IPC-C101 | `CMD_ZOOM.mode = 7` | malformed payload, enum unknown |
| IPC-C102 | `CMD_ROTATE.space = 3` | malformed payload, enum unknown |
| IPC-C103 | `CMD_STYLE.style_id = 9` | malformed payload, enum unknown |
| IPC-C104 | `CMD_SPAN.span_mode=2`, count mismatch vs trailing ids | malformed payload, count mismatch |
| IPC-C105 | `CMD_SPAN.primary_monitor_id` not in subset list | malformed payload, required id missing |
| IPC-C106 | `CMD_MIRROR.enabled=1`, duplicate target ids | malformed payload, duplicate monitor id |
| IPC-C107 | `CMD_MIRROR` target contains source id | malformed payload, source id appears in target list |
| IPC-C108 | `CMD_CAMERA_MOVE.interpolation=0`, `duration_ms=50` | malformed payload, out-of-range duration rule |
| IPC-C109 | `CMD_CAMERA_MOVE.collision_policy=9` | malformed payload, enum unknown |

## Spam/rate limit cases

| ID | Load profile | Expected |
|---|---|---|
| IPC-S200 | 300 small frames/sec sustained for 10 sec | repeated `ERR_RATE_LIMITED`; no crash/hang |
| IPC-S201 | 1 MiB/sec payload stream (above 512 KiB/s cap) | `ERR_RATE_LIMITED` with byte-rate details |
| IPC-S202 | >64 in-flight commands without waiting ACK/ERROR | `ERR_TOO_MANY_IN_FLIGHT`, continue |
| IPC-S203 | Alternate valid and malformed frames at high rate | deterministic mix of ACK/ERROR; no protocol desync |
| IPC-S204 | Recovery check: after rate limiting, reduce to compliant rate | connection remains usable, valid commands ACK again |

## Deterministic replay checks

For each failing case (`IPC-C*`, `IPC-S*`):

1. Capture raw frame stream bytes.
2. Replay stream 5 times against identical build/config.
3. Require identical sequence of outcomes:
   - ACK vs ERROR vs disconnect
   - error code + details
   - handling close/continue

Any mismatch increments `nondeterminism_count`.

## Minimal pass criteria

The sample case suite passes only if:

- all `IPC-C*` expected outcomes match exactly
- `IPC-S*` profiles produce bounded behavior with no crash/hang
- no unexpected close-policy deviations
- replay determinism holds for all failing streams

