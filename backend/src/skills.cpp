#include "skills.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

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
  if ((g[0] & 0xffc0) == 0xfe80) return true;                   // fe80::/10 链路本地
  if ((g[0] & 0xfe00) == 0xfc00) return true;                   // fc00::/7 唯一本地（私网）
  if ((g[0] & 0xffc0) == 0xfec0) return true;                   // fec0::/10 站点本地（弃用仍拒）
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
  // v5：主机名通过后仍解析+逐 IP 复检，封死 DNS Rebinding
  if (!resolveAndRecheck(host)) return {{"error", "拒绝访问本地/内网地址"}};
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
  // v5：文件读取白名单（data 目录 / 系统临时目录 / 设置表追加根目录），封死任意路径读取
  if (!fileReadAllowed(p)) return {{"error", "路径不在允许读取的目录内"}};
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
  std::string rawDue;
  if (params.contains("due_at")) {
    // LLM 也可能把「30 分钟后」写成数字 30：整数按分钟数处理，与纯数字形式同义
    if (params["due_at"].is_string()) rawDue = params["due_at"].get<std::string>();
    else if (params["due_at"].is_number_integer())
      rawDue = std::to_string(params["due_at"].get<int64_t>());
  }
  if (content.empty() || rawDue.empty()) return {{"error", "content/due_at 不能为空"}};
  // §7.2「解析时间（相对分钟或绝对时间）」：入库前归一化成 UTC ISO，
  // 否则 db::listReminders 的 due_at <= nowIso() 字符串比较永远不成立
  std::string due = normalizeDueAt(rawDue);
  if (due.empty())
    return {{"error", "due_at 无法解析（支持 ISO 8601 或相对分钟，如 15分钟后）"}};
  try {
    int64_t botId = params.contains("bot_id") && params["bot_id"].is_number()
                        ? params["bot_id"].get<int64_t>()
                        : 0;
    auto r = db::addReminder(botId, content, due);
    if (r.id <= 0) return {{"error", "提醒写入失败"}};
    return {{"ok", true}, {"id", r.id}, {"due_at", due}};
  } catch (const std::exception& e) {
    return {{"error", std::string("提醒写入失败：") + e.what()}};
  }
}

// ---------- due_at 解析辅助（纯计算，无副作用）----------
std::string trimCopy(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                   s[e - 1] == '\n'))
    --e;
  return s.substr(b, e - b);
}

std::string asciiLower(const std::string& s) {
  std::string out;
  for (unsigned char c : s)
    out += static_cast<char>(c < 0x80 ? std::tolower(c) : c);   // 非 ASCII 字节原样保留
  return out;
}

bool readFixedDigits(const std::string& s, size_t& i, int n, int& out) {
  if (i + static_cast<size_t>(n) > s.size()) return false;
  int v = 0;
  for (int k = 0; k < n; ++k) {
    if (!std::isdigit(static_cast<unsigned char>(s[i + k]))) return false;
    v = v * 10 + (s[i + k] - '0');
  }
  i += static_cast<size_t>(n);
  out = v;
  return true;
}

bool leapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

bool validCivil(int y, int mo, int d, int h, int mi, int sec) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (y < 1970 || y > 9999) return false;
  if (mo < 1 || mo > 12 || d < 1) return false;
  int lim = kDays[mo - 1] + (mo == 2 && leapYear(y) ? 1 : 0);
  if (d > lim) return false;
  return h >= 0 && h <= 23 && mi >= 0 && mi <= 59 && sec >= 0 && sec <= 59;
}

// 民历日 → Unix 纪元日数（Howard Hinnant days_from_civil，不依赖 CRT 时区）
int64_t daysFromCivil(int y, int mo, int d) {
  y -= mo <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned mp = static_cast<unsigned>(mo + (mo > 2 ? -3 : 9));
  const unsigned doy = (153 * mp + 2) / 5 + static_cast<unsigned>(d) - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

// 与 json_util.h 的 nowIso() 逐字符同格式：%Y-%m-%dT%H:%M:%SZ（UTC、秒、尾 Z）
std::string isoFromEpoch(std::time_t t) {
  std::tm tm{};
  if (gmtime_s(&tm, &t) != 0) return {};
  char buf[32];
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) return {};
  return buf;
}

bool inEpochRange(int64_t epoch) { return epoch >= 0 && epoch < 4102444800LL; }  // 1970..2100

// 绝对时间：YYYY-MM-DD[T ]HH:MM[:SS][.fff][Z|±HH:MM]，无时区标记按本地时间→UTC
bool parseAbsoluteDue(const std::string& s, std::time_t& out) {
  size_t i = 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  if (!readFixedDigits(s, i, 4, y)) return false;
  if (i >= s.size() || s[i] != '-') return false;
  ++i;
  if (!readFixedDigits(s, i, 2, mo)) return false;
  if (i >= s.size() || s[i] != '-') return false;
  ++i;
  if (!readFixedDigits(s, i, 2, d)) return false;
  if (i < s.size() && (s[i] == 'T' || s[i] == 't' || s[i] == ' ')) {
    ++i;
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    if (!readFixedDigits(s, i, 2, h)) return false;      // 至少到分
    if (i >= s.size() || s[i] != ':') return false;
    ++i;
    if (!readFixedDigits(s, i, 2, mi)) return false;
    if (i < s.size() && s[i] == ':') {
      ++i;
      if (!readFixedDigits(s, i, 2, sec)) return false;
    }
    if (i < s.size() && s[i] == '.') {                   // 小数秒：吃掉但不参与
      ++i;
      size_t st = i;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
      if (i == st) return false;
    }
  }
  if (!validCivil(y, mo, d, h, mi, sec)) return false;
  int64_t epoch = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
  if (i < s.size()) {
    if (s[i] == 'Z' || s[i] == 'z') {                    // 显式 UTC
      ++i;
    } else if (s[i] == '+' || s[i] == '-') {             // 显式偏移
      char sign = s[i];
      ++i;
      int oh = 0, om = 0;
      if (!readFixedDigits(s, i, 2, oh)) return false;
      if (i < s.size() && s[i] == ':') ++i;
      if (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        if (!readFixedDigits(s, i, 2, om)) return false;
      }
      if (oh > 23 || om > 59) return false;
      int64_t off = oh * 3600 + om * 60;
      epoch -= (sign == '-' ? -off : off);
    } else {
      return false;
    }
    if (i != s.size()) return false;                     // 尾部有杂项
    if (!inEpochRange(epoch)) return false;
    out = static_cast<std::time_t>(epoch);
    return true;
  }
  // 无时区标记：按本地时间解释后转 UTC（mktime 依系统时区/DST 换算，再 gmtime_s 出 UTC 串）
  std::tm t{};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_sec = sec;
  t.tm_isdst = -1;
  std::time_t local = std::mktime(&t);
  if (local == static_cast<std::time_t>(-1)) return false;
  if (!inEpochRange(static_cast<int64_t>(local))) return false;
  out = static_cast<std::time_t>(local);
  return true;
}

// 相对时间：N分钟后 / N 分钟后 / in N minutes / +Nm / N（分钟）；Nh、N小时后按小时；
// N分钟前、N小时前为过去（等价于立刻到期）
bool parseRelativeDue(const std::string& in, std::time_t now, std::time_t& out) {
  std::string s = asciiLower(trimCopy(in));
  size_t i = 0;
  if (s.compare(0, 3, "in ") == 0) i = 3;
  if (i < s.size() && s[i] == '+') ++i;
  while (i < s.size() && s[i] == ' ') ++i;
  size_t ds = i;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
  if (i == ds || i - ds > 6) return false;               // 超长数字不是分钟数
  int64_t n = 0;
  for (size_t k = ds; k < i; ++k) n = n * 10 + (s[k] - '0');
  size_t us = i;
  while (us < s.size() && s[us] == ' ') ++us;
  std::string unit = s.substr(us);
  int64_t sec = -1;
  if (unit.empty() || unit == "m" || unit == "min" || unit == "mins" ||
      unit == "minute" || unit == "minutes" || unit == "分钟后" ||
      unit == "分钟" || unit == "分")
    sec = n * 60;
  else if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" ||
           unit == "hours" || unit == "小时后" || unit == "小时" || unit == "钟头")
    sec = n * 3600;
  else if (unit == "分钟前")
    sec = -n * 60;
  else if (unit == "小时前")
    sec = -n * 3600;
  else
    return false;
  int64_t epoch = static_cast<int64_t>(now) + sec;
  if (!inEpochRange(epoch)) return false;
  out = static_cast<std::time_t>(epoch);
  return true;
}

}  // namespace

// v5 DNS-Rebinding 防护：hostname 已通过 isBlockedHost 后，仍要解析出真实 IP 并逐个
// 复检——拦截「先解析到公网、被攻击者切换回内网」的 rebinding。逐地址转文本后交给
// isBlockedHost 复用同一套 IP 判定。解析无结果时按「放行」处理（连接阶段自然会失败），
// 但绝不放进内网地址。
bool resolveAndRecheck(const std::string& host) {
  WSADATA wsa{};
  (void)WSAStartup(MAKEWORD(2, 2), &wsa);
  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
    WSACleanup();
    return true;  // 解析失败 → 连接阶段自然失败，不放行内网段
  }
  bool safe = true;
  for (auto* ai = res; ai; ai = ai->ai_next) {
    std::string ip;
    char buf[INET6_ADDRSTRLEN] = {};
    if (ai->ai_family == AF_INET) {
      auto* v4 = reinterpret_cast<sockaddr_in*>(ai->ai_addr);
      inet_ntop(AF_INET, &v4->sin_addr, buf, sizeof(buf));
      ip = buf;
    } else if (ai->ai_family == AF_INET6) {
      auto* v6 = reinterpret_cast<sockaddr_in6*>(ai->ai_addr);
      inet_ntop(AF_INET6, &v6->sin6_addr, buf, sizeof(buf));
      ip = buf;
    } else {
      continue;
    }
    if (isBlockedHost(ip)) {
      safe = false;
      break;
    }
  }
  freeaddrinfo(res);
  WSACleanup();
  return safe;
}

// file_read 白名单：允许 OS 临时目录与 data 目录（及其子目录），另支持设置表显式追加根目录。
// 返回允许/拒绝。拒绝时输出 error 文本供工具回填。
bool fileReadAllowed(const std::string& rawPath) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path p = fs::u8path(rawPath);   // 输入按 UTF-8 构造，避免窄字节→ANSI 转换抛异常
  fs::path abs;
  if (p.is_absolute()) abs = p.lexically_normal();
  else abs = (fs::current_path(ec) / p).lexically_normal();
  if (ec) abs = p.lexically_normal();

  auto under = [](const fs::path& target, const fs::path& root) {
    std::error_code e2;
    auto rel = fs::relative(target, root, e2);
    if (e2) return false;
    return rel.empty() || (!rel.is_absolute() && rel.u32string().rfind(U"..", 0) != 0);
  };

  std::string data = db::dataDir();
  if (!data.empty() && under(abs, fs::path(data))) return true;
  if (under(abs, fs::temp_directory_path(ec)) && !ec) return true;

  // 允许通过设置表「file_read_allowlist」追加根目录（分号分隔，绝对路径）
  auto extra = db::getSetting("file_read_allowlist");
  if (extra && !extra->empty()) {
    std::string s = *extra;
    size_t pos = 0;
    while (pos <= s.size()) {
      size_t nxt = s.find(';', pos);
      std::string seg = s.substr(pos, nxt == std::string::npos ? std::string::npos : nxt - pos);
      if (!seg.empty() && under(abs, fs::path(seg))) return true;
      if (nxt == std::string::npos) break;
      pos = nxt + 1;
    }
  }
  return false;
}

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

// due_at 归一化：把绝对 ISO 8601 / 本地墙钟 / 相对分钟等写法统一成与 nowIso()
// 逐字符兼容的 UTC 字符串（"%Y-%m-%dT%H:%M:%SZ"），解析失败返回空串。
std::string normalizeDueAt(const std::string& in) {
  std::string s = trimCopy(in);
  if (s.empty()) return {};
  std::time_t t = 0;
  if (parseAbsoluteDue(s, t)) return isoFromEpoch(t);
  if (parseRelativeDue(s, std::time(nullptr), t)) return isoFromEpoch(t);
  return {};
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
      {"file_read", "读取文件", "读取本地文本文件（≤8KB，仅限 data 目录/系统临时目录/白名单）",
       json{{"type", "object"},
            {"properties", json{{"path", json{{"type", "string"}}}}},
            {"required", json::array({"path"})}}},
      {"reminder", "定时提醒", "设置一条定时提醒",
       json{{"type", "object"},
            {"properties",
             json{{"content", json{{"type", "string"}}},
                  {"due_at",
                   json{{"type", "string"},
                        {"description",
                         "支持 ISO 8601 或相对分钟（例如 15分钟后 / 30）"}}}}},
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
