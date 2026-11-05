#pragma once
#include <string>
#include <vector>

#include "db.h"
#include "llm_provider.h"

namespace chat {

// keep_rounds <= 0 表示不限轮数
std::vector<llm::ChatMsg> buildContext(const db::Persona& persona,
                                       const std::vector<db::Message>& history,
                                       int keep_rounds,
                                       const std::string& newUser);

}  // namespace chat
