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

TEST_CASE("db: listMessages 返回最新 N 条（升序）") {
  auto path = tmpDb("ac_test_listlatest.db");
  REQUIRE(db::open(path.string()));

  auto s = db::createSession();
  for (int i = 1; i <= 5; ++i) {
    std::string role = (i % 2 == 1) ? "user" : "assistant";
    auto m = db::appendMessage(s.id, role, "内容" + std::to_string(i));
    CHECK(m.id == i);
  }

  // limit=3 → 最新 3 条（id 3,4,5），按 id 升序
  auto last3 = db::listMessages(s.id, 3);
  REQUIRE(last3.size() == 3);
  CHECK(last3[0].id == 3);
  CHECK(last3[0].role == "user");
  CHECK(last3[0].content == "内容3");
  CHECK(last3[1].id == 4);
  CHECK(last3[1].role == "assistant");
  CHECK(last3[1].content == "内容4");
  CHECK(last3[2].id == 5);
  CHECK(last3[2].role == "user");
  CHECK(last3[2].content == "内容5");

  // 默认 limit → 全部 5 条，升序
  auto all = db::listMessages(s.id);
  REQUIRE(all.size() == 5);
  for (int i = 0; i < 5; ++i) CHECK(all[i].id == i + 1);
  CHECK(all[4].content == "内容5");

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
