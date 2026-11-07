#pragma once
#include <string>
#include <vector>

namespace cfg {
struct Plugin {
  std::string name;
  std::string url;
  std::vector<std::string> events;
  // v0.5.3：插件可以单独停用而不删除，便于临时排查（省得删了再重建）。
  bool enabled = true;
  // v0.5.3：生效的 Bot。空 = 全部 Bot 都生效（兼容 v0.5.2 的旧配置与「全局插件」语义）。
  std::vector<int64_t> bot_ids;
};
struct ServerConfig {
  std::string host = "127.0.0.1";
  int port = 8765;
  int actual_port = 0;    // 端口回退后记录实际端口，0 表示未回退
  int keep_rounds = 20;
  std::string theme = "dark";
  double temperature = 0.8;
  std::string static_dir; // 空 = 不提供静态文件（WPF 用虚拟主机，无需此项）
  std::string auth_token; // 每启动一次生成的本地鉴权令牌；空 = 关闭鉴权（开发模式）
  std::vector<Plugin> plugins;
};
ServerConfig load(const std::string& path);
void save(const std::string& path, const ServerConfig& c);
}  // namespace cfg
