# IPC Named Pipe Control Protocol v1

Status: Draft for ticket C0-002  
Owner: Contracts (A0)  
Last updated: 2026-04-28

## 1. Scope

This document defines the binary control protocol used over the local named pipe
between the control client and wallpaper host process.

It specifies:

- Frame header and framing rules.
- Message types and sequencing.
- Command payload formats.
- ACK and ERROR payload formats.
- Hard limits (payload and rate).
- Security validation requirements.
- Deterministic error codes and handling behavior.
- Compatibility and versioning policy.

This contract is normative for implementation and test vectors.

## 2. Transport

- Pipe name (v1): `\\.\pipe\ntium.wallpaper.ctrl.v1`
- Transport: Windows named pipe, byte stream, full duplex.
- All integers and IEEE754 floats are little-endian.
- One protocol frame is encoded as header + payload bytes.
- No text/JSON is allowed on this channel.

## 3. Primitive Types

- `u8`, `u16`, `u32`, `u64`: unsigned integer of 1/2/4/8 bytes.
- `f32`, `f64`: IEEE754 float/double.
- `bool8`: `u8` with values `0` or `1` only.

## 4. Frame Format (v1)

### 4.1 Fixed Header Layout (32 bytes)

| Offset | Size | Field | Type | Description |
|---:|---:|---|---|---|
| 0 | 4 | `magic` | `u32` | ASCII `IPC1` (`0x31435049` little-endian) |
| 4 | 1 | `version_major` | `u8` | Protocol major version; MUST be `1` in v1 |
| 5 | 1 | `version_minor` | `u8` | Protocol minor version; MUST be `0` for v1.0 |
| 6 | 1 | `header_len` | `u8` | MUST be `32` |
| 7 | 1 | `msg_type` | `u8` | See message type table |
| 8 | 1 | `flags` | `u8` | Bit flags (defined below) |
| 9 | 1 | `reserved0` | `u8` | MUST be `0` |
| 10 | 2 | `reserved1` | `u16` | MUST be `0` |
| 12 | 4 | `sequence` | `u32` | Sender sequence number (strictly increasing, starts at 1) |
| 16 | 4 | `ack_sequence` | `u32` | Sequence being acknowledged/rejected; else `0` |
| 20 | 4 | `payload_len` | `u32` | Payload bytes following header |
| 24 | 4 | `payload_crc32c` | `u32` | CRC32C of payload, `0` when payload_len = 0 |
| 28 | 4 | `header_crc32c` | `u32` | CRC32C over bytes `[0..27]` |

### 4.2 Header Flag Bits

- `0x01` (`ACK_REQUIRED`): sender requests explicit ACK/ERROR.
- `0x02` (`URGENT`): reserved for future use in v1; MUST be zero.
- All other bits are reserved and MUST be zero.

### 4.3 CRC Algorithm

- Algorithm: CRC-32C (Castagnoli polynomial `0x1EDC6F41`).
- Initial value: `0xFFFFFFFF`.
- Final XOR: `0xFFFFFFFF`.
- Input reflection: enabled.
- Output reflection: enabled.

## 5. Message Types

| Value | Name | Direction | Payload |
|---:|---|---|---|
| `0x01` | `HELLO` | Client -> Server | `HelloPayloadV1` |
| `0x02` | `HELLO_ACK` | Server -> Client | `HelloAckPayloadV1` |
| `0x03` | `PING` | Either | empty |
| `0x04` | `PONG` | Either | empty |
| `0x10` | `COMMAND` | Client -> Server | `CommandEnvelopeV1 + CommandBody` |
| `0x11` | `ACK` | Server -> Client | `AckPayloadV1` |
| `0x12` | `ERROR` | Server -> Client | `ErrorPayloadV1` |

## 6. Session Lifecycle

1. Client connects.
2. Server performs security validation (Section 10) before processing commands.
3. First client frame MUST be `HELLO`; otherwise `ERR_PROTOCOL_STATE`.
4. Server replies `HELLO_ACK` selecting version and limits.
5. Client sends `COMMAND` frames.
6. Each `COMMAND` produces exactly one `ACK` or `ERROR`.
7. `PING`/`PONG` are optional liveness checks and do not alter state.

If any fatal rule is violated, server closes the pipe according to Section 11.

## 7. Payload Formats

All layouts below are exact byte contracts.

### 7.1 `HELLO` (`msg_type=0x01`)

Payload size: 24 bytes.

| Offset | Size | Field | Type |
|---:|---:|---|---|
| 0 | 2 | `client_min_major` | `u16` |
| 2 | 2 | `client_min_minor` | `u16` |
| 4 | 2 | `client_max_major` | `u16` |
| 6 | 2 | `client_max_minor` | `u16` |
| 8 | 4 | `client_caps` | `u32` |
| 12 | 4 | `client_max_payload_bytes` | `u32` |
| 16 | 4 | `client_max_frames_per_sec` | `u32` |
| 20 | 4 | `reserved` | `u32` (MUST be `0`) |

### 7.2 `HELLO_ACK` (`msg_type=0x02`)

Payload size: 24 bytes.

| Offset | Size | Field | Type |
|---:|---:|---|---|
| 0 | 2 | `selected_major` | `u16` |
| 2 | 2 | `selected_minor` | `u16` |
| 4 | 4 | `server_caps` | `u32` |
| 8 | 4 | `server_max_payload_bytes` | `u32` |
| 12 | 4 | `server_max_frames_per_sec` | `u32` |
| 16 | 4 | `server_session_id` | `u32` |
| 20 | 4 | `reserved` | `u32` (MUST be `0`) |

### 7.3 `COMMAND` (`msg_type=0x10`)

Payload = envelope (8 bytes) + command body.

#### Command Envelope (first 8 bytes)

| Offset | Size | Field | Type | Notes |
|---:|---:|---|---|---|
| 0 | 2 | `command_id` | `u16` | See command table |
| 2 | 1 | `command_version` | `u8` | MUST be `1` in protocol v1 |
| 3 | 1 | `command_flags` | `u8` | Command-local flags (all zero unless stated) |
| 4 | 4 | `reserved` | `u32` | MUST be `0` |

#### Command IDs

| ID | Name | Body size |
|---:|---|---:|
| `0x0001` | `CMD_ZOOM` | 16 |
| `0x0002` | `CMD_ROTATE` | 16 |
| `0x0003` | `CMD_STYLE` | 12 |
| `0x0004` | `CMD_SPAN` | variable (8 + 4*N) |
| `0x0005` | `CMD_MIRROR` | variable (8 + 4*N) |
| `0x0006` | `CMD_CAMERA_MOVE` | 36 |

#### `CMD_ZOOM` body (`command_id=0x0001`, 16 bytes)

| Offset | Size | Field | Type | Constraints |
|---:|---:|---|---|---|
| 0 | 4 | `delta` | `f32` | +in / -out |
| 4 | 4 | `anchor_x_norm` | `f32` | `[0.0, 1.0]` |
| 8 | 4 | `anchor_y_norm` | `f32` | `[0.0, 1.0]` |
| 12 | 1 | `mode` | `u8` | `0=dolly`, `1=fov` |
| 13 | 1 | `clamp_to_surface` | `bool8` | `0/1` |
| 14 | 2 | `reserved` | `u16` | MUST be `0` |

#### `CMD_ROTATE` body (`command_id=0x0002`, 16 bytes)

| Offset | Size | Field | Type | Constraints |
|---:|---:|---|---|---|
| 0 | 4 | `delta_yaw_deg` | `f32` | finite |
| 4 | 4 | `delta_pitch_deg` | `f32` | finite |
| 8 | 4 | `delta_roll_deg` | `f32` | finite |
| 12 | 1 | `space` | `u8` | `0=world`, `1=local` |
| 13 | 3 | `reserved` | `u8[3]` | MUST be `0` |

#### `CMD_STYLE` body (`command_id=0x0003`, 12 bytes)

| Offset | Size | Field | Type | Constraints |
|---:|---:|---|---|---|
| 0 | 4 | `style_id` | `u32` | enum below |
| 4 | 4 | `transition_sec` | `f32` | `>=0`, finite |
| 8 | 4 | `style_flags` | `u32` | bitmask |

`style_id` values:

- `1 = satellite`
- `2 = roadmap`
- `3 = terrain`
- `4 = night`

`style_flags` values:

- `0x00000001 = preserve_labels`
- `0x00000002 = preserve_atmosphere`

#### `CMD_SPAN` body (`command_id=0x0004`, variable)

| Offset | Size | Field | Type | Constraints |
|---:|---:|---|---|---|
| 0 | 1 | `span_mode` | `u8` | `0=single`, `1=all`, `2=subset` |
| 1 | 1 | `monitor_count` | `u8` | see rules |
| 2 | 2 | `reserved` | `u16` | MUST be `0` |
| 4 | 4 | `primary_monitor_id` | `u32` | topology monitor id |
| 8 | 4*N | `monitor_ids` | `u32[N]` | only when subset |

Rules:

- `span_mode=0` or `1`: `monitor_count` MUST be `0`, no `monitor_ids`.
- `span_mode=2`: `monitor_count` MUST be `1..16`; `monitor_ids` count MUST match.
- For `span_mode=2`, `primary_monitor_id` MUST appear in `monitor_ids`.

#### `CMD_MIRROR` body (`command_id=0x0005`, variable)

| Offset | Size | Field | Type | Constraints |
|---:|---:|---|---|---|
| 0 | 1 | `enabled` | `bool8` | `0/1` |
| 1 | 1 | `target_count` | `u8` | see rules |
| 2 | 2 | `reserved` | `u16` | MUST be `0` |
| 4 | 4 | `source_monitor_id` | `u32` | topology monitor id |
| 8 | 4*N | `target_monitor_ids` | `u32[N]` | mirror targets |

Rules:

- `enabled=0`: `target_count` MUST be `0`, no targets.
- `enabled=1`: `target_count` MUST be `1..15`.
- Targets MUST NOT include `source_monitor_id`.
- Targets MUST be unique.

#### `CMD_CAMERA_MOVE` body (`command_id=0x0006`, 36 bytes)

| Offset | Size | Field | Type | Constraints |
|---:|---:|---|---|---|
| 0 | 1 | `move_kind` | `u8` | `0=delta_local_m`, `1=delta_world_m`, `2=set_world_ecef_m` |
| 1 | 1 | `interpolation` | `u8` | `0=immediate`, `1=linear`, `2=smoothstep` |
| 2 | 2 | `reserved0` | `u16` | MUST be `0` |
| 4 | 8 | `x` | `f64` | finite |
| 12 | 8 | `y` | `f64` | finite |
| 20 | 8 | `z` | `f64` | finite |
| 28 | 4 | `duration_ms` | `f32` | `0` for immediate, else `>0` |
| 32 | 1 | `collision_policy` | `u8` | `0=none`, `1=clamp_surface` |
| 33 | 3 | `reserved1` | `u8[3]` | MUST be `0` |

### 7.4 `ACK` (`msg_type=0x11`)

Payload size: 24 bytes.

| Offset | Size | Field | Type | Description |
|---:|---:|---|---|---|
| 0 | 4 | `acked_sequence` | `u32` | Sequence of accepted message |
| 4 | 2 | `acked_msg_type` | `u16` | Original message type |
| 6 | 2 | `acked_command_id` | `u16` | `0` when not a command |
| 8 | 4 | `result_flags` | `u32` | bit0=applied, bit1=coalesced, bit2=deferred |
| 12 | 4 | `state_revision` | `u32` | Monotonic applied-state revision |
| 16 | 4 | `queue_depth` | `u32` | Pending command queue after processing |
| 20 | 4 | `reserved` | `u32` | MUST be `0` |

### 7.5 `ERROR` (`msg_type=0x12`)

Payload size: 24 bytes.

| Offset | Size | Field | Type | Description |
|---:|---:|---|---|---|
| 0 | 4 | `failed_sequence` | `u32` | Sequence of rejected message |
| 4 | 2 | `failed_msg_type` | `u16` | Message type that failed |
| 6 | 2 | `failed_command_id` | `u16` | `0` when not a command |
| 8 | 4 | `error_code` | `u32` | Deterministic error code |
| 12 | 4 | `detail_a` | `u32` | Code-specific detail |
| 16 | 4 | `detail_b` | `u32` | Code-specific detail |
| 20 | 1 | `handling` | `u8` | `0=continue`, `1=close_after_send` |
| 21 | 3 | `reserved` | `u8[3]` | MUST be `0` |

## 8. Limits and Rate Controls

Server MUST enforce:

- `MAX_PAYLOAD_BYTES = 65536` (64 KiB)
- `MAX_FRAME_BYTES = 65568` (32-byte header + payload)
- `MAX_INBOUND_FRAMES_PER_SEC = 120` (sliding 1 second window)
- `MAX_INBOUND_PAYLOAD_BYTES_PER_SEC = 524288` (512 KiB/s, sliding 1 second)
- `MAX_IN_FLIGHT_COMMANDS = 64` (client commands sent but not yet ACK/ERROR)

Behavior on limit violations is defined in Section 11.

## 9. Compatibility and Versioning Policy

Versioning is `major.minor` in every frame header.

- Major version mismatch is incompatible and MUST be rejected.
- Minor version changes are backward-compatible only.
- For v1.x, receivers MUST:
  - reject `payload_len` smaller than the command/message minimum.
  - parse known fields.
  - ignore extra trailing bytes in a known message/command body.
- New command IDs and message types may be added in minor versions.
- Reserved fields/bits MUST be sent as `0` and ignored when receiving.

Current protocol support for this contract: `1.0` only.

## 10. Security Requirements (Normative)

Server implementation MUST enforce all checks below for each connection:

1. **Remote client rejection**
   - Create named pipe with `PIPE_REJECT_REMOTE_CLIENTS`.
   - If remote origin is still detected, reject with `ERR_REMOTE_CLIENT_REJECTED`
     and close.

2. **Same-user SID validation**
   - Resolve client process token (via client PID/token APIs).
   - Compare `TokenUser` SID to server process `TokenUser` SID.
   - Mismatch -> `ERR_AUTH_SID_MISMATCH`, close connection.

3. **Same-session validation**
   - Compare client `TokenSessionId` with server session id.
   - Mismatch -> `ERR_AUTH_SESSION_MISMATCH`, close connection.

No command may be executed before all security checks pass.

## 11. Deterministic Error Codes and Handling Policy

### 11.1 Error Codes

| Code | Name | Meaning | `detail_a` | `detail_b` | Wire behavior |
|---:|---|---|---:|---:|---|
| `0x00000001` | `ERR_UNSUPPORTED_VERSION` | Header version not supported | received major | received minor | ERROR then close |
| `0x00000002` | `ERR_BAD_HEADER_LEN` | `header_len != 32` | header_len | 32 | ERROR then close |
| `0x00000003` | `ERR_BAD_HEADER_CRC` | Header CRC mismatch | expected CRC | received CRC | close immediately (no ERROR) |
| `0x00000004` | `ERR_BAD_PAYLOAD_CRC` | Payload CRC mismatch | expected CRC | received CRC | ERROR continue |
| `0x00000005` | `ERR_PAYLOAD_TOO_LARGE` | `payload_len > MAX_PAYLOAD_BYTES` | payload_len | limit | ERROR then close |
| `0x00000006` | `ERR_UNKNOWN_MSG_TYPE` | Unknown `msg_type` | msg_type | 0 | ERROR continue |
| `0x00000007` | `ERR_PROTOCOL_STATE` | Message not valid in current state | state id | msg_type | ERROR then close |
| `0x00000008` | `ERR_UNKNOWN_COMMAND` | Unknown `command_id` | command_id | 0 | ERROR continue |
| `0x00000009` | `ERR_UNSUPPORTED_COMMAND_VERSION` | Unknown `command_version` | command_id | version | ERROR continue |
| `0x0000000A` | `ERR_MALFORMED_PAYLOAD` | Field size/range/reserved violation | field id | rule id | ERROR continue |
| `0x0000000B` | `ERR_RATE_LIMITED` | Frame or byte rate exceeded | observed | limit | ERROR continue |
| `0x0000000C` | `ERR_TOO_MANY_IN_FLIGHT` | In-flight command cap exceeded | observed | limit | ERROR continue |
| `0x0000000D` | `ERR_AUTH_SID_MISMATCH` | Client SID != server SID | 0 | 0 | ERROR then close |
| `0x0000000E` | `ERR_AUTH_SESSION_MISMATCH` | Client session != server session | client session | server session | ERROR then close |
| `0x0000000F` | `ERR_REMOTE_CLIENT_REJECTED` | Remote origin detected | 0 | 0 | ERROR then close |
| `0x00000010` | `ERR_INTERNAL` | Internal server failure | impl-defined | impl-defined | ERROR then close |

### 11.2 Deterministic Handling Rules

- Exactly one terminal response per `COMMAND`: `ACK` or `ERROR`.
- `ERROR.handling=0` means connection remains open.
- `ERROR.handling=1` means server closes after sending that frame.
- For immediate-close conditions (for example `ERR_BAD_HEADER_CRC`), no response
  frame is sent; server disconnects deterministically.

### 11.3 Deterministic `detail_a` / `detail_b` Encoding

To keep server/client test vectors deterministic, these identifiers are fixed:

#### Protocol State IDs (`ERR_PROTOCOL_STATE.detail_a`)

| ID | State |
|---:|---|
| `1` | `CONNECTED_WAIT_HELLO` |
| `2` | `ACTIVE` |
| `3` | `CLOSING` |

#### Field IDs (`ERR_MALFORMED_PAYLOAD.detail_a`)

| ID | Field |
|---:|---|
| `1` | `header.flags_reserved_bits` |
| `2` | `header.reserved0` |
| `3` | `header.reserved1` |
| `4` | `hello.reserved` |
| `5` | `hello_ack.reserved` |
| `6` | `command_envelope.reserved` |
| `7` | `command_envelope.command_version` |
| `8` | `zoom.anchor_x_norm` |
| `9` | `zoom.anchor_y_norm` |
| `10` | `zoom.mode` |
| `11` | `zoom.clamp_to_surface` |
| `12` | `rotate.space` |
| `13` | `style.style_id` |
| `14` | `style.transition_sec` |
| `15` | `span.span_mode` |
| `16` | `span.monitor_count` |
| `17` | `span.primary_monitor_id` |
| `18` | `mirror.enabled` |
| `19` | `mirror.target_count` |
| `20` | `mirror.source_monitor_id` |
| `21` | `camera_move.move_kind` |
| `22` | `camera_move.interpolation` |
| `23` | `camera_move.duration_ms` |
| `24` | `camera_move.collision_policy` |

#### Rule IDs (`ERR_MALFORMED_PAYLOAD.detail_b`)

| ID | Rule |
|---:|---|
| `1` | value is not finite |
| `2` | value outside inclusive range |
| `3` | enum value unknown |
| `4` | reserved field non-zero |
| `5` | count does not match trailing array length |
| `6` | duplicate monitor id found |
| `7` | source id appears in target list |
| `8` | required id missing from list |
| `9` | invalid size for command/message body |

#### Rate Limit Encoding (`ERR_RATE_LIMITED`)

- `detail_a`: observed value in current sliding window.
- `detail_b`: configured limit.
- On frame-rate breach, both values are in frames/sec.
- On byte-rate breach, both values are in bytes/sec.

## 12. Command Examples

Examples show decoded values for `COMMAND` payloads (header omitted for brevity).
In all examples:

- `command_version = 1`
- `command_flags = 0`
- `reserved = 0`

### 12.1 Zoom example (`CMD_ZOOM`)

- `command_id = 0x0001`
- `delta = +0.75`
- `anchor_x_norm = 0.62`
- `anchor_y_norm = 0.40`
- `mode = 0 (dolly)`
- `clamp_to_surface = 1`

### 12.2 Rotate example (`CMD_ROTATE`)

- `command_id = 0x0002`
- `delta_yaw_deg = +15.0`
- `delta_pitch_deg = -4.0`
- `delta_roll_deg = 0.0`
- `space = 1 (local)`

### 12.3 Style example (`CMD_STYLE`)

- `command_id = 0x0003`
- `style_id = 4 (night)`
- `transition_sec = 0.35`
- `style_flags = 0x00000001 (preserve_labels)`

### 12.4 Span example (`CMD_SPAN`)

- `command_id = 0x0004`
- `span_mode = 2 (subset)`
- `monitor_count = 2`
- `primary_monitor_id = 1001`
- `monitor_ids = [1001, 1003]`

### 12.5 Mirror example (`CMD_MIRROR`)

- `command_id = 0x0005`
- `enabled = 1`
- `source_monitor_id = 1001`
- `target_count = 1`
- `target_monitor_ids = [1002]`

### 12.6 Camera move example (`CMD_CAMERA_MOVE`)

- `command_id = 0x0006`
- `move_kind = 1 (delta_world_m)`
- `interpolation = 1 (linear)`
- `x = +250.0`
- `y = -30.0`
- `z = +10.0`
- `duration_ms = 180.0`
- `collision_policy = 1 (clamp_surface)`

## 13. Conformance Notes for Test Vectors

Minimum vectors required by this contract:

- valid HELLO/HELLO_ACK exchange.
- valid command + ACK for each command ID (`0x0001` to `0x0006`).
- invalid frame with header CRC mismatch (`ERR_BAD_HEADER_CRC` path).
- invalid frame with payload CRC mismatch (`ERR_BAD_PAYLOAD_CRC` path).
- oversize payload (`ERR_PAYLOAD_TOO_LARGE` path).
- unknown message type (`ERR_UNKNOWN_MSG_TYPE` path).
- SID mismatch, session mismatch, and remote client rejection paths.
