#include "chat_service.h"

namespace chat {

std::vector<llm::ChatMsg> buildContext(const db::Persona& persona,
                                       const std::vector<db::Message>& history,
                                       int keep_rounds,
                                       const std::string& newUser) {
  std::vector<llm::ChatMsg> out;
  out.push_back({"system", persona.system_prompt});

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
