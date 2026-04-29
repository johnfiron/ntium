#include "src/app/RuntimeGraph.h"

#include <utility>

namespace ntium::app {

RuntimeGraphResult MakeRuntimeGraphResult(
    RuntimeGraphStatusCode code,
    std::string node_name,
    std::string detail) {
  RuntimeGraphResult result;
  result.code = code;
  result.node_name = std::move(node_name);
  result.detail = std::move(detail);
  return result;
}

const char* ToString(RuntimeGraphStatusCode code) {
  switch (code) {
    case RuntimeGraphStatusCode::kOk:
      return "ok";
    case RuntimeGraphStatusCode::kInvalidNode:
      return "invalid_node";
    case RuntimeGraphStatusCode::kDuplicateNode:
      return "duplicate_node";
    case RuntimeGraphStatusCode::kInitializeFailed:
      return "initialize_failed";
    case RuntimeGraphStatusCode::kShutdownFailed:
      return "shutdown_failed";
  }
  return "unknown";
}

RuntimeGraphResult RuntimeGraph::AddNode(RuntimeGraphNode node) {
  if (node.name.empty() || !node.initialize || !node.shutdown) {
    return MakeRuntimeGraphResult(
        RuntimeGraphStatusCode::kInvalidNode, std::move(node.name),
        "node requires name + initialize + shutdown callbacks");
  }

  for (const RuntimeGraphNode& existing : nodes_) {
    if (existing.name == node.name) {
      return MakeRuntimeGraphResult(
          RuntimeGraphStatusCode::kDuplicateNode, node.name,
          "node name already registered");
    }
  }

  nodes_.push_back(std::move(node));
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult RuntimeGraph::Initialize() {
  initialized_node_indices_.clear();
  initialized_ = false;

  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    RuntimeGraphResult node_result = nodes_[index].initialize();
    if (!node_result.ok()) {
      if (node_result.node_name.empty()) {
        node_result.node_name = nodes_[index].name;
      }
      const RuntimeGraphResult rollback_result = ShutdownInitializedNodes();
      if (!rollback_result.ok() && node_result.detail.empty()) {
        node_result.detail = "rollback failure after init error";
      }
      return node_result;
    }
    initialized_node_indices_.push_back(index);
  }

  initialized_ = true;
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult RuntimeGraph::Shutdown() {
  const RuntimeGraphResult result = ShutdownInitializedNodes();
  initialized_ = false;
  return result;
}

std::vector<std::string> RuntimeGraph::initialized_nodes() const {
  std::vector<std::string> names;
  names.reserve(initialized_node_indices_.size());
  for (const std::size_t index : initialized_node_indices_) {
    if (index < nodes_.size()) {
      names.push_back(nodes_[index].name);
    }
  }
  return names;
}

RuntimeGraphResult RuntimeGraph::ShutdownInitializedNodes() {
  RuntimeGraphResult first_error = MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
  for (std::size_t idx = initialized_node_indices_.size(); idx > 0; --idx) {
    const std::size_t node_index = initialized_node_indices_[idx - 1U];
    RuntimeGraphResult node_result = nodes_[node_index].shutdown();
    if (!node_result.ok() && first_error.ok()) {
      if (node_result.node_name.empty()) {
        node_result.node_name = nodes_[node_index].name;
      }
      first_error = std::move(node_result);
    }
  }
  initialized_node_indices_.clear();
  return first_error;
}

}  // namespace ntium::app
