#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ntium::app {

enum class RuntimeGraphStatusCode : std::uint8_t {
  kOk = 0U,
  kInvalidNode = 1U,
  kDuplicateNode = 2U,
  kInitializeFailed = 3U,
  kShutdownFailed = 4U,
};

struct RuntimeGraphResult {
  RuntimeGraphStatusCode code = RuntimeGraphStatusCode::kOk;
  std::string node_name;
  std::string detail;

  bool ok() const { return code == RuntimeGraphStatusCode::kOk; }
};

struct RuntimeGraphNode {
  std::string name;
  std::function<RuntimeGraphResult()> initialize;
  std::function<RuntimeGraphResult()> shutdown;
};

RuntimeGraphResult MakeRuntimeGraphResult(
    RuntimeGraphStatusCode code,
    std::string node_name = {},
    std::string detail = {});

const char* ToString(RuntimeGraphStatusCode code);

class RuntimeGraph {
 public:
  RuntimeGraphResult AddNode(RuntimeGraphNode node);
  RuntimeGraphResult Initialize();
  RuntimeGraphResult Shutdown();

  std::vector<std::string> initialized_nodes() const;
  bool initialized() const { return initialized_; }

 private:
  RuntimeGraphResult ShutdownInitializedNodes();

  std::vector<RuntimeGraphNode> nodes_;
  std::vector<std::size_t> initialized_node_indices_;
  bool initialized_ = false;
};

}  // namespace ntium::app
