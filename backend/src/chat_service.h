#pragma once
#include <string>
#include <vector>

#include "db.h"
#include "llm_provider.h"

namespace chat {

// keep_rounds <= 0 表示不限轮数；memories 非空时在 system prompt 后注入 [长期记忆] 块
std::vector<llm::ChatMsg> buildContext(const std::string& systemPrompt,
                                       const std::vector<db::Message>& history,
                                       int keep_rounds,
                                       const std::string& newUser,
                                       const std::vector<db::Memory>& memories = {});

}  // namespace chat
