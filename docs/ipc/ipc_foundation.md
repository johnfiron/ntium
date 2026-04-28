# IPC foundation (Batch 1: I1-501 / I1-502)

This document describes the foundation interfaces and validation flow introduced for
the named-pipe control plane. It maps directly to
`docs/contracts/ipc_pipe_v1.md` and is intended as implementation guidance for
the full server loop, command dispatcher, and tests.

## Files

- `src/ipc/PipeProtocol.h/.cpp`
  - Wire constants and enums (`message type`, `command id`, `error code`).
  - Frame decode/encode helpers for the fixed 32-byte header.
  - CRC-32C implementation and deterministic header/payload validation helpers.
  - Message and command payload validation helpers aligned to protocol rule IDs.
- `src/ipc/PipeServer.h/.cpp`
  - Named-pipe server foundation interface for overlapped-I/O oriented operation.
  - Lifecycle + callback contracts for connection events, frame dispatch, and
    outgoing writes.
  - Windows-first operation context model (`Connect`, `ReadHeader`, `ReadPayload`,
    `Write`) with non-Windows unsupported stubs.
- `src/ipc/PipeSecurity.h/.cpp`
  - Security policy contract for same-user, same-session authorization.
  - Windows implementation that validates:
    - remote-client rejection,
    - SID equality (`TokenUser`),
    - session-id equality (`TokenSessionId`).
  - Non-Windows unsupported fallback decision.

## Protocol flow

The foundation is built around deterministic frame parsing and explicit validation
stages:

1. **Header decode**
   - Read exactly 32 bytes.
   - Decode little-endian fields to `PipeFrameHeader`.
2. **Header validation**
   - Validate magic/version/header length.
   - Validate CRC over header bytes `[0..27]`.
   - Enforce reserved bits/fields and payload limits.
3. **Payload read**
   - Read `payload_len` bytes.
4. **Payload CRC**
   - Validate CRC-32C (or zero-length payload contract).
5. **Message/command validation**
   - Validate fixed-size requirements where applicable (`HELLO`, `HELLO_ACK`,
     `ACK`, `ERROR`).
   - For `COMMAND`, parse envelope and validate command body constraints.
6. **Dispatch**
   - Deliver a valid frame to the session callback.

`TryParseFrame` in `PipeProtocol.cpp` intentionally returns both parse success and
`PipeValidationIssue` for deterministic error frame generation.

## Server lifecycle model

`PipeServer` is intentionally an interface scaffold for a completion-port /
overlapped style loop:

- `Start(config)` opens server resources and enters listening mode.
- `PollOnce(timeout)` advances one cycle of I/O completion processing.
- `Stop()` begins shutdown and closes active resources.

Each accepted client maps to a `PipeConnectionContext` carrying:

- connection id and protocol state (`CONNECTED_WAIT_HELLO`, `ACTIVE`, `CLOSING`),
- inbound buffer assembly state,
- sequence bookkeeping,
- in-flight command count / rate-window inputs.

Callbacks:

- `OnConnectionOpened`
- `OnFrameReceived`
- `OnConnectionClosed`
- `OnSecurityRejected`
- `OnServerError`

This design keeps framing/security concerns in the foundation while leaving command
execution and ACK/ERROR production to higher layers.

## Error handling mapping

`PipeValidationIssue` encodes:

- `code` (`PipeErrorCode`)
- `detail_a` / `detail_b`
- `handling` (`continue` or `close_after_send`)
- `send_error_frame` (false for immediate-close paths such as bad header CRC)

This supports direct mapping to contract behavior:

- **Immediate close**
  - `ERR_BAD_HEADER_CRC` sets `send_error_frame=false`, close immediately.
- **Error then continue**
  - e.g., `ERR_BAD_PAYLOAD_CRC`, `ERR_UNKNOWN_MSG_TYPE`,
    `ERR_UNKNOWN_COMMAND`, malformed command payload.
- **Error then close**
  - e.g., `ERR_UNSUPPORTED_VERSION`, `ERR_BAD_HEADER_LEN`,
    `ERR_PAYLOAD_TOO_LARGE`, auth/session failures.

## Security policy behavior

`SameUserSameSessionSecurityPolicy` returns `PipeSecurityDecision`:

- `kAllow`
- `kDeny` (with protocol error code/details)
- `kUnsupportedPlatform` (non-Windows fallback)
- `kInternalError`

Windows checks are performed before any command processing:

1. Reject remote clients.
2. Resolve client PID from named pipe.
3. Compare client `TokenUser` SID with server SID.
4. Compare client `TokenSessionId` with server session id.

On rejection, caller should emit `ERROR` when applicable and close according to
the contract.
