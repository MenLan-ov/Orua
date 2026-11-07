#include <httplib.h>
#include <string>

#include "config.h"
#include "db.h"
#include "event_bus.h"
#include "logger.h"
#include "server.h"
#include "validation.h"

int main(int argc, char** argv) {
  std::string dataDir = "data";
  std::string staticDir;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    if (k == "--data") dataDir = argv[i + 1];
    else if (k == "--static") staticDir = argv[i + 1];
  }
  logx::init(dataDir + "/logs");
  logx::info("backend starting, data dir: " + dataDir);

  auto cfgPath = dataDir + "/config.json";
  auto sc = cfg::load(cfgPath);
  if (!staticDir.empty()) sc.static_dir = staticDir;
  // v5：每次启动生成新令牌并在 config.json 落盘，前端经 WPF 注入后随请求回传，
  // 关闭依赖 DNS 复位的密钥窃取面（Rebinding）；空值仅在开发/e2e 直连时允许关闭鉴权。
  sc.auth_token = val::randomToken();

  if (!db::open(dataDir + "/app.db")) {
    logx::error("db open failed, exit");
    return 1;
  }

  ev::EventBus::instance().loadPlugins(sc.plugins);

  // 端口回退：每个端口用全新 Server 尝试（bind 失败会置 is_decommisioned，复用一个实例会污染后续尝试）
  for (int port = sc.port; port <= sc.port + 10; ++port) {
    logx::info("trying to listen on " + sc.host + ":" + std::to_string(port));
    httplib::Server svr;
    srv::registerRoutes(svr, sc.static_dir, sc.auth_token, cfgPath);
    if (svr.bind_to_port(sc.host, port)) {
      sc.actual_port = port;
      cfg::save(cfgPath, sc);
      logx::info("listening on port " + std::to_string(port));
      svr.listen_after_bind();
      db::close();
      return 0;
    }
  }
  logx::error("no available port");
  return 1;
}
