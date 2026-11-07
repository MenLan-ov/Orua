#pragma once
#include <cstdint>
#include <random>
#include <string>

// 参数边界校验与安全令牌生成。纯头文件，backend 与 backend_tests 共用，
// 使验证/令牌逻辑无需把 server.cpp（仅 backend 编译）拖进测试目标。

namespace val {

// 数值边界：越界一律收窄到最近端（clamp），绝不抛异常。
inline int clampMaxTokens(int64_t v) {
  if (v < 1) return 1;
  if (v > 65536) return 65536;
  return static_cast<int>(v);
}

inline double clampTemperature(double v) {
  if (v < 0.0) return 0.0;
  if (v > 2.0) return 2.0;
  return v;
}

inline int clampKeepRounds(int64_t v) {
  if (v < 0) return 0;      // 0 = 不带历史（仅当前轮）
  if (v > 100) return 100;
  return static_cast<int>(v);
}

inline int clampProxyPort(int64_t v) {
  if (v <= 0) return 0;     // 0 = 禁用代理
  if (v > 65535) return 65535;
  return static_cast<int>(v);
}

// 每启动一次的后端本地鉴权令牌：32 字节随机熵，URL-safe base64（无 = 填充）。
// 使用 crypto 级随机源；退化到 mt19937 时经互斥保护（极低概率，仅兜底）。
inline std::string randomToken() {
  unsigned char bytes[32];
  std::random_device rd;
  for (auto& b : bytes) b = static_cast<unsigned char>(rd());
  static const char* kTbl =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve(43);
  unsigned int acc = 0;
  int bits = 0;
  for (unsigned char b : bytes) {
    acc = (acc << 8) | b;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out.push_back(kTbl[(acc >> bits) & 0x3F]);
    }
  }
  if (bits > 0) out.push_back(kTbl[(acc << (6 - bits)) & 0x3F]);
  return out;
}

}  // namespace val