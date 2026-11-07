#pragma once
#include <string>

#include <httplib.h>

namespace srv {
// v5：authToken 非空时，对带 Origin 的 /api/v1/* 请求强制校验 X-Orua-Token（DNS-Rebinding 防护）。
// v0.5.2：cfgPath 用于插件增删后回写 config.json（空串则插件写接口返回 500）。
void registerRoutes(httplib::Server& svr, const std::string& staticDir,
                    const std::string& authToken, const std::string& cfgPath = "");
}  // namespace srv
