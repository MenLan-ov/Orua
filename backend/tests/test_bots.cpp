#include <doctest/doctest.h>
#include <filesystem>

#include "db.h"

namespace {
std::filesystem::path tmpDb(const char* name) {
  auto p = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove(p);
  return p;
}
}  // namespace

TEST_CASE("bots: 默认机器人播种与删除保护") {
  auto path = tmpDb("ac_v2_bots_seed.db");
  REQUIRE(db::open(path.string()));
  auto bots = db::listBots();
  REQUIRE(bots.size() == 1);
  auto def = db::getDefaultBot();
  REQUIRE(def.has_value());
  CHECK(def->name == "默认助手");
  CHECK(def->id == bots[0].id);
  CHECK(db::deleteBot(def->id) == false);  // 默认机器人不可删
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("bots: CRUD 与 API 覆盖字段") {
  auto path = tmpDb("ac_v2_bots_crud.db");
  REQUIRE(db::open(path.string()));

  db::Bot b;
  b.name = "猫娘";
  b.system_prompt = "你是猫娘";
  b.avatar = "🐱";
  b.temperature = 0.9;
  b.api_base_url = "https://cat.example.com/v1";
  b.api_model = "cat-7b";
  auto created = db::createBot(b);
  CHECK(created.id > 0);
  CHECK(created.is_enabled);

  auto got = db::getBot(created.id);
  REQUIRE(got.has_value());
  CHECK(got->api_base_url == "https://cat.example.com/v1");
  CHECK(got->api_model == "cat-7b");
  CHECK(got->api_key_enc.empty());

  created.greeting = "喵~";
  created.api_key_enc = "ENC123";
  CHECK(db::updateBot(created));
  got = db::getBot(created.id);
  CHECK(got->greeting == "喵~");
  CHECK(got->api_key_enc == "ENC123");

  CHECK(db::setBotEnabled(created.id, false));
  CHECK(db::getBot(created.id)->is_enabled == false);
  CHECK(db::setBotEnabled(created.id, true));

  CHECK(db::deleteBot(created.id));
  CHECK(!db::getBot(created.id).has_value());
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("bots: 会话绑定与切换机器人") {
  auto path = tmpDb("ac_v2_sessions_bot.db");
  REQUIRE(db::open(path.string()));
  auto def = db::getDefaultBot();
  REQUIRE(def.has_value());
  auto s = db::createSession();
  CHECK(s.bot_id == 0);
  db::setSessionBot(s.id, def->id);
  auto got = db::getSession(s.id);
  REQUIRE(got.has_value());
  CHECK(got->bot_id == def->id);
  db::close();
  std::filesystem::remove(path);
}
