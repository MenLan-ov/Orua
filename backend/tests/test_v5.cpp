#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>

#include "skills.h"
#include "validation.h"

TEST_CASE("v5: max_tokens 边界收窄到 [1,65536]") {
  CHECK(val::clampMaxTokens(0) == 1);
  CHECK(val::clampMaxTokens(-100) == 1);
  CHECK(val::clampMaxTokens(65536) == 65536);
  CHECK(val::clampMaxTokens(65537) == 65536);
  CHECK(val::clampMaxTokens(999999) == 65536);
  CHECK(val::clampMaxTokens(2048) == 2048);
}

TEST_CASE("v5: temperature 边界收窄到 [0,2]") {
  CHECK(val::clampTemperature(-0.5) == doctest::Approx(0.0));
  CHECK(val::clampTemperature(0.0) == doctest::Approx(0.0));
  CHECK(val::clampTemperature(2.0) == doctest::Approx(2.0));
  CHECK(val::clampTemperature(2.5) == doctest::Approx(2.0));
  CHECK(val::clampTemperature(0.8) == doctest::Approx(0.8));
}

TEST_CASE("v5: keep_rounds 边界收窄到 [0,100]") {
  CHECK(val::clampKeepRounds(-1) == 0);
  CHECK(val::clampKeepRounds(0) == 0);
  CHECK(val::clampKeepRounds(100) == 100);
  CHECK(val::clampKeepRounds(101) == 100);
  CHECK(val::clampKeepRounds(20) == 20);
}

TEST_CASE("v5: proxy_port 边界收窄到 [0,65535]") {
  CHECK(val::clampProxyPort(-1) == 0);
  CHECK(val::clampProxyPort(0) == 0);
  CHECK(val::clampProxyPort(65535) == 65535);
  CHECK(val::clampProxyPort(65536) == 65535);
  CHECK(val::clampProxyPort(8080) == 8080);
}

TEST_CASE("v5: randomToken 唯一、长度稳定、仅 URL-safe 字符") {
  std::string a = val::randomToken();
  std::string b = val::randomToken();
  CHECK_FALSE(a.empty());
  CHECK(a != b);
  CHECK(a.size() == 43);
  for (char c : a) {
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_';
    CHECK(ok);
  }
}

TEST_CASE("v5: file_read 白名单放行系统临时目录") {
  auto tmp = std::filesystem::temp_directory_path() / "ac_v5_allow.txt";
  std::ofstream f(tmp);
  f << "hello";
  f.close();
  CHECK(sk::fileReadAllowed(tmp.string()));
  std::filesystem::remove(tmp);
}

TEST_CASE("v5: file_read 白名单拒绝工作区外任意路径") {
  // Windows 上常见的"系统目录之外的受保护路径"：C:\Windows\System32
  CHECK_FALSE(sk::fileReadAllowed("C:\\Windows\\System32\\drivers\\etc\\hosts"));
}

TEST_CASE("v5: isBlockedHost 对 IPv4 内网/回环/私网判定") {
  CHECK(sk::isBlockedHost("127.0.0.1"));
  CHECK(sk::isBlockedHost("10.0.0.5"));
  CHECK(sk::isBlockedHost("192.168.1.1"));
  CHECK(sk::isBlockedHost("172.16.0.1"));
  CHECK(sk::isBlockedHost("169.254.0.2"));
  CHECK(sk::isBlockedHost("0.0.0.0"));
  CHECK(sk::isBlockedHost("8.8.8.8") == false);
}

TEST_CASE("v5: isBlockedHost 对主机名/十进制简写判定") {
  CHECK(sk::isBlockedHost("localhost"));
  CHECK(sk::isBlockedHost("2130706433"));   // 127.0.0.1 十进制简写
  CHECK(sk::isBlockedHost("::1"));
  CHECK(sk::isBlockedHost("fe80::1"));
  CHECK(sk::isBlockedHost("example.com") == false);
}