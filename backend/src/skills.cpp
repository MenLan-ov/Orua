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

// 拒绝 localhost 与内网段（SSRF 防护）
bool isBlockedHost(const std::string& host) {
  std::string h = host;
  if (h == "localhost") return true;
  if (h.rfind("127.", 0) == 0) return true;
  if (h.rfind("10.", 0) == 0) return true;
  if (h.rfind("192.168.", 0) == 0) return true;
  if (h.rfind("172.", 0) == 0) {
    auto dot = h.find('.', 4);
    if (dot != std::string::npos) {
      int part = std::atoi(h.substr(4, dot - 4).c_str());
      if (part >= 16 && part <= 31) return true;
    }
  }
  return false;
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

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
  std::string host = authority;
  int port = https ? 443 : 80;
  auto colon = authority.find(':');
  if (colon != std::string::npos) {
    host = authority.substr(0, colon);
    try { port = std::stoi(authority.substr(colon + 1)); } catch (...) {}
  }
  if (isBlockedHost(host)) return {{"error", "拒绝访问本地/内网地址"}};

  HINTERNET hSess = WinHttpOpen(L"OruaSkill/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSess) return {{"error", "WinHttpOpen 失败"}};
  WinHttpSetTimeouts(hSess, 3000, 3000, 10000, 10000);
  HINTERNET hConn = WinHttpConnect(hSess, widen(host).c_str(), port, 0);
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
    std::string body;
    char buf[2048];
    DWORD read = 0, avail = 0;
    while (body.size() < 2048 && WinHttpQueryDataAvailable(hReq, &avail) && avail > 0 &&
           WinHttpReadData(hReq, buf, (std::min)(avail, (DWORD)sizeof(buf)), &read) &&
           read > 0)
      body.append(buf, read);
    if (body.size() > 2048) body.resize(2048);
    out = {{"body", body}};
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
      {"http_get", "HTTP 请求", "GET 指定网页并返回正文摘要（≤2KB，拒绝内网地址）",
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
