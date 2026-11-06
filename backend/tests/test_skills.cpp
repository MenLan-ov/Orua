#include <doctest/doctest.h>
#include <filesystem>
#include <thread>

#include "db.h"

namespace {
std::filesystem::path tmpDb(const char* name) {
  auto p = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove(p);
  return p;
}
}  // namespace

TEST_CASE("skills: bot_skills 全量覆盖") {
  auto path = tmpDb("ac_v4_skills.db");
  REQUIRE(db::open(path.string()));
  auto b = db::createBot(db::Bot{0, "测试", "🤖", "", "人设"});

  db::setBotSkills(b.id, {{"calculator", true}, {"reminder", false}, {"sysinfo", true}});
  auto list = db::listBotSkills(b.id);
  REQUIRE(list.size() == 3);
  CHECK(list[0].skill_id == "calculator");
  CHECK(list[0].enabled);
  CHECK(list[1].skill_id == "reminder");
  CHECK(!list[1].enabled);

  // 全量覆盖：替换
  db::setBotSkills(b.id, {{"datetime", true}});
  list = db::listBotSkills(b.id);
  REQUIRE(list.size() == 1);
  CHECK(list[0].skill_id == "datetime");
  CHECK(list[0].enabled);

  // 清空
  db::setBotSkills(b.id, {});
  CHECK(db::listBotSkills(b.id).empty());

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("skills: setBotSkills 事务回滚——重复 skill_id 抛异常且不半覆盖") {
  auto path = tmpDb("ac_v4_skills_tx.db");
  REQUIRE(db::open(path.string()));
  auto b = db::createBot(db::Bot{0, "测试", "🤖", "", "人设"});

  db::setBotSkills(b.id, {{"calculator", true}});

  // 重复 skill_id 违反 UNIQUE(bot_id, skill_id)，事务应回滚，旧技能集保持不变
  CHECK_THROWS(db::setBotSkills(b.id, {{"datetime", true}, {"datetime", false}}));

  auto list = db::listBotSkills(b.id);
  REQUIRE(list.size() == 1);
  CHECK(list[0].skill_id == "calculator");
  CHECK(list[0].enabled);

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("skills: reminders CRUD 与到期过滤") {
  auto path = tmpDb("ac_v4_reminders.db");
  REQUIRE(db::open(path.string()));
  auto b = db::createBot(db::Bot{0, "测试", "🤖", "", "人设"});

  auto r1 = db::addReminder(b.id, "喝水", "2000-01-01T00:00:00Z");   // 已过期
  auto r2 = db::addReminder(b.id, "睡觉", "2099-01-01T00:00:00Z");   // 未到期
  CHECK(r1.id > 0);
  CHECK(r2.id > 0);

  auto due = db::listReminders(true);
  REQUIRE(due.size() == 1);
  CHECK(due[0].content == "喝水");

  CHECK(db::markReminderDone(r1.id));
  CHECK(db::markReminderDone(9999) == false);
  CHECK(db::listReminders(true).empty());
  auto all = db::listReminders(false);
  CHECK(all.size() == 2);
  CHECK(all[0].done);   // due_at ASC：喝水在前

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("skills: 删除 Bot 级联清 bot_skills 与 reminders") {
  auto path = tmpDb("ac_v4_cascade.db");
  REQUIRE(db::open(path.string()));
  auto b = db::createBot(db::Bot{0, "测试", "🤖", "", "人设"});
  db::setBotSkills(b.id, {{"calculator", true}});
  db::addReminder(b.id, "提醒", "2099-01-01T00:00:00Z");

  CHECK(db::deleteBot(b.id));
  CHECK(db::listBotSkills(b.id).empty());
  CHECK(db::listReminders(false).empty());

  db::close();
  std::filesystem::remove(path);
}
