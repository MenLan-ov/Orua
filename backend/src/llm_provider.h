#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "json_util.h"

namespace llm {

struct ChatMsg {
  std::string role;     // "system" | "user" | "assistant" | "tool"
  std::string content;
  json extra = json::object();   // assistant: {"tool_calls":[...]}；tool: {"tool_call_id":...}
};
struct ChatRequest {
  std::vector<ChatMsg> messages;
  double temperature = 0.8;
  std::string base_url;
  std::string api_key;
  std::string model;
  bool proxy_enabled = false;
  std::string proxy_host;
  int proxy_port = 0;
  int max_tokens = 0;
  json tools = json::array();
};
struct StreamEvent {
  std::string type;        // "delta" | "error" | "tool_calls"
  std::string content;
  int error_code = 0;
  std::string message;
  std::string data;        // type=="tool_calls" 时为完整 tool_calls JSON 数组字符串
};
using StreamCallback = std::function<void(const StreamEvent&)>;

// 流式 tool_calls 分片聚合：按 index 聚合 id/type/function.name/function.arguments
class ToolCallAccumulator {
 public:
  void feed(const json& delta);
  json finish();
 private:
  json out_ = json::array();
};

class ILlmProvider {
 public:
  virtual ~ILlmProvider() = default;
  virtual void streamChat(const ChatRequest& req, StreamCallback cb,
                          const std::atomic_bool& stop) = 0;
};

}  // namespace llm
