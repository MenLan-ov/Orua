#include <httplib.h>
#include <string>

#include "config.h"
#include "db.h"
#include "logger.h"
#include "server.h"

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

  if (!db::open(dataDir + "/app.db")) {
    logx::error("db open failed, exit");
    return 1;
  }

  httplib::Server svr;
  srv::registerRoutes(svr, sc.static_dir);

  for (int port = sc.port; port <= sc.port + 10; ++port) {
    logx::info("trying to listen on " + sc.host + ":" + std::to_string(port));
    if (svr.listen(sc.host, port)) {
      sc.actual_port = port;
      cfg::save(cfgPath, sc);
      logx::info("listening on port " + std::to_string(port));
      db::close();
      return 0;
    }
  }
  logx::error("no available port");
  return 1;
}
