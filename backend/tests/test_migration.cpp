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

TEST_CASE("migration: 旧 group_messages 放开 role='system' 且历史行不丢") {
  auto path = tmpDb("ac_v4_gm_system.db");
  {
    sqlite3* raw = nullptr;
    std::string file = path.string();
    REQUIRE(sqlite3_open(file.c_str(), &raw) == SQLITE_OK);
    // 旧库形态：role 的 CHECK 只允许 user/assistant，群聊失败留痕写不进去。
    // groups 里放一行父记录，重建后的 FK 校验才有依据。
    const char* sql =
        "CREATE TABLE groups ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  topic TEXT NOT NULL DEFAULT '',"
        "  status TEXT NOT NULL DEFAULT 'idle',"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL);"
        "INSERT INTO groups(id,name,created_at,updated_at) VALUES(1,'旧群','2026-01-01T00:00:00Z','2026-01-01T00:00:00Z');"
        "CREATE TABLE group_messages ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  group_id INTEGER NOT NULL,"
        "  bot_id INTEGER,"
        "  role TEXT NOT NULL CHECK(role IN ('user','assistant')),"
        "  content TEXT NOT NULL,"
        "  created_at TEXT NOT NULL);"
        "INSERT INTO group_messages(group_id,bot_id,role,content,created_at)"
        "  VALUES(1,1,'assistant','旧回复','2026-01-01T00:00:00Z');";
    REQUIRE(sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  REQUIRE(db::open(path.string()));
  auto rows = db::listGroupMessages(1);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].id == 1);
  CHECK(rows[0].content == "旧回复");
  auto note = db::appendGroupMessage(1, 0, "system", "⚠ 甲 回复失败：模拟网络错误");
  CHECK(note.id > 1);
  CHECK(db::listGroupMessages(1).size() == 2);
  db::close();

  // 幂等：升级过的库再次 open 不再重建，也不复制历史行
  REQUIRE(db::open(path.string()));
  CHECK(db::listGroupMessages(1).size() == 2);
  db::close();
  std::filesystem::remove(path);
}
