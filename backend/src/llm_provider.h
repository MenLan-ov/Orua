#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace llm {

struct ChatMsg {
  std::string role;     // "system" | "user" | "assistant"
  std::string content;
};
struct ChatRequest {
  std::vector<ChatMsg> messages;
  double temperature = 0.8;
  int max_tokens = 0;
  std::string base_url;    // 如 https://api.deepseek.com/v1
  std::string api_key;
  std::string model;
  bool proxy_enabled = false;
  std::string proxy_host;  // 为空 = 使用系统代理（注册表）
  int proxy_port = 0;
};
struct StreamEvent {
  std::string type;        // "delta" | "error"
  std::string content;
  int error_code = 0;
  std::string message;
};
using StreamCallback = std::function<void(const StreamEvent&)>;

class ILlmProvider {
 public:
  virtual ~ILlmProvider() = default;
  // 同步执行；期间持续回调 delta；出错回调 error；stop 置位后尽快返回
  virtual void streamChat(const ChatRequest& req, StreamCallback cb,
                          const std::atomic_bool& stop) = 0;
};

}  // namespace llm
