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

// due_at 归一化纯函数：绝对 ISO 8601（含 Z / ±HH:MM / 无标记按本地时间）或相对写法
// （15分钟后 / in 15 minutes / +15m / 15 / 2h）→ 与 nowIso() 兼容的 UTC 字符串；
// 无法解析返回空串（调用方据此拒绝写库）。
std::string normalizeDueAt(const std::string& in);

}  // namespace sk
