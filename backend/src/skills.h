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

}  // namespace sk
