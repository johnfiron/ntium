# ETW Event Schema for Performance Instrumentation

- **Tickets:** T1-801, T1-802
- **Audience:** Runtime, render, ingest, and control subsystem owners
- **Goal:** Standardize ETW events/counters required by the Batch 3 harness and gate checker.

## 1) Provider definitions

Use two providers to separate app-level semantics from runtime support:

1. **Primary app provider (required)**
   - Name: `Acme.App.Perf`
   - Type: manifest or TraceLogging provider
   - Level usage:
     - `Informational`: normal perf events
     - `Warning`: throttling/overrun indicators
     - `Error`: dropped work / unrecoverable pipeline faults

2. **OS/system providers (captured via WPR profile)**
   - CPU scheduling, context switches
   - Disk I/O (if content ingest includes file reads)
   - GPU/DXGI/DWM providers where available

## 2) Common event envelope

Every app perf event should include:

- `timestamp_qpc` (uint64)
- `scenario` (string) — `baseline|idle|active|burst`
- `session_id` (guid/string)
- `thread_id` (uint32)
- `activity_id` (guid, optional but recommended)
- `event_version` (uint16)

Additional event-specific fields are listed below.

## 3) Pipeline event taxonomy

### 3.1 Render pipeline events (required)

1. `Render.FrameScheduled`
   - `frame_id` (uint64)
   - `reason` (string enum: `invalidate|resize|topology|recovery|input`)

2. `Render.FrameStart`
   - `frame_id` (uint64)

3. `Render.FrameEnd`
   - `frame_id` (uint64)
   - `duration_ms` (float)
   - `presented` (bool)

4. `Render.Present`
   - `frame_id` (uint64)
   - `present_result` (string enum: `ok|occluded|device_lost|dropped|skipped_idle`)
   - `queue_depth` (uint32)

### 3.2 Ingest pipeline events (required)

1. `Ingest.JobQueued`
   - `job_id` (uint64)
   - `queue_depth` (uint32)
   - `source` (string enum: `file|ipc|network|other`)

2. `Ingest.JobStart`
   - `job_id` (uint64)
   - `queue_depth` (uint32)

3. `Ingest.JobEnd`
   - `job_id` (uint64)
   - `duration_ms` (float)
   - `status` (string enum: `ok|cancelled|failed`)

4. `Ingest.QueueDepthSample`
   - `queue_depth` (uint32)

### 3.3 Control pipeline events (required)

1. `Control.TickStart`
   - `tick_id` (uint64)

2. `Control.TickEnd`
   - `tick_id` (uint64)
   - `duration_ms` (float)

3. `Control.InputEvent`
   - `event_id` (uint64)
   - `kind` (string enum: `pointer|keyboard|wheel|gesture|automation`)

4. `Control.BurstThrottle`
   - `window_id` (uint64)
   - `received_events` (uint32)
   - `throttled_events` (uint32)
   - `window_ms` (float)

## 4) Derived counters required by gates

The harness parser must produce these scenario aggregates:

- `idle_cpu_percent_total`
  - Derived from process CPU samples during scenario `idle`.
- `idle_render_present_rate_hz`
  - Count of `Render.Present` with `present_result=ok` divided by idle window duration.
- `idle_pipeline_activity_per_sec`
  - Combined count rate of:
    - `Render.FrameStart`
    - `Ingest.JobStart`
    - `Control.TickStart`
- `active_p95_frame_ms`, `active_p99_frame_ms`
  - Percentiles over `Render.FrameEnd.duration_ms` for scenario `active`.
- `burst_input_events`
  - Count or sum from `Control.InputEvent` within `burst` scenario.
- `burst_throttled_events`
  - Sum of `Control.BurstThrottle.throttled_events` within `burst`.
- `burst_throttle_ratio`
  - `burst_throttled_events / burst_input_events`.
- `ingest_queue_peak_depth`
  - Maximum observed `queue_depth` from ingest queue events.
- `control_loop_p95_ms`
  - p95 of `Control.TickEnd.duration_ms`.

## 5) Correlation guidance

- Use `activity_id` to correlate ingest/control work to render invalidations where possible.
- `frame_id`, `job_id`, `tick_id` must be monotonically increasing per process.
- Scenario markers should be emitted at start/end of each harness phase:
  - `Perf.ScenarioStart {scenario}`
  - `Perf.ScenarioEnd {scenario}`

## 6) Versioning and compatibility

- Increment `event_version` when a field changes meaning.
- Additive fields are preferred; avoid breaking removals.
- Parser/checker should ignore unknown fields and require only documented minimum schema.

## 7) Minimal example events

```text
Perf.ScenarioStart scenario=idle
Render.FrameStart frame_id=510
Render.FrameEnd frame_id=510 duration_ms=2.1 presented=false
Render.Present frame_id=510 present_result=skipped_idle queue_depth=0
Control.BurstThrottle window_id=12 received_events=120 throttled_events=92 window_ms=250
Perf.ScenarioEnd scenario=idle
```
