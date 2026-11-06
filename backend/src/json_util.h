#pragma once
#include <chrono>
#include <ctime>
#include <string>
#include "json.hpp"

using json = nlohmann::json;

inline json okJson(const json& data) { return json{{"code", 0}, {"data", data}}; }
inline json errJson(int code, const std::string& message) {
  return json{{"code", code}, {"message", message}};
}
inline std::string nowIso() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_s(&tm, &t);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}
// 按 Unicode 码点安全截断（中文 3 字节不会截断出半个字符）
inline std::string utf8Truncate(const std::string& s, size_t maxChars) {
  size_t n = 0, i = 0;
  while (i < s.size() && n < maxChars) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    i += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
    ++n;
  }
  return s.substr(0, i);
}
