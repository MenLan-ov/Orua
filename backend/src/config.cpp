#include "config.h"

#include <filesystem>
#include <fstream>

#include "json_util.h"
#include "logger.h"

namespace cfg {

ServerConfig load(const std::string& path) {
  ServerConfig c;
  std::ifstream f(path);
  if (!f.is_open()) {
    logx::warn("config not found, using defaults: " + path);
    return c;
  }
  try {
    json j = json::parse(f);
    if (j.contains("server")) {
      auto& s = j["server"];
      c.host = s.value("host", c.host);
      c.port = s.value("port", c.port);
      c.actual_port = s.value("actual_port", c.actual_port);
      c.static_dir = s.value("static_dir", c.static_dir);
    }
    if (j.contains("defaults")) {
      auto& d = j["defaults"];
      c.keep_rounds = d.value("keep_rounds", c.keep_rounds);
      c.theme = d.value("theme", c.theme);
      c.temperature = d.value("temperature", c.temperature);
    }
    if (j.contains("plugins") && j["plugins"].is_array()) {
      for (auto& p : j["plugins"]) {
        Plugin pl;
        pl.name = p.value("name", "");
        pl.url = p.value("url", "");
        if (p.contains("events") && p["events"].is_array())
          for (auto& e : p["events"]) pl.events.push_back(e.get<std::string>());
        c.plugins.push_back(std::move(pl));
      }
    }
  } catch (const std::exception& e) {
    logx::warn(std::string("config parse failed, using defaults: ") + e.what());
    return ServerConfig{};
  }
  return c;
}

void save(const std::string& path, const ServerConfig& c) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  json j = {
      {"server",
       {{"host", c.host},
        {"port", c.port},
        {"actual_port", c.actual_port},
        {"static_dir", c.static_dir}}},
      {"defaults",
       {{"keep_rounds", c.keep_rounds},
        {"theme", c.theme},
        {"temperature", c.temperature}}},
      {"plugins", json::array()}};
  for (auto& p : c.plugins)
    j["plugins"].push_back({{"name", p.name}, {"url", p.url}, {"events", p.events}});
  std::ofstream f(path);
  f << j.dump(2);
}

}  // namespace cfg
