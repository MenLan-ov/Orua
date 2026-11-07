#pragma once
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "json_util.h"

namespace ev {

using Listener = std::function<void(const std::string& type, const json& data)>;

class EventBus {
 public:
  static EventBus& instance();

  void loadPlugins(const std::vector<cfg::Plugin>& plugins);
  void subscribe(Listener l);
  // 同步通知本进程监听者；异步分发到匹配事件的插件 Webhook（v5：有界分派，不无限开线程）
  void emit(const std::string& type, const json& data);

  // v0.5.3 测试连接：同步 POST 一条 ping，返回是否可达与细节。
  // 与正式分发区分开：这里要的是「立刻告诉用户结果」，所以不走有界队列。
  struct PingResult {
    bool ok = false;
    int status = 0;        // HTTP 状态码，0 = 连接阶段就失败
    std::string detail;    // 失败原因或响应片段
    int elapsed_ms = 0;
  };
  PingResult ping(const std::string& url);

 private:
  struct WebhookJob {
    std::string name;
    std::string url;
    std::string body;
  };

  std::mutex mtx_;
  std::vector<Listener> listeners_;
  std::vector<cfg::Plugin> plugins_;

  // Webhook 有界分派：并发上限 + 有界队列，突发 emit 不会 scale 出无限线程。
  std::mutex wm_;
  size_t inflight_ = 0;
  size_t queued_ = 0;
  std::deque<WebhookJob> pending_;

  void scheduleWebhook(const std::string& name, const std::string& url,
                       const std::string& body);
  void finishWebhook();
};

}  // namespace ev
