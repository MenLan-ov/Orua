#include "chat_service.h"

namespace chat {

std::vector<llm::ChatMsg> buildContext(const std::string& systemPrompt,
                                       const std::vector<db::Message>& history,
                                       int keep_rounds,
                                       const std::string& newUser,
                                       const std::vector<db::Memory>& memories) {
  std::vector<llm::ChatMsg> out;
  out.push_back({"system", systemPrompt});
  if (!memories.empty()) {
    std::string block = "[长期记忆]\n";
    for (const auto& m : memories) block += "· " + m.content + "\n";
    out.push_back({"system", block});
  }

  // 定位截断起点：历史中倒数第 keep_rounds 条 user 消息的下标
  std::vector<size_t> userIdx;
  for (size_t i = 0; i < history.size(); ++i)
    if (history[i].role == "user") userIdx.push_back(i);
  size_t start = 0;
  if (keep_rounds > 0 && userIdx.size() > static_cast<size_t>(keep_rounds))
    start = userIdx[userIdx.size() - keep_rounds];

  for (size_t i = start; i < history.size(); ++i) {
    if (history[i].role == "user" || history[i].role == "assistant")
      out.push_back({history[i].role, history[i].content});
  }
  if (!newUser.empty()) out.push_back({"user", newUser});
  return out;
}

}  // namespace chat
