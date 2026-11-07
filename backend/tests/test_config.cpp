#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>

#include "config.h"

TEST_CASE("config: 缺失文件返回默认值") {
  auto path = (std::filesystem::temp_directory_path() / "ac_no_such_config.json").string();
  std::filesystem::remove(path);
  auto c = cfg::load(path);
  CHECK(c.host == "127.0.0.1");
  CHECK(c.port == 8765);
  CHECK(c.actual_port == 0);
  CHECK(c.keep_rounds == 20);
  CHECK(c.theme == "dark");
  CHECK(c.plugins.empty());
}

TEST_CASE("config: 保存后读取往返一致") {
  auto path = (std::filesystem::temp_directory_path() / "ac_test_config.json").string();
  cfg::ServerConfig c;
  c.port = 9000;
  c.actual_port = 9001;
  c.keep_rounds = 5;
  c.theme = "sakura";
  c.auth_token = "tok_abc123-_";
  cfg::Plugin p;
  p.name = "demo";
  p.url = "http://127.0.0.1:9000/hook";
  p.events = {"chat.completed"};
  c.plugins.push_back(p);
  cfg::save(path, c);

  auto got = cfg::load(path);
  CHECK(got.port == 9000);
  CHECK(got.actual_port == 9001);
  CHECK(got.keep_rounds == 5);
  CHECK(got.theme == "sakura");
  CHECK(got.auth_token == "tok_abc123-_");
  REQUIRE(got.plugins.size() == 1);
  CHECK(got.plugins[0].name == "demo");
  CHECK(got.plugins[0].url == "http://127.0.0.1:9000/hook");
  REQUIRE(got.plugins[0].events.size() == 1);
  CHECK(got.plugins[0].events[0] == "chat.completed");
  std::filesystem::remove(path);
}

TEST_CASE("config: 损坏 JSON 回退默认值") {
  auto path = (std::filesystem::temp_directory_path() / "ac_bad_config.json").string();
  std::ofstream f(path);
  f << "{not valid json";
  f.close();
  auto c = cfg::load(path);
  CHECK(c.port == 8765);
  std::filesystem::remove(path);
}
