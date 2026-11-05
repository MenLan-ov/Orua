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

TEST_CASE("db: 打开即建表并播种默认机器人") {
  auto path = tmpDb("ac_test_seed.db");
  REQUIRE(db::open(path.string()));
  auto bots = db::listBots();
  REQUIRE(bots.size() == 1);
  auto def = db::getDefaultBot();
  REQUIRE(def.has_value());
  CHECK(def->name == "默认助手");
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("db: 会话与消息 CRUD + 级联删除") {
  auto path = tmpDb("ac_test_crud.db");
  REQUIRE(db::open(path.string()));

  auto s = db::createSession();
  CHECK(s.title == "新对话");
  auto m1 = db::appendMessage(s.id, "user", "你好");
  auto m2 = db::appendMessage(s.id, "assistant", "你好呀");
  CHECK(m1.id < m2.id);

  auto msgs = db::listMessages(s.id);
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0].role == "user");       // 升序
  CHECK(msgs[1].role == "assistant");

  auto last = db::lastMessageByRole(s.id, "assistant");
  REQUIRE(last.has_value());
  CHECK(last->content == "你好呀");

  db::renameSession(s.id, "测试会话");
  auto got = db::getSession(s.id);
  REQUIRE(got.has_value());
  CHECK(got->title == "测试会话");

  db::deleteMessage(m2.id);
  CHECK(db::listMessages(s.id).size() == 1);

  db::deleteSession(s.id);
  CHECK(db::listMessages(s.id).empty());   // 级联
  CHECK(!db::getSession(s.id).has_value());

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("db: 人设 CRUD 与激活约束") {
  auto path = tmpDb("ac_test_persona.db");
  REQUIRE(db::open(path.string()));

  db::Persona p;
  p.name = "猫娘";
  p.greeting = "喵~";
  p.system_prompt = "你是猫娘";
  p.temperature = 0.9;
  auto created = db::createPersona(p);
  CHECK(created.id > 0);

  CHECK(db::activatePersona(created.id));
  auto active = db::getActivePersona();
  REQUIRE(active.has_value());
  CHECK(active->name == "猫娘");

  // 激活中的人设不允许删除
  CHECK(db::deletePersona(created.id) == false);

  created.greeting = "喵喵~";
  CHECK(db::updatePersona(created));
  CHECK(db::listPersonas().size() == 1);  // 本任务起不再播种默认人设
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("db: settings 读写") {
  auto path = tmpDb("ac_test_settings.db");
  REQUIRE(db::open(path.string()));
  db::setSetting("model", "deepseek-chat");
  auto v = db::getSetting("model");
  REQUIRE(v.has_value());
  CHECK(*v == "deepseek-chat");
  CHECK(db::getSetting("nonexistent") == std::nullopt);
  db::close();
  std::filesystem::remove(path);
}
