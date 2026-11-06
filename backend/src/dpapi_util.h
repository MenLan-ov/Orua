#pragma once
#include <string>

namespace dpapi {
std::string protect(const std::string& plain);   // 返回 Base64
std::string unprotect(const std::string& b64);   // 失败抛 std::runtime_error
}  // namespace dpapi
