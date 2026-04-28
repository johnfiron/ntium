#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "src/render/DirtyRectCoalescer.h"
#include "src/render/RenderTypes.h"

namespace render {

struct DirtyPresentPipelineConfig {
  DirtyRectCoalescerConfig coalescer = {};
};

struct DirtyPresentPipelineUpdate {
  RenderStatus status = RenderStatus::kOk;
  uint64_t event_sequence = 0;
  bool has_effective_work = false;
  DirtyFullFrameFallbackReason batch_fallback_reason =
      DirtyFullFrameFallbackReason::kNone;
  std::vector<MonitorIdentity> touched_monitors;
};

// Event-driven dirty present pipeline:
// - maps dirty events into monitor-local rect sets
// - accumulates pending per-monitor present intents
// - exposes drain/consume APIs for scheduler-driven present execution
class IDirtyPresentPipeline {
 public:
  virtual ~IDirtyPresentPipeline() = default;

  virtual RenderStatus SetActiveMonitors(
      const std::vector<MonitorLayout>& active_monitors,
      uint64_t event_sequence) = 0;

  virtual RenderStatus QueueDirtyEvent(
      const DirtyPresentEvent& event,
      DirtyPresentPipelineUpdate* out_update) = 0;

  virtual RenderStatus BuildPendingPresentIntents(
      std::vector<PresentIntent>* out_intents) const = 0;

  virtual RenderStatus ConsumePresentIntent(
      const MonitorIdentity& monitor,
      PresentIntent* out_intent) = 0;

  virtual bool HasPendingWork() const = 0;
  virtual std::vector<MonitorIdentity> ListPendingMonitors() const = 0;
  virtual DirtyRectCoalescerConfig coalescer_config() const = 0;
};

std::unique_ptr<IDirtyPresentPipeline> CreateDirtyPresentPipeline(
    DirtyPresentPipelineConfig config = {});

}  // namespace render
