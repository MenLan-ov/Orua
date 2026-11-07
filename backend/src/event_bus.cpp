#include "event_bus.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

// httplib 内部会 include winsock2.h。必须先让它进来，再把 windows.h/winhttp.h
// 放在后面，否则 winsock2 会与 windows.h 里的 winsock1 定义冲突（sockaddr 重定义）。
#include <httplib.h>

#include <windows.h>
#include <winhttp.h>

#include "logger.h"

namespace ev {
namespace {

constexpr size_t kMaxInflight = 8;    // 并发 Webhook 上限
constexpr size_t kMaxQueue = 256;     // 有界队列：满则丢弃最新

struct ParsedUrl {
  std::string host;
  int port = 80;
  std::string path = "/";
  bool tls = false;
};

ParsedUrl parseUrl(const std::string& u) {
  std::string s = u;
  ParsedUrl out;
  // https 也要剥掉，否则 "https:" 会被当成 host、路径整个错位。
  // 这里只做解析，真正的 TLS 由 httplib::SSLClient 承担。
  if (s.rfind("https://", 0) == 0) {
    s = s.substr(8);
    out.tls = true;
  } else if (s.rfind("http://", 0) == 0) {
    s = s.substr(7);
  }
  auto slash = s.find('/');
  std::string hostport = slash == std::string::npos ? s : s.substr(0, slash);
  out.path = slash == std::string::npos ? "/" : s.substr(slash);
  auto colon = hostport.find(':');
  out.host = colon == std::string::npos ? hostport : hostport.substr(0, colon);
  if (colon != std::string::npos) {
    try {
      out.port = std::stoi(hostport.substr(colon + 1));
    } catch (...) {
      out.port = out.tls ? 443 : 80;
    }
  } else {
    out.port = out.tls ? 443 : 80;
  }
  return out;
}

}  // namespace

namespace {

std::wstring widenAscii(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// 投递一次 Webhook。走 WinHTTP 而不是 httplib，原因有两个：
//   1) 本项目 httplib 编译时没开 OpenSSL，没有 SSLClient，https 地址根本无法投递；
//   2) WinHTTP 会自动使用系统代理设置，与 LLM 请求行为一致，用户开了代理也不会漏配。
// 抽成函数是因为成功路径和队列补发路径逻辑必须完全一致
// （曾经两处各写一遍，https 修正只改到一处就会漏）。
bool postWebhook(const std::string& url, const std::string& body, int* statusOut,
                 std::string* bodyOut = nullptr) {
  auto u = parseUrl(url);
  if (u.host.empty()) return false;
  HINTERNET hSess = WinHttpOpen(L"OruaPlugin/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSess) return false;
  // Webhook 不跟随重定向：目标地址是用户填的，跟随会让「测试连接」的结果
  // 与实际投递地址不一致，也不好解释。
  DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  WinHttpSetOption(hSess, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
  WinHttpSetTimeouts(hSess, 2000, 2000, 2000, 2000);

  bool ok = false;
  HINTERNET hConn = WinHttpConnect(hSess, widenAscii(u.host).c_str(), (INTERNET_PORT)u.port, 0);
  HINTERNET hReq = hConn ? WinHttpOpenRequest(hConn, L"POST", widenAscii(u.path).c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              u.tls ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
  if (hReq) {
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (WinHttpSendRequest(hReq, headers.c_str(), (DWORD)-1L, (LPVOID)body.data(),
                           (DWORD)body.size(), (DWORD)body.size(), 0) &&
        WinHttpReceiveResponse(hReq, nullptr)) {
      DWORD code = 0, codeSize = sizeof(code);
      if (!WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize,
                               WINHTTP_NO_HEADER_INDEX))
        code = 0;
      if (statusOut) *statusOut = (int)code;
      ok = code >= 200 && code < 300;
      // 只在需要时读响应体（「测试连接」要把服务端返回的错误片段显示给用户）
      if (bodyOut) {
        char buf[256];
        DWORD read = 0, avail = 0;
        while (bodyOut->size() < 200 && WinHttpQueryDataAvailable(hReq, &avail) && avail > 0 &&
               WinHttpReadData(hReq, buf, (std::min)(avail, (DWORD)sizeof(buf)), &read) &&
               read > 0)
          bodyOut->append(buf, read);
        if (bodyOut->size() > 200) bodyOut->resize(200);
      }
    }
  }
  if (hReq) WinHttpCloseHandle(hReq);
  if (hConn) WinHttpCloseHandle(hConn);
  WinHttpCloseHandle(hSess);
  return ok;
}

}  // namespace

EventBus& EventBus::instance() {
  static EventBus bus;
  return bus;
}

void EventBus::loadPlugins(const std::vector<cfg::Plugin>& plugins) {
  std::lock_guard<std::mutex> lk(mtx_);
  plugins_ = plugins;
  logx::info("plugins loaded: " + std::to_string(plugins_.size()));
}

void EventBus::subscribe(Listener l) {
  std::lock_guard<std::mutex> lk(mtx_);
  listeners_.push_back(std::move(l));
}

void EventBus::emit(const std::string& type, const json& data) {
  json payload = {{"type", type}, {"ts", nowIso()}, {"data", data}};
  // v0.5.3：插件可以限定生效 Bot。事件载荷里若能取到 bot_id 就按它过滤；
  // 取不到（如 session.deleted 只有 id）则视为「与 Bot 无关」，只发给全 Bot 插件。
  int64_t botId = 0;
  if (data.is_object() && data.contains("bot_id") && data["bot_id"].is_number_integer())
    botId = data["bot_id"].get<int64_t>();

  std::vector<Listener> ls;
  std::vector<cfg::Plugin> targets;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    ls = listeners_;
    for (auto& p : plugins_) {
      if (!p.enabled) continue;  // 停用的插件不分发
      if (std::find(p.events.begin(), p.events.end(), type) == p.events.end()) continue;
      // bot_ids 为空 = 全部 Bot 生效（兼容旧配置）
      if (!p.bot_ids.empty()) {
        if (botId == 0) continue;
        if (std::find(p.bot_ids.begin(), p.bot_ids.end(), botId) == p.bot_ids.end())
          continue;
      }
      targets.push_back(p);
    }
  }
  for (auto& l : ls) {
    try {
      l(type, data);
    } catch (...) {
    }
  }
  for (auto& p : targets) {
    scheduleWebhook(p.name, p.url, payload.dump());
  }
}

void EventBus::scheduleWebhook(const std::string& name, const std::string& url,
                               const std::string& body) {
  std::lock_guard<std::mutex> lk(wm_);
  if (inflight_ < kMaxInflight) {
    ++inflight_;
    std::thread([this, name, url, body]() {
      try {
        if (!postWebhook(url, body, nullptr))
          logx::warn("plugin webhook failed: " + name);
      } catch (const std::exception& e) {
        logx::warn(std::string("plugin webhook exception: ") + e.what());
      }
      finishWebhook();
    }).detach();
    return;
  }
  if (queued_ < kMaxQueue) {
    ++queued_;
    pending_.push_back(WebhookJob{name, url, body});
  } else {
    logx::warn("plugin webhook queue full, dropping: " + name);
  }
}

void EventBus::finishWebhook() {
  std::lock_guard<std::mutex> lk(wm_);
  if (inflight_ > 0) --inflight_;
  while (inflight_ < kMaxInflight && !pending_.empty()) {
    auto job = std::move(pending_.front());
    pending_.pop_front();
    --queued_;
    ++inflight_;
    std::thread([this, name = job.name, url = job.url, body = job.body]() {
      try {
        // 必须用捕获的副本：job 已 move 出队列，且本线程与调用方并发
        if (!postWebhook(url, body, nullptr))
          logx::warn("plugin webhook failed: " + name);
      } catch (const std::exception& e) {
        logx::warn(std::string("plugin webhook exception: ") + e.what());
      }
      finishWebhook();
    }).detach();
  }
}

// 同步 ping：给「测试连接」按钮用。2 秒超时与正式分发保持一致，
// 否则用户会以为连接正常、实际分发却超时。
EventBus::PingResult EventBus::ping(const std::string& url) {
  PingResult r;
  auto started = std::chrono::steady_clock::now();
  auto elapsed = [&]() {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count());
  };
  try {
    json body = {{"type", "plugin.test"},
                 {"ts", nowIso()},
                 {"data", {{"message", "Orua 插件连通性测试"}}}};
    std::string respBody;
    bool ok = postWebhook(url, body.dump(), &r.status, &respBody);
    r.elapsed_ms = elapsed();
    if (r.status == 0) {
      r.detail = "无法连接（地址不可达、DNS 失败或被拒绝）";
      return r;
    }
    // 2xx 视为成功；3xx/4xx/5xx 都算失败——Webhook 端返回非 2xx 说明它不接受这次投递。
    r.ok = ok;
    if (!r.ok) {
      r.detail = "服务端返回 HTTP " + std::to_string(r.status);
      if (!respBody.empty()) r.detail += "：" + respBody;
    } else {
      r.detail = "连接成功（HTTP " + std::to_string(r.status) + "）";
    }
    return r;
  } catch (const std::exception& e) {
    r.elapsed_ms = elapsed();
    r.detail = std::string("异常：") + e.what();
    return r;
  }
}

}  // namespace ev