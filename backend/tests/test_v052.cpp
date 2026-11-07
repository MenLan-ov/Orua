#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "config.h"

// v0.5.2：插件（Webhook）配置的读写往返。
// 插件增删接口最终都落到 cfg::save / cfg::load，这里固化这一层的语义，
// 避免「UI 上加了插件但重启后消失」这类回归。
TEST_CASE("v0.5.2: 插件配置写入后能原样读回") {
  auto dir = std::filesystem::temp_directory_path() / "orua_test_plugins";
  std::filesystem::create_directories(dir);
  auto path = (dir / "config.json").string();

  cfg::ServerConfig sc;
  sc.plugins = {
      {"通知机器人", "https://example.com/hook", {"chat.completed", "chat.error"}},
      {"日志收集", "http://127.0.0.1:9000/log", {"session.created"}},
  };
  cfg::save(path, sc);

  auto back = cfg::load(path);
  REQUIRE(back.plugins.size() == 2);
  CHECK(back.plugins[0].name == "通知机器人");
  CHECK(back.plugins[0].url == "https://example.com/hook");
  REQUIRE(back.plugins[0].events.size() == 2);
  CHECK(back.plugins[0].events[0] == "chat.completed");
  CHECK(back.plugins[0].events[1] == "chat.error");
  CHECK(back.plugins[1].name == "日志收集");
  CHECK(back.plugins[1].events.size() == 1);

  std::filesystem::remove_all(dir);
}

TEST_CASE("v0.5.2: 空插件列表可往返，且不产生垃圾条目") {
  auto dir = std::filesystem::temp_directory_path() / "orua_test_plugins_empty";
  std::filesystem::create_directories(dir);
  auto path = (dir / "config.json").string();

  cfg::ServerConfig sc;
  sc.plugins.clear();
  cfg::save(path, sc);

  auto back = cfg::load(path);
  CHECK(back.plugins.empty());

  std::filesystem::remove_all(dir);
}

TEST_CASE("v0.5.2: 配置文件不存在时返回默认空插件列表") {
  auto dir = std::filesystem::temp_directory_path() / "orua_test_plugins_missing";
  std::filesystem::create_directories(dir);
  auto path = (dir / "nope.json").string();

  auto back = cfg::load(path);
  CHECK(back.plugins.empty());

  std::filesystem::remove_all(dir);
}

// 保存时保留既有字段（host/port/token 等），不能因为加插件把服务配置写丢。
TEST_CASE("v0.5.2: 写插件不影响服务端配置字段") {
  auto dir = std::filesystem::temp_directory_path() / "orua_test_plugins_keep";
  std::filesystem::create_directories(dir);
  auto path = (dir / "config.json").string();

  cfg::ServerConfig sc;
  sc.host = "127.0.0.1";
  sc.port = 8765;
  sc.actual_port = 8767;
  sc.auth_token = "token-abc";
  sc.keep_rounds = 12;
  sc.plugins = {{"p1", "https://a.example/h", {"chat.completed"}}};
  cfg::save(path, sc);

  auto back = cfg::load(path);
  CHECK(back.host == "127.0.0.1");
  CHECK(back.port == 8765);
  CHECK(back.actual_port == 8767);
  CHECK(back.auth_token == "token-abc");
  CHECK(back.keep_rounds == 12);
  REQUIRE(back.plugins.size() == 1);

  std::filesystem::remove_all(dir);
}
