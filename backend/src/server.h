#pragma once
#include <string>

#include <httplib.h>

namespace srv {
// v5：authToken 非空时，对带 Origin 的 /api/v1/* 请求强制校验 X-Orua-Token（DNS-Rebinding 防护）。
void registerRoutes(httplib::Server& svr, const std::string& staticDir,
                    const std::string& authToken);
}  // namespace srv
