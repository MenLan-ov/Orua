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

// 手工构造 v1 旧库：sessions 无 bot_id、personas 有激活数据
void makeV1Db(const std::string& path) {
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  const char* schema = R"SQL(
    CREATE TABLE sessions (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      title TEXT NOT NULL DEFAULT '新对话',
      created_at TEXT NOT NULL,
      updated_at TEXT NOT NULL
    );
    CREATE TABLE messages (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
      role TEXT NOT NULL CHECK(role IN ('user','assistant','system')),
      content TEXT NOT NULL,
      created_at TEXT NOT NULL
    );
    CREATE TABLE personas (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      name TEXT NOT NULL,
      avatar TEXT NOT NULL DEFAULT '🌸',
      greeting TEXT NOT NULL DEFAULT '',
      system_prompt TEXT NOT NULL,
      temperature REAL NOT NULL DEFAULT 0.8,
      is_active INTEGER NOT NULL DEFAULT 0,
      created_at TEXT NOT NULL,
      updated_at TEXT NOT NULL
    );
    CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
  )SQL";
  REQUIRE(sqlite3_exec(raw, schema, nullptr, nullptr, nullptr) == SQLITE_OK);
  const char* data = R"SQL(
    INSERT INTO personas(name,avatar,greeting,system_prompt,temperature,is_active,created_at,updated_at)
      VALUES('旧人设','🌸','你好','你是旧人设',0.7,1,'2026-01-01T00:00:00Z','2026-01-01T00:00:00Z');
    INSERT INTO sessions(title,created_at,updated_at) VALUES('旧会话1','2026-01-01T00:00:00Z','2026-01-01T00:00:00Z');
    INSERT INTO sessions(title,created_at,updated_at) VALUES('旧会话2','2026-01-02T00:00:00Z','2026-01-02T00:00:00Z');
  )SQL";
  REQUIRE(sqlite3_exec(raw, data, nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(raw);
}
}  // namespace

TEST_CASE("migration: v1 personas 迁移为 bots 且幂等") {
  auto path = tmpDb("ac_v2_migrate.db");
  makeV1Db(path.string());

  REQUIRE(db::open(path.string()));
  auto bots = db::listBots();
  REQUIRE(bots.size() == 1);
  CHECK(bots[0].name == "旧人设");
  CHECK(bots[0].system_prompt == "你是旧人设");
  CHECK(bots[0].avatar == "🌸");
  auto def = db::getDefaultBot();
  REQUIRE(def.has_value());
  CHECK(def->id == bots[0].id);

  auto sessions = db::listSessions();
  REQUIRE(sessions.size() == 2);
  CHECK(sessions[0].bot_id == 0);   // 旧会话 bot_id 回填 0

  // bot_id 列真实存在（新会话可绑定）
  auto s = db::createSession();
  CHECK(s.bot_id == 0);
  db::setSessionBot(s.id, def->id);
  CHECK(db::getSession(s.id)->bot_id == def->id);
  db::close();

  // 幂等：重复 open 不重复迁移、不丢数据
  REQUIRE(db::open(path.string()));
  CHECK(db::listBots().size() == 1);
  CHECK(db::listSessions().size() == 3);
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("migration: 空库播种默认机器人") {
  auto path = tmpDb("ac_v2_migrate_seed.db");
  REQUIRE(db::open(path.string()));
  auto bots = db::listBots();
  REQUIRE(bots.size() == 1);
  CHECK(bots[0].name == "默认助手");
  auto def = db::getDefaultBot();
  REQUIRE(def.has_value());
  CHECK(def->id == bots[0].id);
  db::close();
  std::filesystem::remove(path);
}
