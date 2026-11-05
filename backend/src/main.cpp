#include <httplib.h>
#include <string>

#include "json_util.h"
#include "logger.h"

int main(int argc, char** argv) {
  std::string dataDir = "data";
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) == "--data") dataDir = argv[i + 1];
  }
  logx::init(dataDir + "/logs");
  logx::info("backend starting, data dir: " + dataDir);

  httplib::Server svr;
  svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"status\":\"ok\",\"version\":\"1.0.0\"}", "application/json");
  });

  for (int port = 8765; port <= 8775; ++port) {
    logx::info("trying to listen on 127.0.0.1:" + std::to_string(port));
    if (svr.listen("127.0.0.1", port)) {
      logx::info("listening on port " + std::to_string(port));
      return 0;
    }
  }
  logx::error("no available port");
  return 1;
}
