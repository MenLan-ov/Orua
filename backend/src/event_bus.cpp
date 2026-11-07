#include "event_bus.h"

#include <algorithm>
#include <thread>

#include <httplib.h>

#include "logger.h"

namespace ev {
namespace {

constexpr size_t kMaxInflight = 8;    // 并发 Webhook 上限
constexpr size_t kMaxQueue = 256;     // 有界队列：满则丢弃最新

struct ParsedUrl {
  std::string host;
  int port = 80;
  std::string path = "/";
};

ParsedUrl parseUrl(const std::string& u) {
  std::string s = u;
  if (s.rfind("http://", 0) == 0) s = s.substr(7);
  auto slash = s.find('/');
  std::string hostport = slash == std::string::npos ? s : s.substr(0, slash);
  ParsedUrl out;
  out.path = slash == std::string::npos ? "/" : s.substr(slash);
  auto colon = hostport.find(':');
  out.host = colon == std::string::npos ? hostport : hostport.substr(0, colon);
  if (colon != std::string::npos) {
    try {
      out.port = std::stoi(hostport.substr(colon + 1));
    } catch (...) {
      out.port = 80;
    }
  }
  return out;
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
  std::vector<Listener> ls;
  std::vector<cfg::Plugin> targets;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    ls = listeners_;
    for (auto& p : plugins_)
      if (std::find(p.events.begin(), p.events.end(), type) != p.events.end())
        targets.push_back(p);
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
        auto u = parseUrl(url);
        httplib::Client cli(u.host, u.port);
        cli.set_connection_timeout(2);
        cli.set_read_timeout(2);
        cli.set_write_timeout(2);
        auto res = cli.Post(u.path.c_str(), body, "application/json");
        if (!res || res->status != 200)
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
        auto u = parseUrl(url);
        httplib::Client cli(u.host, u.port);
        cli.set_connection_timeout(2);
        cli.set_read_timeout(2);
        cli.set_write_timeout(2);
        auto res = cli.Post(u.path.c_str(), body, "application/json");
        if (!res || res->status != 200)
          logx::warn("plugin webhook failed: " + name);
      } catch (const std::exception& e) {
        logx::warn(std::string("plugin webhook exception: ") + e.what());
      }
      finishWebhook();
    }).detach();
  }
}

}  // namespace ev