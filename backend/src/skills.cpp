#include "skills.h"

#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "db.h"
#include "logger.h"

namespace sk {
namespace {

// ---------- calculator：递归下降四则运算（无 eval） ----------
struct CalcParser {
  const std::string& s;
  size_t i = 0;

  explicit CalcParser(const std::string& str) : s(str) {}

  void skipWs() {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  }
  double parseExpr() {
    double v = parseTerm();
    for (;;) {
      skipWs();
      if (i < s.size() && s[i] == '+') { ++i; v += parseTerm(); }
      else if (i < s.size() && s[i] == '-') { ++i; v -= parseTerm(); }
      else break;
    }
    return v;
  }
  double parseTerm() {
    double v = parseFactor();
    for (;;) {
      skipWs();
      if (i < s.size() && s[i] == '*') { ++i; v *= parseFactor(); }
      else if (i < s.size() && s[i] == '/') {
        ++i;
        double d = parseFactor();
        if (d == 0.0) throw std::runtime_error("除数为零");
        v /= d;
      } else break;
    }
    return v;
  }
  double parseFactor() {
    skipWs();
    if (i >= s.size()) throw std::runtime_error("表达式不完整");
    if (s[i] == '(') {
      ++i;
      double v = parseExpr();
      skipWs();
      if (i >= s.size() || s[i] != ')') throw std::runtime_error("括号不匹配");
      ++i;
      return v;
    }
    if (s[i] == '-') { ++i; return -parseFactor(); }
    if (!std::isdigit(static_cast<unsigned char>(s[i])) && s[i] != '.')
      throw std::runtime_error("非法字符");
    size_t start = i;
    while (i < s.size() &&
           (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.'))
      ++i;
    try {
      return std::stod(s.substr(start, i - start));
    } catch (...) {
      throw std::runtime_error("数字解析失败");
    }
  }
};

json execCalculator(const json& params) {
  std::string expr;
  if (params.contains("expression") && params["expression"].is_string())
    expr = params["expression"].get<std::string>();
  if (expr.empty()) return {{"error", "表达式为空"}};
  try {
    CalcParser p(expr);
    double v = p.parseExpr();
    p.skipWs();
    if (p.i != p.s.size()) throw std::runtime_error("存在多余字符");
    return {{"result", v}};
  } catch (const std::exception& e) {
    return {{"error", std::string("计算失败：") + e.what()}};
  }
}

json execDatetime(const json&) {
  std::time_t t = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &t);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
  char date[32], weekday[32];
  std::strftime(date, sizeof(date), "%Y-%m-%d", &local);
  std::strftime(weekday, sizeof(weekday), "%A", &local);
  return {{"now", buf}, {"date", date}, {"weekday", weekday}};
}

json execSysinfo(const json&) {
  MEMORYSTATUSEX ms;
  ms.dwLength = sizeof(ms);
  if (!GlobalMemoryStatusEx(&ms)) return {{"error", "内存信息获取失败"}};
  ULARGE_INTEGER freeAvail, totalBytes, freeBytes;
  if (!GetDiskFreeSpaceExW(L"C:\\", &freeAvail, &totalBytes, &freeBytes))
    return {{"error", "磁盘信息获取失败"}};
  return {{"memory_total_mb", ms.ullTotalPhys / 1024 / 1024},
          {"memory_free_mb", ms.ullAvailPhys / 1024 / 1024},
          {"disk_free_gb", freeBytes.QuadPart / 1024 / 1024 / 1024}};
}

// 主机名归一化 + 内网/回环拒绝（SSRF 防护）：转小写 → 去单个尾点 → 逐条判定
bool endsWithAscii(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool isAllDigits(const std::string& s) {
  if (s.empty()) return false;
  for (unsigned char c : s)
    if (!std::isdigit(c)) return false;
  return true;
}

// IPv6 文本 → 16 位分组（支持一处 "::" 压缩）；非法形式返回 false（调用侧从严拒绝）
bool ipv6ToGroups(const std::string& h, std::vector<uint16_t>& out) {
  size_t dc = h.find("::");
  std::string left = dc == std::string::npos ? h : h.substr(0, dc);
  std::string right = dc == std::string::npos ? "" : h.substr(dc + 2);
  if (dc != std::string::npos && h.find("::", dc + 2) != std::string::npos) return false;
  auto fill = [](const std::string& chunk, std::vector<uint16_t>& dst) {
    if (chunk.empty()) return true;
    size_t pos = 0;
    while (pos < chunk.size()) {
      size_t nxt = chunk.find(':', pos);
      if (nxt == std::string::npos) nxt = chunk.size();
      std::string g = chunk.substr(pos, nxt - pos);
      if (g.empty() || g.size() > 4) return false;
      uint32_t v = 0;
      for (char ch : g) {
        char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!std::isxdigit(static_cast<unsigned char>(lc))) return false;
        v = v * 16 + static_cast<uint32_t>(lc >= '0' && lc <= '9' ? lc - '0' : lc - 'a' + 10);
      }
      dst.push_back(static_cast<uint16_t>(v));
      pos = nxt + 1;
    }
    return true;
  };
  std::vector<uint16_t> L, R;
  if (!fill(left, L) || !fill(right, R)) return false;
  size_t n = L.size() + R.size();
  if (dc == std::string::npos ? n != 8 : n > 7) return false;
  out = L;
  while (out.size() < 8 - R.size()) out.push_back(0);
  for (uint16_t v : R) out.push_back(v);
  return out.size() == 8;
}

// IPv6 字面量是否落在回环/未指定/内网段
bool isBlockedIpv6(const std::string& h) {
  std::vector<uint16_t> g;
  if (!ipv6ToGroups(h, g)) return true;  // 解析不了就不放行
  bool allZero = true;
  for (uint16_t v : g)
    if (v != 0) allZero = false;
  if (allZero) return true;                                     // :: 未指定地址
  bool headZero = g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0 && g[4] == 0;
  if (headZero && g[5] == 0 && (g[6] == 0 && (g[7] == 0 || g[7] == 1))) return true;  // :: / ::1
  if (headZero && g[5] == 0xffff) {                               // ::ffff:a.b.c → 按内嵌 IPv4 判定
    std::string v4 = std::to_string(g[6] >> 8) + "." + std::to_string(g[6] & 0xff) + "." +
                     std::to_string(g[7] >> 8) + "." + std::to_string(g[7] & 0xff);
    return isBlockedHost(v4);
  }
  if (headZero && g[5] == 0) return true;                         // ::a.b.c.d / 全零前缀兼容段
  return false;
}

// isBlockedHost 定义见本文件匿名命名空间之后（sk 命名空间内，便于 doctest 直接调用）

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string narrow(const std::wstring& s) {
  std::string out;
  for (wchar_t c : s) out += static_cast<char>(c & 0xFF);
  return out;
}

json execHttpGet(const json& params) {
  std::string url;
  if (params.contains("url") && params["url"].is_string())
    url = params["url"].get<std::string>();
  if (url.empty()) return {{"error", "url 为空"}};
  bool https = true;
  if (url.rfind("https://", 0) == 0) url = url.substr(8);
  else if (url.rfind("http://", 0) == 0) { url = url.substr(7); https = false; }
  else return {{"error", "仅支持 http/https"}};
  auto slash = url.find('/');
  std::string authority = slash == std::string::npos ? url : url.substr(0, slash);
  std::string path = slash == std::string::npos ? "/" : url.substr(slash);
  if (authority.empty()) return {{"error", "url 缺少主机名"}};
  auto at = authority.rfind('@');   // 丢弃 userinfo：校验与连接都只针对真实主机部分
  if (at != std::string::npos) authority = authority.substr(at + 1);
  if (authority.empty()) return {{"error", "url 缺少主机名"}};
  std::string host = authority;
  int port = https ? 443 : 80;
  if (authority.front() == '[') {                    // IPv6 字面量：[...]:port，不能用首个冒号切
    auto close = authority.find(']');
    if (close == std::string::npos) return {{"error", "url 主机部分非法"}};
    host = authority.substr(1, close - 1);
    std::string rest = authority.substr(close + 1);
    if (!rest.empty() && rest.front() == ':') {
      try { port = std::stoi(rest.substr(1)); } catch (...) {}
    }
  } else if (authority.find(':') != std::string::npos) {
    auto colon = authority.find(':');
    host = authority.substr(0, colon);
    try { port = std::stoi(authority.substr(colon + 1)); } catch (...) {}
  }
  if (isBlockedHost(host)) return {{"error", "拒绝访问本地/内网地址"}};
  bool ipv6Literal = host.find(':') != std::string::npos;
  std::string connectHost = ipv6Literal ? "[" + host + "]" : host;   // WinHTTP 要求带括号

  HINTERNET hSess = WinHttpOpen(L"OruaSkill/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSess) return {{"error", "WinHttpOpen 失败"}};
  // 禁用自动重定向：跟随 3xx 会让「只校验首个 URL」的内网拦截被 Location 绕过
  DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if (!WinHttpSetOption(hSess, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) {
    WinHttpCloseHandle(hSess);
    return {{"error", "禁用自动重定向失败"}};
  }
  WinHttpSetTimeouts(hSess, 3000, 3000, 10000, 10000);
  HINTERNET hConn = WinHttpConnect(hSess, widen(connectHost).c_str(), port, 0);
  HINTERNET hReq = hConn ? WinHttpOpenRequest(hConn, L"GET", widen(path).c_str(),
                                              nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              https ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
  if (!hConn || !hReq) {
    if (hReq) WinHttpCloseHandle(hReq);
    if (hConn) WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);
    return {{"error", "连接创建失败"}};
  }
  json out = {{"error", "请求失败"}};
  if (WinHttpSendRequest(hReq, nullptr, 0, nullptr, 0, 0, 0) &&
      WinHttpReceiveResponse(hReq, nullptr)) {
    DWORD code = 0, codeSize = sizeof(code);
    if (!WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize,
                             WINHTTP_NO_HEADER_INDEX))
      code = 0;
    std::string body;
    char buf[2048];
    DWORD read = 0, avail = 0;
    while (body.size() < 2048 && WinHttpQueryDataAvailable(hReq, &avail) && avail > 0 &&
           WinHttpReadData(hReq, buf, (std::min)(avail, (DWORD)sizeof(buf)), &read) &&
           read > 0)
      body.append(buf, read);
    if (body.size() > 2048) body.resize(2048);
    out = {{"status_code", code}, {"body", body}};
    if (code >= 300 && code < 400) {
      // 不跟随：把 Location 交给调用方，重定向目标同样不会被本技能访问
      wchar_t loc[512] = {};
      DWORD locSize = sizeof(loc) - sizeof(wchar_t);
      if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CUSTOM, L"Location", loc, &locSize,
                              WINHTTP_NO_HEADER_INDEX) && loc[0])
        out["location"] = narrow(std::wstring(loc, locSize / sizeof(wchar_t)));
    }
  }
  WinHttpCloseHandle(hReq);
  WinHttpCloseHandle(hConn);
  WinHttpCloseHandle(hSess);
  return out;
}

json execFileRead(const json& params) {
  std::string p;
  if (params.contains("path") && params["path"].is_string())
    p = params["path"].get<std::string>();
  if (p.empty()) return {{"error", "path 为空"}};
  std::ifstream ifs(p, std::ios::binary);
  if (!ifs) return {{"error", "文件打开失败"}};
  std::string content;
  char buf[4096];
  while (content.size() < 8192 && ifs.read(buf, sizeof(buf)))
    content.append(buf, ifs.gcount());
  if (ifs.gcount() > 0 && content.size() < 8192)
    content.append(buf, (std::min)(ifs.gcount(), (std::streamsize)(8192 - content.size())));
  return {{"content", content}};
}

json execReminder(const json& params) {
  std::string content;
  if (params.contains("content") && params["content"].is_string())
    content = params["content"].get<std::string>();
  std::string due;
  if (params.contains("due_at") && params["due_at"].is_string())
    due = params["due_at"].get<std::string>();
  if (content.empty() || due.empty()) return {{"error", "content/due_at 不能为空"}};
  try {
    int64_t botId = params.contains("bot_id") && params["bot_id"].is_number()
                        ? params["bot_id"].get<int64_t>()
                        : 0;
    auto r = db::addReminder(botId, content, due);
    if (r.id <= 0) return {{"error", "提醒写入失败"}};
    return {{"ok", true}, {"id", r.id}};
  } catch (const std::exception& e) {
    return {{"error", std::string("提醒写入失败：") + e.what()}};
  }
}

}  // namespace

// 主机名归一化 + 内网/回环拒绝（SSRF 防护）：转小写 → 去单个尾点 → 逐条判定。
// 放在 sk 命名空间（非匿名）里，测试可直接喂裸主机名；行为对同一输入恒定，无副作用。
bool isBlockedHost(const std::string& host) {
  std::string h;
  for (unsigned char c : host) h += static_cast<char>(std::tolower(c));
  if (!h.empty() && h.back() == '.') h.pop_back();   // FQDN 尾点（Localhost. → localhost）
  if (h.empty()) return true;
  if (h == "localhost" || endsWithAscii(h, ".localhost")) return true;
  if (h.front() == '[') {            // [::1] / [::1]:8765：端口只在右括号之后，不能按末冒号切
    auto close = h.find(']');
    if (close == std::string::npos) return true;                    // 方括号不成对 → 从严
    std::string bare = h.substr(1, close - 1);
    std::string rest = h.substr(close + 1);
    if (!rest.empty() && rest.front() != ':') return true;          // ] 后还有杂项 → 从严
    return isBlockedIpv6(bare);
  }
  if (h.back() == ']') return true;                                 // 无左括号的畸形形式
  if (h.find(':') != std::string::npos) return isBlockedIpv6(h);   // 无括号的 IPv6 字面量
  if (isAllDigits(h)) return true;                                 // 十进制简写 2130706433
  if (h.rfind("0x", 0) == 0) return true;                          // 十六进制简写 0x7f000001
  static const char* kPrefixes[] = {"127.", "10.", "192.168.", "169.254.", "0."};
  for (const char* p : kPrefixes)
    if (h.rfind(p, 0) == 0) return true;
  if (h.rfind("172.", 0) == 0) {
    auto dot = h.find('.', 4);
    if (dot == std::string::npos) return true;       // "172" 之类不完整形式从严
    int part = 0;
    try {
      part = std::stoi(h.substr(4, dot - 4));
    } catch (...) {
      return true;
    }
    if (part >= 16 && part <= 31) return true;
  }
  // 数值点分形式里的前导零段（0177.0.0.1 这类八进制简写）不是合法 DNS 名，一律拒绝
  if (h.find('.') != std::string::npos) {
    bool allNumeric = true;
    for (char c : h)
      if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') allNumeric = false;
    if (allNumeric) {
      size_t pos = 0;
      while (pos < h.size()) {
        size_t nxt = h.find('.', pos);
        if (nxt == std::string::npos) nxt = h.size();
        std::string seg = h.substr(pos, nxt - pos);
        if (seg.size() > 1 && seg.front() == '0') return true;
        pos = nxt + 1;
      }
    }
  }
  return false;
}

std::vector<SkillDef> registry() {
  return {
      {"calculator", "计算器", "四则运算表达式求值（仅支持 + - * / 括号与数字）",
       json{{"type", "object"},
            {"properties", json{{"expression", json{{"type", "string"}}}}},
            {"required", json::array({"expression"})}}},
      {"datetime", "时间日期", "查询当前时间、日期与星期",
       json{{"type", "object"}, {"properties", json::object()}}},
      {"sysinfo", "系统信息", "查询内存与磁盘空间占用",
       json{{"type", "object"}, {"properties", json::object()}}},
      {"http_get", "HTTP 请求",
       "GET 指定公网网页并返回正文摘要（≤2KB，拒绝 localhost/内网段/回环与数字简写主机，"
       "不跟随重定向）",
       json{{"type", "object"},
            {"properties", json{{"url", json{{"type", "string"}}}}},
            {"required", json::array({"url"})}}},
      {"file_read", "读取文件", "读取本地文本文件（≤8KB）",
       json{{"type", "object"},
            {"properties", json{{"path", json{{"type", "string"}}}}},
            {"required", json::array({"path"})}}},
      {"reminder", "定时提醒", "设置一条定时提醒",
       json{{"type", "object"},
            {"properties",
             json{{"content", json{{"type", "string"}}},
                  {"due_at", json{{"type", "string"},
                                  {"description", "ISO 8601 时间字符串"}}}}},
            {"required", json::array({"content", "due_at"})}}},
  };
}

json execute(const std::string& id, const json& params) {
  if (!params.is_object()) return {{"error", "参数必须为 JSON 对象"}};
  if (id == "calculator") return execCalculator(params);
  if (id == "datetime") return execDatetime(params);
  if (id == "sysinfo") return execSysinfo(params);
  if (id == "http_get") return execHttpGet(params);
  if (id == "file_read") return execFileRead(params);
  if (id == "reminder") return execReminder(params);
  return {{"error", "未知技能：" + id}};
}

}  // namespace sk
