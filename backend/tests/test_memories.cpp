#include <doctest/doctest.h>
#include <filesystem>

#include "db.h"
#include "sqlite3.h"

namespace {
std::filesystem::path tmpDb(const char* name) {
  auto p = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove(p);
  return p;
}

// 手工构造 v2 形状的 bots 表（无 v3 新列）用于迁移测试
void createV2ShapeDb(const std::filesystem::path& p) {
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(p.string().c_str(), &raw) == SQLITE_OK);
  const char* sql =
      "CREATE TABLE sessions (id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, "
      "bot_id INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL, updated_at TEXT NOT NULL);"
      "CREATE TABLE messages (id INTEGER PRIMARY KEY AUTOINCREMENT, session_id INTEGER NOT NULL, "
      "role TEXT NOT NULL, content TEXT NOT NULL, created_at TEXT NOT NULL);"
      "CREATE TABLE personas (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, "
      "avatar TEXT NOT NULL DEFAULT '', greeting TEXT NOT NULL DEFAULT '', "
      "system_prompt TEXT NOT NULL, temperature REAL NOT NULL DEFAULT 0.8, "
      "is_active INTEGER NOT NULL DEFAULT 0);"
      "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
      "CREATE TABLE bots (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, "
      "avatar TEXT NOT NULL DEFAULT '', greeting TEXT NOT NULL DEFAULT '', "
      "system_prompt TEXT NOT NULL, temperature REAL NOT NULL DEFAULT 0.8, "
      "is_enabled INTEGER NOT NULL DEFAULT 1, api_base_url TEXT NOT NULL DEFAULT '', "
      "api_key_enc TEXT NOT NULL DEFAULT '', api_model TEXT NOT NULL DEFAULT '', "
      "created_at TEXT NOT NULL, updated_at TEXT NOT NULL);"
      "CREATE TABLE groups (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, "
      "topic TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'idle', "
      "created_at TEXT NOT NULL, updated_at TEXT NOT NULL);"
      "CREATE TABLE group_members (id INTEGER PRIMARY KEY AUTOINCREMENT, "
      "group_id INTEGER NOT NULL, bot_id INTEGER NOT NULL, position INTEGER NOT NULL);"
      "CREATE TABLE group_messages (id INTEGER PRIMARY KEY AUTOINCREMENT, "
      "group_id INTEGER NOT NULL, bot_id INTEGER, role TEXT NOT NULL, "
      "content TEXT NOT NULL, created_at TEXT NOT NULL);"
      "INSERT INTO bots(name,avatar,greeting,system_prompt,temperature,is_enabled,"
      "api_base_url,api_key_enc,api_model,created_at,updated_at) "
      "VALUES('旧Bot','�','','旧人设',0.8,1,'','','','2026-01-01','2026-01-01');"
      "INSERT INTO settings(key,value) VALUES('default_bot_id','1');";
  char* err = nullptr;
  REQUIRE(sqlite3_exec(raw, sql, nullptr, nullptr, &err) == SQLITE_OK);
  sqlite3_close(raw);
}
}  // namespace

TEST_CASE("memories: CRUD 与 scope/group 过滤") {
  auto path = tmpDb("ac_v3_mem_crud.db");
  REQUIRE(db::open(path.string()));

  auto b = db::createBot(db::Bot{0, "测试", "🤖", "", "人设"});
  auto m1 = db::addMemory(b.id, "private", 0, "用户喜欢咖啡", "[\"咖啡\",\"偏好\"]", 0);
  CHECK(m1.id > 0);
  auto m2 = db::addMemory(b.id, "group", 7, "群里约定每周五复盘", "[\"约定\"]", 0);
  CHECK(m2.id > 0);

  auto all = db::listMemories(b.id);
  CHECK(all.size() == 2);
  auto priv = db::listMemories(b.id, "private");
  REQUIRE(priv.size() == 1);
  CHECK(priv[0].content == "用户喜欢咖啡");
  CHECK(priv[0].tags == "[\"咖啡\",\"偏好\"]");
  auto grp = db::listMemories(b.id, "group", 7);
  REQUIRE(grp.size() == 1);
  CHECK(grp[0].group_id == 7);
  CHECK(db::listMemories(b.id, "group", 8).empty());

  CHECK(db::updateMemoryContent(m1.id, "用户喜欢拿铁"));
  CHECK(db::getBot(b.id).has_value());  // 无关断言占位，保持 getBot 可用
  CHECK(db::listMemories(b.id, "private")[0].content == "用户喜欢拿铁");
  CHECK(db::updateMemoryContent(9999, "x") == false);

  CHECK(db::deleteMemory(m2.id));
  CHECK(db::listMemories(b.id).size() == 1);
  CHECK(db::deleteMemory(9999) == false);

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("memories: q 关键词搜索（content 与 tags）") {
  auto path = tmpDb("ac_v3_mem_search.db");
  REQUIRE(db::open(path.string()));
  auto b = db::createBot(db::Bot{0, "测试", "🤖", "", "人设"});
  db::addMemory(b.id, "private", 0, "生日是五月", "[\"生日\"]", 0);
  db::addMemory(b.id, "private", 0, "喜欢海边", "[\"旅行\"]", 0);

  CHECK(db::listMemories(b.id, "private", -1, "生日").size() == 1);
  CHECK(db::listMemories(b.id, "private", -1, "旅行").size() == 1);
  CHECK(db::listMemories(b.id, "private", -1, "不存在词").empty());

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("memories: 删除 Bot 级联清记忆") {
  auto path = tmpDb("ac_v3_mem_cascade_bot.db");
  REQUIRE(db::open(path.string()));
  auto b = db::createBot(db::Bot{0, "测试", "🤖", "", "人设"});
  db::addMemory(b.id, "private", 0, "记忆1", "[]", 0);
  auto def = db::getDefaultBot();
  REQUIRE(def.has_value());
  CHECK(db::deleteBot(b.id));
  CHECK(db::listMemories(b.id).empty());
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("memories: 删除群级联清群记忆") {
  auto path = tmpDb("ac_v3_mem_cascade_group.db");
  REQUIRE(db::open(path.string()));
  auto b = db::createBot(db::Bot{0, "测试", "🤖", "", "人设"});
  auto g = db::createGroup("测试群", "");
  db::addMemory(b.id, "group", g.id, "群记忆1", "[]", 0);
  db::deleteGroup(g.id);
  CHECK(db::listMemories(b.id, "group", g.id).empty());
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("memories: bots 新列与 v2 老库迁移") {
  auto path = tmpDb("ac_v3_mem_migrate.db");
  createV2ShapeDb(path);
  REQUIRE(db::open(path.string()));
  auto bots = db::listBots();
  REQUIRE(bots.size() == 1);
  CHECK(bots[0].name == "旧Bot");
  CHECK(bots[0].max_tokens == 2048);   // 迁移默认值
  CHECK(bots[0].api_type == "openai");
  CHECK(bots[0].avatar_path.empty());

  db::Bot nb;
  nb.name = "新Bot";
  nb.system_prompt = "人设";
  nb.max_tokens = 4096;
  nb.api_type = "openai";
  nb.avatar_path = "uploads/9.png";
  auto c = db::createBot(nb);
  auto got = db::getBot(c.id);
  REQUIRE(got.has_value());
  CHECK(got->max_tokens == 4096);
  CHECK(got->api_type == "openai");
  CHECK(got->avatar_path == "uploads/9.png");

  c.max_tokens = 1024;
  CHECK(db::updateBot(c));
  CHECK(db::getBot(c.id)->max_tokens == 1024);
  CHECK(!db::dataDir().empty());

  db::close();
  std::filesystem::remove(path);
}
