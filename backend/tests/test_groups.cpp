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

TEST_CASE("groups: CRUD 与成员管理") {
  auto path = tmpDb("ac_v2_groups_crud.db");
  REQUIRE(db::open(path.string()));

  db::Bot ba;
  ba.name = "甲";
  ba.system_prompt = "你是甲";
  db::Bot bb;
  bb.name = "乙";
  bb.system_prompt = "你是乙";
  auto b1 = db::createBot(ba);
  auto b2 = db::createBot(bb);

  auto g = db::createGroup("辩论群", "AI 会取代人类吗");
  CHECK(g.id > 0);
  CHECK(g.status == "idle");

  db::setGroupMembers(g.id, {b1.id, b2.id});
  auto members = db::listGroupMembers(g.id);
  REQUIRE(members.size() == 2);
  CHECK(members[0].position == 0);
  CHECK(members[0].bot_id == b1.id);
  CHECK(members[1].bot_id == b2.id);

  // 全量替换（顺序即发言顺序）
  db::setGroupMembers(g.id, {b2.id, b1.id});
  members = db::listGroupMembers(g.id);
  REQUIRE(members.size() == 2);
  CHECK(members[0].bot_id == b2.id);

  CHECK(db::updateGroup(g.id, "改名群", "新话题"));
  auto got = db::getGroup(g.id);
  REQUIRE(got.has_value());
  CHECK(got->name == "改名群");
  CHECK(got->topic == "新话题");
  CHECK(got->member_count == 2);

  CHECK(db::setGroupStatus(g.id, "running"));
  CHECK(db::getGroup(g.id)->status == "running");

  auto m1 = db::appendGroupMessage(g.id, b1.id, "assistant", "大家好");
  auto m2 = db::appendGroupMessage(g.id, 0, "user", "插句话");
  CHECK(m1.id < m2.id);
  auto msgs = db::listGroupMessages(g.id);
  REQUIRE(msgs.size() == 2);
  CHECK(msgs[0].bot_id == b1.id);
  CHECK(msgs[1].bot_id == 0);
  CHECK(msgs[1].role == "user");

  // 删除机器人 → 其群成员记录被清理
  db::setBotEnabled(b2.id, false);
  CHECK(db::deleteBot(b2.id));
  CHECK(db::listGroupMembers(g.id).size() == 1);

  // 删除群 → 级联删成员与消息
  db::deleteGroup(g.id);
  CHECK(db::listGroupMembers(g.id).empty());
  CHECK(db::listGroupMessages(g.id).empty());
  CHECK(!db::getGroup(g.id).has_value());
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("groups: 列表含成员数与最后活跃时间") {
  auto path = tmpDb("ac_v2_groups_list.db");
  REQUIRE(db::open(path.string()));
  auto def = db::getDefaultBot();
  REQUIRE(def.has_value());
  auto g = db::createGroup("群A", "");
  db::setGroupMembers(g.id, {def->id});
  db::appendGroupMessage(g.id, 0, "user", "第一条");
  auto list = db::listGroups();
  REQUIRE(list.size() == 1);
  CHECK(list[0].member_count == 1);
  CHECK(!list[0].last_active.empty());
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("groups: 重新打开后状态复位 idle") {
  auto path = tmpDb("ac_v2_groups_reset.db");
  REQUIRE(db::open(path.string()));
  auto def = db::getDefaultBot();
  REQUIRE(def.has_value());
  auto g = db::createGroup("群R", "");
  db::setGroupMembers(g.id, {def->id});
  db::setGroupStatus(g.id, "running");
  db::close();

  REQUIRE(db::open(path.string()));
  CHECK(db::getGroup(g.id)->status == "idle");
  db::close();
  std::filesystem::remove(path);
}
