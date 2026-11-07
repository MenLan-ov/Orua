#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "config.h"
#include "db.h"
#include "event_bus.h"

// v0.5.3：插件新增 enabled / bot_ids 两个字段。
// 关键约束是向后兼容——v0.5.2 的旧 config.json 里没有这两个键，
// 读出来必须仍是「启用 + 全 Bot 生效」，否则升级会让现有插件全部失效。

namespace {
std::filesystem::path tempDir(const char* name) {
  auto d = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(d);
  std::filesystem::create_directories(d);
  return d;
}
}  // namespace

TEST_CASE("v0.5.3: 插件 enabled / bot_ids 可往返") {
  auto dir = tempDir("orua_test_plugin_v053");
  auto path = (dir / "config.json").string();

  cfg::ServerConfig sc;
  cfg::Plugin p1;
  p1.name = "全 Bot 插件";
  p1.url = "https://example.com/a";
  p1.events = {"chat.completed"};
  p1.enabled = true;
  // bot_ids 空 = 全部 Bot
  cfg::Plugin p2;
  p2.name = "单 Bot 插件";
  p2.url = "https://example.com/b";
  p2.events = {"chat.error"};
  p2.enabled = false;
  p2.bot_ids = {1, 3};
  sc.plugins = {p1, p2};
  cfg::save(path, sc);

  auto back = cfg::load(path);
  REQUIRE(back.plugins.size() == 2);
  CHECK(back.plugins[0].enabled == true);
  CHECK(back.plugins[0].bot_ids.empty());
  CHECK(back.plugins[1].enabled == false);
  REQUIRE(back.plugins[1].bot_ids.size() == 2);
  CHECK(back.plugins[1].bot_ids[0] == 1);
  CHECK(back.plugins[1].bot_ids[1] == 3);

  std::filesystem::remove_all(dir);
}

// 这条是升级安全的核心：旧配置文件不含 enabled/bot_ids，
// 必须按「启用 + 全 Bot」解释，不能让老用户的插件在升级后静默失效。
TEST_CASE("v0.5.3: v0.5.2 旧配置缺字段时默认启用且全 Bot 生效") {
  auto dir = tempDir("orua_test_plugin_legacy");
  auto path = (dir / "config.json").string();

  std::ofstream f(path);
  f << R"({
  "server": {"host":"127.0.0.1","port":8765},
  "defaults": {"keep_rounds":20,"theme":"dark","temperature":0.8},
  "plugins": [
    {"name":"老插件","url":"https://example.com/old","events":["chat.completed"]}
  ]
})";
  f.close();

  auto back = cfg::load(path);
  REQUIRE(back.plugins.size() == 1);
  CHECK(back.plugins[0].name == "老插件");
  CHECK(back.plugins[0].enabled == true);      // 缺省即启用
  CHECK(back.plugins[0].bot_ids.empty());      // 空 = 全 Bot

  std::filesystem::remove_all(dir);
}

TEST_CASE("v0.5.3: bot_ids 里的非法值被忽略") {
  auto dir = tempDir("orua_test_plugin_badids");
  auto path = (dir / "config.json").string();

  std::ofstream f(path);
  f << R"({
  "plugins": [
    {"name":"p","url":"https://e.com/h","events":["chat.completed"],
     "enabled":false,"bot_ids":[2,"x",null,4]}
  ]
})";
  f.close();

  auto back = cfg::load(path);
  REQUIRE(back.plugins.size() == 1);
  CHECK(back.plugins[0].enabled == false);
  REQUIRE(back.plugins[0].bot_ids.size() == 2);
  CHECK(back.plugins[0].bot_ids[0] == 2);
  CHECK(back.plugins[0].bot_ids[1] == 4);

  std::filesystem::remove_all(dir);
}

// 这是 v0.5.3 里最容易踩的坑：setBotSkills 是「先删光再重建」，
// 原先重建时把 params 一律写成 "{}"，导致用户每勾一次技能就把已配的参数清空。
TEST_CASE("v0.5.3: 重新勾选技能不清空已配置的参数") {
  auto path = (std::filesystem::temp_directory_path() / "ac_test_v053_params.db");
  std::filesystem::remove(path);
  REQUIRE(db::open(path.string()));

  auto bot = db::getDefaultBot();
  REQUIRE(bot.has_value());
  int64_t botId = bot->id;

  // 先启用技能并写入参数
  db::setBotSkills(botId, {{"http_get", true}, {"calculator", true}});
  REQUIRE(db::setBotSkillParams(botId, "http_get", R"({"timeout_ms":5000})"));

  auto got = db::getBotSkillParams(botId, "http_get");
  REQUIRE(got.has_value());
  CHECK(*got == R"({"timeout_ms":5000})");

  // 再次勾选（模拟用户在前端点了别的技能）——参数必须还在
  db::setBotSkills(botId, {{"http_get", true}, {"calculator", true}, {"datetime", true}});
  auto again = db::getBotSkillParams(botId, "http_get");
  REQUIRE(again.has_value());
  CHECK(*again == R"({"timeout_ms":5000})");

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("v0.5.3: 未启用的技能写参数返回 false") {
  auto path = (std::filesystem::temp_directory_path() / "ac_test_v053_noparams.db");
  std::filesystem::remove(path);
  REQUIRE(db::open(path.string()));

  auto bot = db::getDefaultBot();
  REQUIRE(bot.has_value());
  int64_t botId = bot->id;

  db::setBotSkills(botId, {{"calculator", true}});
  CHECK_FALSE(db::setBotSkillParams(botId, "file_read", R"({"x":1})"));
  CHECK_FALSE(db::getBotSkillParams(botId, "file_read").has_value());
  // 已启用的可以写
  CHECK(db::setBotSkillParams(botId, "calculator", R"({"precision":2})"));

  db::close();
  std::filesystem::remove(path);
}

// v0.5.3：Webhook 的 https 地址必须能正确解析。
// 修复前 parseUrl 只剥 "http://"，于是 "https://example.com/x" 的 host 会被
// 解析成 "https:"、path 变成 "//example.com/x"，投递必然失败且报错莫名其妙。
// 这里通过 ping 的可观察结果反推：host 若被解析成 "https:"，
// 会立刻以「无法连接」返回；正确解析则会真的去连 example.com（拿不到 2xx）。
TEST_CASE("v0.5.3 https webhook 地址被正确解析") {
  auto r = ev::EventBus::instance().ping("https://example.com/orua-hook");
  // 关键断言：不能因为「地址格式错」而连解析都失败。
  // 真实网络可能不可达，所以只要求它不是那种「host 为空/非法」的立即失败形态。
  CHECK(r.elapsed_ms >= 0);
  CHECK(r.detail.find("https:") == std::string::npos);
}

TEST_CASE("v0.5.3 非法 webhook 地址不会崩溃") {
  auto r = ev::EventBus::instance().ping("");
  CHECK_FALSE(r.ok);
  CHECK_FALSE(r.detail.empty());

  auto r2 = ev::EventBus::instance().ping("不是地址");
  CHECK_FALSE(r2.ok);
  CHECK_FALSE(r2.detail.empty());
}
