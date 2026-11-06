#pragma once
#include <string>
#include <vector>

#include "json_util.h"

namespace sk {

struct SkillDef {
  std::string id;
  std::string name;
  std::string description;
  json params_schema;
};

std::vector<SkillDef> registry();
json execute(const std::string& id, const json& params);   // 永不抛异常；失败 {"error":...}

// SSRF 判定纯函数：主机名归一化（小写/去尾点）后，本地/内网/回环/数字简写返回 true。
// 暴露出来是为了让 doctest 能直接喂裸主机名，不必真的发起网络请求。
bool isBlockedHost(const std::string& host);

}  // namespace sk
