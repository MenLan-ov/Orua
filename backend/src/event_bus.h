#pragma once
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
  // 同步通知本进程监听者；异步 fire-and-forget 分发到匹配事件的插件 Webhook
  void emit(const std::string& type, const json& data);

 private:
  std::mutex mtx_;
  std::vector<Listener> listeners_;
  std::vector<cfg::Plugin> plugins_;
};

}  // namespace ev
