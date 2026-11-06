#include "openai_provider.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "json_util.h"
#include "logger.h"

namespace llm {
namespace {

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

struct Url {
  bool https = true;
  std::string host;
  int port = 443;
  std::string path;   // 不含 /chat/completions 的部分，以 / 开头
};

// 解析 base_url（如 https://api.deepseek.com/v1），无 scheme 时默认 https
Url parseUrl(const std::string& base) {
  Url u;
  std::string rest = trim(base);
  if (rest.rfind("http://", 0) == 0) {
    u.https = false;
    u.port = 80;
    rest = rest.substr(7);
  } else if (rest.rfind("https://", 0) == 0) {
    rest = rest.substr(8);
  }
  auto slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  u.path = slash == std::string::npos ? "" : rest.substr(slash);
  while (!u.path.empty() && u.path.back() == '/') u.path.pop_back();
  auto colon = authority.find(':');
  if (colon != std::string::npos) {
    u.host = authority.substr(0, colon);
    try {
      u.port = std::stoi(authority.substr(colon + 1));
    } catch (...) {
      u.port = u.https ? 443 : 80;
    }
  } else {
    u.host = authority;
  }
  return u;
}

// 自定义代理优先；host 为空时读系统代理注册表（HKCU Internet Settings）
std::string resolveProxy(const ChatRequest& req) {
  if (!req.proxy_enabled) return "";
  if (!req.proxy_host.empty())
    return req.proxy_host + ":" + std::to_string(req.proxy_port);
  std::string v;
  HKEY hk;
  if (RegOpenKeyExA(HKEY_CURRENT_USER,
                    "Software\\Microsoft\\Windows\\CurrentVersion\\Internet "
                    "Settings",
                    0, KEY_READ, &hk) == ERROR_SUCCESS) {
    char buf[512];
    DWORD len = sizeof(buf), type = 0;
    if (RegQueryValueExA(hk, "ProxyServer", nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf), &len) == ERROR_SUCCESS &&
        type == REG_SZ)
      v = buf;
    RegCloseKey(hk);
  }
  if (v.empty()) return "";
  auto pick = [&](const std::string& proto) -> std::string {
    auto p = v.find(proto + "=");
    if (p == std::string::npos) return "";
    auto end = v.find(';', p);
    return v.substr(p + proto.size() + 1,
                    end == std::string::npos ? std::string::npos
                                             : end - p - proto.size() - 1);
  };
  std::string r = pick("https");
  if (r.empty()) r = pick("http");
  if (r.empty()) r = v;
  return r;
}

std::string buildBody(const ChatRequest& req) {
  json body = {{"model", req.model}, {"stream", true}, {"messages", json::array()}};
  for (auto& m : req.messages) {
    json j = {{"role", m.role}, {"content", m.content}};
    if (!m.extra.empty())
      for (auto& [k, v] : m.extra.items()) j[k] = v;
    body["messages"].push_back(j);
  }
  if (req.temperature > 0) body["temperature"] = req.temperature;
  if (req.max_tokens > 0) body["max_tokens"] = req.max_tokens;
  if (!req.tools.empty()) body["tools"] = req.tools;
  return body.dump();
}

void mockStream(StreamCallback cb, const std::atomic_bool& stop,
                const ChatRequest& req) {
  bool hasTool = false;
  for (auto& m : req.messages)
    if (m.role == "tool") hasTool = true;
  if (!req.tools.empty() && !hasTool) {
    json tc = json::array(
        {{{"id", "call_mock_1"},
          {"type", "function"},
          {"function", {{"name", "calculator"}, {"arguments", "{\"expression\":\"1+1\"}"}}}}});
    cb({"tool_calls", "", 0, "", tc.dump()});
    return;
  }
  std::vector<std::string> pieces =
      hasTool
          ? std::vector<std::string>{"计算结果：2"}
          : std::vector<std::string>{
                "你好呀！", "我是内置 Mock 回复，", "当前 LLM_MOCK=1，", "用于前端联调。",
                "\n\n支持 **Markdown** 渲染哦：\n\n```js\nconsole.log('hello');\n```"};
  for (auto& p : pieces) {
    if (stop) return;
    cb({"delta", p, 0, ""});
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  }
}

}  // namespace

void ToolCallAccumulator::feed(const json& delta) {
  if (!delta.contains("tool_calls") || !delta["tool_calls"].is_array()) return;
  for (auto& tc : delta["tool_calls"]) {
    int idx = tc.value("index", 0);
    while (static_cast<int>(out_.size()) <= idx) out_.push_back(json::object());
    auto& slot = out_[idx];
    if (tc.contains("id") && tc["id"].is_string()) slot["id"] = tc["id"];
    if (tc.value("type", "") == "function") slot["type"] = "function";
    if (tc.contains("function") && tc["function"].is_object()) {
      auto& fn = tc["function"];
      if (fn.contains("name") && fn["name"].is_string() && !fn["name"].get<std::string>().empty())
        slot["function"]["name"] = fn["name"];
      if (fn.contains("arguments") && fn["arguments"].is_string()) {
        std::string prev = slot["function"].value("arguments", "");
        slot["function"]["arguments"] = prev + fn["arguments"].get<std::string>();
      }
    }
  }
}

json ToolCallAccumulator::finish() { return out_; }

void SseParser::feed(const std::string& chunk) {
  buf_ += chunk;
  size_t pos;
  while ((pos = buf_.find('\n')) != std::string::npos) {
    std::string line = buf_.substr(0, pos);
    buf_.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) {                       // 事件边界
      if (hasData_ && onEvent) onEvent(eventName_, data_);
      eventName_ = "message";
      data_.clear();
      hasData_ = false;
      continue;
    }
    if (line[0] == ':') continue;             // 注释行
    if (line.rfind("event:", 0) == 0) {
      eventName_ = trim(line.substr(6));
    } else if (line.rfind("data:", 0) == 0) {
      if (hasData_) data_ += "\n";
      data_ += trim(line.substr(5));
      hasData_ = true;
    }
    // id:/retry: 等其他字段忽略
  }
}

void SseParser::finish() {
  if (hasData_ && onEvent) onEvent(eventName_, data_);
  hasData_ = false;
}

void OpenAiCompatProvider::streamChat(const ChatRequest& req, StreamCallback cb,
                                      const std::atomic_bool& stop) {
  if (std::getenv("LLM_MOCK")) {
    mockStream(cb, stop, req);
    return;
  }
  if (req.base_url.empty() || req.api_key.empty() || req.model.empty()) {
    cb({"error", "", 400, "请先在设置中填写 API Base URL、API Key 与模型名称"});
    return;
  }

  Url url = parseUrl(req.base_url);
  std::string fullPath = url.path + "/chat/completions";
  std::string auth = "Authorization: Bearer " + req.api_key + "\r\n";

  HINTERNET hSession = WinHttpOpen(L"AICompanion/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    cb({"error", "", 500, "WinHttpOpen 失败"});
    return;
  }
  WinHttpSetTimeouts(hSession, 10000, 10000, 30000, 60000);

  HINTERNET hConn = WinHttpConnect(hSession, widen(url.host).c_str(), url.port, 0);
  HINTERNET hReq = hConn ? WinHttpOpenRequest(hConn, L"POST", widen(fullPath).c_str(),
                                              nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              url.https ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
  if (!hConn || !hReq) {
    cb({"error", "", 500, "无法建立连接（WinHttpConnect/OpenRequest 失败）"});
    if (hReq) WinHttpCloseHandle(hReq);
    if (hConn) WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);
    return;
  }

  std::string proxy = resolveProxy(req);
  if (!proxy.empty()) {
    std::wstring wProxy = widen(proxy);
    wchar_t bypass[] = L"<local>";
    WINHTTP_PROXY_INFO pi;
    pi.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
    pi.lpszProxy = const_cast<wchar_t*>(wProxy.c_str());
    pi.lpszProxyBypass = bypass;
    WinHttpSetOption(hReq, WINHTTP_OPTION_PROXY, &pi, sizeof(pi));
  }

  std::wstring hdrs =
      L"Content-Type: application/json\r\n" + widen(auth) + L"Accept: text/event-stream\r\n";
  std::string body = buildBody(req);
  BOOL sent = WinHttpSendRequest(hReq, hdrs.c_str(), static_cast<DWORD>(hdrs.size()),
                                 const_cast<char*>(body.data()),
                                 static_cast<DWORD>(body.size()),
                                 static_cast<DWORD>(body.size()), 0);
  if (!sent) {
    DWORD err = GetLastError();
    cb({"error", "", 500, "无法连接 API 服务器（错误码 " + std::to_string(err) + "）"});
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);
    return;
  }
  if (!WinHttpReceiveResponse(hReq, nullptr)) {
    cb({"error", "", 500, "等待上游响应失败"});
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);
    return;
  }

  DWORD status = 0, sz = sizeof(status);
  WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                     WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                     WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    std::string errBody;
    char buf[2048];
    DWORD read = 0;
    while (WinHttpQueryDataAvailable(hReq, &sz) && sz > 0 &&
           WinHttpReadData(hReq, buf, (std::min)(sz, (DWORD)sizeof(buf)), &read) &&
           read > 0)
      errBody.append(buf, read);
    std::string msg;
    int code = 500;
    if (status == 401 || status == 403) {
      code = 401;
      msg = "API Key 无效或没有权限";
    } else if (status == 404) {
      code = 404;
      msg = "接口不存在，请检查 Base URL 是否包含 /v1";
    } else if (status == 429) {
      code = 429;
      msg = "请求过于频繁，请稍后再试";
    } else {
      msg = "上游服务错误（HTTP " + std::to_string(status) + "）";
    }
    if (!errBody.empty()) {
      try {
        json j = json::parse(errBody);
        if (j.contains("error") && j["error"].is_object() &&
            j["error"].contains("message"))
          msg = j["error"]["message"].get<std::string>();
      } catch (...) {
      }
    }
    cb({"error", "", code, msg});
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);
    return;
  }

  SseParser parser;
  ToolCallAccumulator acc;
  parser.onEvent = [&](const std::string& /*ev*/, const std::string& data) {
    if (data == "[DONE]") return;
    try {
      json j = json::parse(data);
      if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty())
        return;
      auto& d = j["choices"][0]["delta"];
      if (!d.is_object()) return;
      bool hadTool = d.contains("tool_calls") && d["tool_calls"].is_array();
      if (hadTool) acc.feed(d);
      if (d.contains("content") && d["content"].is_string()) {
        std::string t = d["content"].get<std::string>();
        if (!t.empty()) cb({"delta", t, 0, ""});
      }
    } catch (...) {
      // 忽略无法解析的事件（如 usage 统计、非标准扩展）
    }
  };

  char buf[8192];
  DWORD read = 0;
  for (;;) {
    if (stop) {
      logx::info("upstream stream aborted by stop flag");
      break;
    }
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
    DWORD want = (std::min)(avail, (DWORD)sizeof(buf));
    if (!WinHttpReadData(hReq, buf, want, &read) || read == 0) break;
    parser.feed(std::string(buf, read));
  }
  parser.finish();
  json tcs = acc.finish();
  if (!tcs.empty()) cb({"tool_calls", "", 0, "", tcs.dump()});

  WinHttpCloseHandle(hReq);
  WinHttpCloseHandle(hConn);
  WinHttpCloseHandle(hSession);
  logx::info("upstream stream ended");
}

std::string buildBodyForTest(const ChatRequest& req) { return buildBody(req); }

}  // namespace llm
