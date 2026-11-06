#pragma once
#include <functional>
#include <string>

#include "llm_provider.h"

namespace llm {

// OpenAI 兼容 SSE 流解析器：行缓冲 + 前缀剥离，兼容各厂商差异
// （data 换行位置不定、[DONE] 位置不定、usage 等额外事件宽容跳过）
class SseParser {
 public:
  std::function<void(const std::string& event, const std::string& data)> onEvent;
  void feed(const std::string& chunk);
  void finish();   // 收尾：flush 未派发的最后事件

 private:
  std::string buf_;
  std::string eventName_ = "message";
  std::string data_;
  bool hasData_ = false;
};

class OpenAiCompatProvider : public ILlmProvider {
 public:
  void streamChat(const ChatRequest& req, StreamCallback cb,
                  const std::atomic_bool& stop) override;
};

// 测试可见性：离线断言请求体（tools/extra 透传）
std::string buildBodyForTest(const ChatRequest& req);

}  // namespace llm
