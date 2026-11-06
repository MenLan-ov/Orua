#include "db.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <stdexcept>

#include "json_util.h"
#include "logger.h"
#include "sqlite3.h"

namespace db {
namespace {

sqlite3* g_db = nullptr;
std::recursive_mutex g_mtx;
std::string g_dataDir;

const char* kSchema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS sessions (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  title      TEXT NOT NULL DEFAULT '新对话',
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS messages (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
  role       TEXT NOT NULL CHECK(role IN ('user','assistant','system')),
  content    TEXT NOT NULL,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id, id);
CREATE TABLE IF NOT EXISTS personas (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  name          TEXT NOT NULL,
  avatar        TEXT NOT NULL DEFAULT '🌸',
  greeting      TEXT NOT NULL DEFAULT '',
  system_prompt TEXT NOT NULL,
  temperature   REAL NOT NULL DEFAULT 0.8,
  is_active     INTEGER NOT NULL DEFAULT 0,
  created_at    TEXT NOT NULL,
  updated_at    TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS settings (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS bots (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  name          TEXT NOT NULL,
  avatar        TEXT NOT NULL DEFAULT '🤖',
  greeting      TEXT NOT NULL DEFAULT '',
  system_prompt TEXT NOT NULL,
  temperature   REAL NOT NULL DEFAULT 0.8,
  is_enabled    INTEGER NOT NULL DEFAULT 1,
  api_base_url  TEXT NOT NULL DEFAULT '',
  api_key_enc   TEXT NOT NULL DEFAULT '',
  api_model     TEXT NOT NULL DEFAULT '',
  max_tokens   INTEGER NOT NULL DEFAULT 2048,
  api_type     TEXT NOT NULL DEFAULT 'openai',
  avatar_path  TEXT,
  created_at    TEXT NOT NULL,
  updated_at    TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS groups (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  name       TEXT NOT NULL,
  topic      TEXT NOT NULL DEFAULT '',
  status     TEXT NOT NULL DEFAULT 'idle',
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS group_members (
  id       INTEGER PRIMARY KEY AUTOINCREMENT,
  group_id INTEGER NOT NULL REFERENCES groups(id) ON DELETE CASCADE,
  bot_id   INTEGER NOT NULL REFERENCES bots(id),
  position INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_group_members_group ON group_members(group_id, position);
CREATE TABLE IF NOT EXISTS group_messages (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  group_id   INTEGER NOT NULL REFERENCES groups(id) ON DELETE CASCADE,
  bot_id     INTEGER,
  role       TEXT NOT NULL CHECK(role IN ('user','assistant')),
  content    TEXT NOT NULL,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_group_messages_group ON group_messages(group_id, id);
CREATE TABLE IF NOT EXISTS bot_memories (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  bot_id INTEGER NOT NULL,
  scope TEXT NOT NULL CHECK(scope IN ('private','group')),
  group_id INTEGER,
  content TEXT NOT NULL,
  tags TEXT NOT NULL DEFAULT '[]',
  source_msg_id INTEGER,
  created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
  updated_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
);
CREATE INDEX IF NOT EXISTS idx_mem_bot ON bot_memories(bot_id, scope);
CREATE INDEX IF NOT EXISTS idx_mem_group ON bot_memories(bot_id, group_id);
CREATE TABLE IF NOT EXISTS bot_skills (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  bot_id INTEGER NOT NULL,
  skill_id TEXT NOT NULL,
  enabled INTEGER NOT NULL DEFAULT 1,
  params TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  UNIQUE(bot_id, skill_id)
);
CREATE TABLE IF NOT EXISTS reminders (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  bot_id INTEGER NOT NULL,
  content TEXT NOT NULL,
  due_at TEXT NOT NULL,
  done INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_reminders_due ON reminders(done, due_at);
)SQL";

std::string colText(sqlite3_stmt* st, int i) {
  const unsigned char* t = sqlite3_column_text(st, i);
  return t ? reinterpret_cast<const char*>(t) : "";
}
int64_t colInt(sqlite3_stmt* st, int i) { return sqlite3_column_int64(st, i); }

void exec(const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(g_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "unknown";
    sqlite3_free(err);
    throw std::runtime_error("sqlite exec failed: " + msg);
  }
}

template <typename Fn>
void query(const char* sql, const std::vector<std::string>& params, Fn rowFn) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) != SQLITE_OK)
    throw std::runtime_error("prepare failed: " + std::string(sqlite3_errmsg(g_db)));
  for (size_t i = 0; i < params.size(); ++i)
    sqlite3_bind_text(st, static_cast<int>(i + 1), params[i].c_str(), -1,
                      SQLITE_TRANSIENT);
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) rowFn(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE)
    throw std::runtime_error("step failed: " + std::string(sqlite3_errmsg(g_db)));
}

void execute(const char* sql, const std::vector<std::string>& params = {}) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) != SQLITE_OK)
    throw std::runtime_error("prepare failed: " + std::string(sqlite3_errmsg(g_db)));
  for (size_t i = 0; i < params.size(); ++i)
    sqlite3_bind_text(st, static_cast<int>(i + 1), params[i].c_str(), -1,
                      SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE)
    throw std::runtime_error("step failed: " + std::string(sqlite3_errmsg(g_db)));
}

Session rowToSession(sqlite3_stmt* st) {
  return {colInt(st, 0), colText(st, 1), colInt(st, 2), colText(st, 3), colText(st, 4)};
}
Message rowToMessage(sqlite3_stmt* st) {
  return {colInt(st, 0), colInt(st, 1), colText(st, 2), colText(st, 3),
          colText(st, 4)};
}
Bot rowToBot(sqlite3_stmt* st) {
  Bot b;
  b.id = colInt(st, 0);
  b.name = colText(st, 1);
  b.avatar = colText(st, 2);
  b.greeting = colText(st, 3);
  b.system_prompt = colText(st, 4);
  b.temperature = sqlite3_column_double(st, 5);
  b.is_enabled = colInt(st, 6) != 0;
  b.api_base_url = colText(st, 7);
  b.api_key_enc = colText(st, 8);
  b.api_model = colText(st, 9);
  b.max_tokens = static_cast<int>(colInt(st, 10));
  b.api_type = colText(st, 11);
  b.avatar_path = colText(st, 12);
  b.created_at = colText(st, 13);
  b.updated_at = colText(st, 14);
  return b;
}

Memory rowToMemory(sqlite3_stmt* st) {
  Memory m;
  m.id = colInt(st, 0);
  m.bot_id = colInt(st, 1);
  m.scope = colText(st, 2);
  m.group_id = colInt(st, 3);
  m.content = colText(st, 4);
  m.tags = colText(st, 5);
  m.source_msg_id = colInt(st, 6);
  m.created_at = colText(st, 7);
  m.updated_at = colText(st, 8);
  return m;
}
BotSkill rowToSkill(sqlite3_stmt* st) {
  BotSkill s;
  s.id = colInt(st, 0);
  s.bot_id = colInt(st, 1);
  s.skill_id = colText(st, 2);
  s.enabled = colInt(st, 3) != 0;
  s.params = colText(st, 4);
  s.created_at = colText(st, 5);
  s.updated_at = colText(st, 6);
  return s;
}
Reminder rowToReminder(sqlite3_stmt* st) {
  Reminder r;
  r.id = colInt(st, 0);
  r.bot_id = colInt(st, 1);
  r.content = colText(st, 2);
  r.due_at = colText(st, 3);
  r.done = colInt(st, 4) != 0;
  r.created_at = colText(st, 5);
  return r;
}

}  // namespace

bool open(const std::string& dbPath) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  if (g_db) return true;
  g_dataDir = std::filesystem::path(dbPath).parent_path().string();
  if (sqlite3_open(dbPath.c_str(), &g_db) != SQLITE_OK) {
    logx::error("db open failed: " + std::string(sqlite3_errmsg(g_db)));
    sqlite3_close(g_db);
    g_db = nullptr;
    return false;
  }
  try {
    exec(kSchema);
    // sessions.bot_id 列迁移（旧库 ALTER 补列；新库建表后同样走此路径）
    bool hasBotId = false;
    query("PRAGMA table_info(sessions)", {},
          [&](sqlite3_stmt* st) {
            if (colText(st, 1) == "bot_id") hasBotId = true;
          });
    if (!hasBotId)
      exec("ALTER TABLE sessions ADD COLUMN bot_id INTEGER NOT NULL DEFAULT 0");

    // bots 新列（v3：max_tokens/api_type/avatar_path），幂等：新库建表后同样走此路径
    auto ensureBotCol = [&](const char* col, const char* ddl) {
      bool has = false;
      query("PRAGMA table_info(bots)", {},
            [&](sqlite3_stmt* st) {
              if (colText(st, 1) == col) has = true;
            });
      if (!has) exec(ddl);
    };
    ensureBotCol("max_tokens",
                 "ALTER TABLE bots ADD COLUMN max_tokens INTEGER NOT NULL DEFAULT 2048");
    ensureBotCol("api_type",
                 "ALTER TABLE bots ADD COLUMN api_type TEXT NOT NULL DEFAULT 'openai'");
    ensureBotCol("avatar_path", "ALTER TABLE bots ADD COLUMN avatar_path TEXT");

    // personas → bots 一次性迁移（幂等：bots 已有数据则跳过）
    bool hasBots = false;
    query("SELECT COUNT(*) FROM bots", {},
          [&](sqlite3_stmt* st) { hasBots = colInt(st, 0) > 0; });
    if (!hasBots) {
      bool hasPersonas = false;
      query("SELECT COUNT(*) FROM personas", {},
            [&](sqlite3_stmt* st) { hasPersonas = colInt(st, 0) > 0; });
      bool defSet = false;
      if (hasPersonas) {
        // 直接查询 personas 表（Persona 结构体与 CRUD 已随本任务移除，表仅作迁移数据源）
        query("SELECT name,avatar,greeting,system_prompt,temperature,is_active "
              "FROM personas ORDER BY id ASC", {},
              [&](sqlite3_stmt* st) {
                Bot b;
                b.name = colText(st, 0);
                b.avatar = colText(st, 1);
                b.greeting = colText(st, 2);
                b.system_prompt = colText(st, 3);
                b.temperature = sqlite3_column_double(st, 4);
                Bot nb = createBot(b);
                if (colInt(st, 5) != 0 && !defSet) {
                  setSetting("default_bot_id", std::to_string(nb.id));
                  defSet = true;
                }
              });
      } else {
        Bot d;
        d.name = "默认助手";
        d.greeting = "你好呀，我是你的 AI 陪伴，有什么想聊的吗？";
        d.system_prompt = "你是一个温暖、耐心的 AI 陪伴助手，用自然友好的语气和用户聊天。";
        Bot nb = createBot(d);
        setSetting("default_bot_id", std::to_string(nb.id));
        defSet = true;
      }
      if (!defSet) {  // 旧库无激活人设：兜底取第一个
        std::optional<int64_t> firstId;
        query("SELECT id FROM bots ORDER BY id ASC LIMIT 1", {},
              [&](sqlite3_stmt* st) { firstId = colInt(st, 0); });
        if (firstId) setSetting("default_bot_id", std::to_string(*firstId));
      }
    }
    exec("UPDATE groups SET status='idle'");
    return true;
  } catch (const std::exception& e) {
    logx::error(std::string("db init failed: ") + e.what());
    return false;
  }
}

void close() {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  if (g_db) {
    sqlite3_close(g_db);
    g_db = nullptr;
  }
}

Session createSession(const std::string& title, int64_t botId) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::string ts = nowIso();
  execute("INSERT INTO sessions(title, bot_id, created_at, updated_at) VALUES(?,?,?,?)",
          {title, std::to_string(botId), ts, ts});
  return {sqlite3_last_insert_rowid(g_db), title, botId, ts, ts};
}

void setSessionBot(int64_t sessionId, int64_t botId) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("UPDATE sessions SET bot_id=?, updated_at=? WHERE id=?",
          {std::to_string(botId), nowIso(), std::to_string(sessionId)});
}

std::vector<Session> listSessions() {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::vector<Session> out;
  query("SELECT id,title,bot_id,created_at,updated_at FROM sessions ORDER BY updated_at DESC",
        {}, [&](sqlite3_stmt* st) { out.push_back(rowToSession(st)); });
  return out;
}

void renameSession(int64_t id, const std::string& title) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("UPDATE sessions SET title=?, updated_at=? WHERE id=?",
          {title, nowIso(), std::to_string(id)});
}

void deleteSession(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("DELETE FROM sessions WHERE id=?", {std::to_string(id)});
}

void touchSession(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("UPDATE sessions SET updated_at=? WHERE id=?",
          {nowIso(), std::to_string(id)});
}

std::optional<Session> getSession(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::optional<Session> out;
  query("SELECT id,title,bot_id,created_at,updated_at FROM sessions WHERE id=?",
        {std::to_string(id)},
        [&](sqlite3_stmt* st) { out = rowToSession(st); });
  return out;
}

Message appendMessage(int64_t sessionId, const std::string& role,
                      const std::string& content) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::string ts = nowIso();
  execute("INSERT INTO messages(session_id, role, content, created_at) VALUES(?,?,?,?)",
          {std::to_string(sessionId), role, content, ts});
  return {sqlite3_last_insert_rowid(g_db), sessionId, role, content, ts};
}

std::vector<Message> listMessages(int64_t sessionId, int limit) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::vector<Message> out;
  query("SELECT id,session_id,role,content,created_at FROM messages "
        "WHERE session_id=? ORDER BY id DESC LIMIT ?",
        {std::to_string(sessionId), std::to_string(limit)},
        [&](sqlite3_stmt* st) { out.push_back(rowToMessage(st)); });
  std::reverse(out.begin(), out.end());
  return out;
}

void deleteMessage(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("DELETE FROM messages WHERE id=?", {std::to_string(id)});
}

void clearMessages(int64_t sessionId) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("DELETE FROM messages WHERE session_id=?", {std::to_string(sessionId)});
}

std::optional<Message> lastMessageByRole(int64_t sessionId,
                                         const std::string& role) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::optional<Message> out;
  query("SELECT id,session_id,role,content,created_at FROM messages "
        "WHERE session_id=? AND role=? ORDER BY id DESC LIMIT 1",
        {std::to_string(sessionId), role},
        [&](sqlite3_stmt* st) { out = rowToMessage(st); });
  return out;
}

Bot createBot(const Bot& b) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::string ts = nowIso();
  execute("INSERT INTO bots(name,avatar,greeting,system_prompt,temperature,is_enabled,"
          "api_base_url,api_key_enc,api_model,max_tokens,api_type,avatar_path,"
          "created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
          {b.name, b.avatar, b.greeting, b.system_prompt,
           std::to_string(b.temperature), b.is_enabled ? "1" : "0",
           b.api_base_url, b.api_key_enc, b.api_model,
           std::to_string(b.max_tokens), b.api_type, b.avatar_path, ts, ts});
  Bot out = b;
  out.id = sqlite3_last_insert_rowid(g_db);
  out.created_at = out.updated_at = ts;
  return out;
}

bool updateBot(const Bot& b) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("UPDATE bots SET name=?,avatar=?,greeting=?,system_prompt=?,temperature=?,"
          "is_enabled=?,api_base_url=?,api_key_enc=?,api_model=?,max_tokens=?,"
          "api_type=?,avatar_path=?,updated_at=? WHERE id=?",
          {b.name, b.avatar, b.greeting, b.system_prompt,
           std::to_string(b.temperature), b.is_enabled ? "1" : "0",
           b.api_base_url, b.api_key_enc, b.api_model,
           std::to_string(b.max_tokens), b.api_type, b.avatar_path,
           nowIso(), std::to_string(b.id)});
  return sqlite3_changes(g_db) > 0;
}

bool deleteBot(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  auto def = getDefaultBot();   // recursive_mutex 可重入
  if (def && def->id == id) return false;
  execute("UPDATE sessions SET bot_id=0 WHERE bot_id=?", {std::to_string(id)});
  execute("DELETE FROM group_members WHERE bot_id=?", {std::to_string(id)});
  execute("DELETE FROM bots WHERE id=?", {std::to_string(id)});
  deleteMemoriesByBot(id);
  execute("DELETE FROM bot_skills WHERE bot_id=?", {std::to_string(id)});
  execute("DELETE FROM reminders WHERE bot_id=?", {std::to_string(id)});
  return true;
}

std::vector<Bot> listBots() {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::vector<Bot> out;
  query("SELECT id,name,avatar,greeting,system_prompt,temperature,is_enabled,"
        "api_base_url,api_key_enc,api_model,max_tokens,api_type,avatar_path,"
        "created_at,updated_at FROM bots ORDER BY id ASC",
        {}, [&](sqlite3_stmt* st) { out.push_back(rowToBot(st)); });
  return out;
}

std::optional<Bot> getBot(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::optional<Bot> out;
  query("SELECT id,name,avatar,greeting,system_prompt,temperature,is_enabled,"
        "api_base_url,api_key_enc,api_model,max_tokens,api_type,avatar_path,"
        "created_at,updated_at FROM bots WHERE id=?",
        {std::to_string(id)},
        [&](sqlite3_stmt* st) { out = rowToBot(st); });
  return out;
}

bool setBotEnabled(int64_t id, bool enabled) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("UPDATE bots SET is_enabled=?, updated_at=? WHERE id=?",
          {enabled ? "1" : "0", nowIso(), std::to_string(id)});
  return sqlite3_changes(g_db) > 0;
}

std::optional<Bot> getDefaultBot() {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  auto idStr = getSetting("default_bot_id");
  if (idStr) {
    try {
      auto b = getBot(std::stoll(*idStr));
      if (b) return b;
    } catch (...) {
    }
  }
  std::optional<Bot> out;
  query("SELECT id,name,avatar,greeting,system_prompt,temperature,is_enabled,"
        "api_base_url,api_key_enc,api_model,max_tokens,api_type,avatar_path,"
        "created_at,updated_at FROM bots ORDER BY id ASC LIMIT 1",
        {}, [&](sqlite3_stmt* st) { out = rowToBot(st); });
  return out;
}

Group rowToGroup(sqlite3_stmt* st) {
  Group g;
  g.id = colInt(st, 0);
  g.name = colText(st, 1);
  g.topic = colText(st, 2);
  g.status = colText(st, 3);
  g.created_at = colText(st, 4);
  g.updated_at = colText(st, 5);
  g.member_count = static_cast<int>(colInt(st, 6));
  g.last_active = colText(st, 7);
  return g;
}

const char* kGroupCols = "g.id, g.name, g.topic, g.status, g.created_at, g.updated_at, "
                         "(SELECT COUNT(*) FROM group_members m WHERE m.group_id=g.id), "
                         "(SELECT MAX(created_at) FROM group_messages gm WHERE gm.group_id=g.id) ";

Group createGroup(const std::string& name, const std::string& topic) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::string ts = nowIso();
  execute("INSERT INTO groups(name, topic, created_at, updated_at) VALUES(?,?,?,?)",
          {name, topic, ts, ts});
  Group g;
  g.id = sqlite3_last_insert_rowid(g_db);
  g.name = name;
  g.topic = topic;
  g.status = "idle";
  g.created_at = g.updated_at = ts;
  return g;
}

bool updateGroup(int64_t id, const std::optional<std::string>& name,
                 const std::optional<std::string>& topic) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  if (name) execute("UPDATE groups SET name=?, updated_at=? WHERE id=?",
                    {*name, nowIso(), std::to_string(id)});
  if (topic) execute("UPDATE groups SET topic=?, updated_at=? WHERE id=?",
                     {*topic, nowIso(), std::to_string(id)});
  return true;
}

bool setGroupStatus(int64_t id, const std::string& status) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("UPDATE groups SET status=?, updated_at=? WHERE id=?",
          {status, nowIso(), std::to_string(id)});
  return sqlite3_changes(g_db) > 0;
}

void touchGroup(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("UPDATE groups SET updated_at=? WHERE id=?", {nowIso(), std::to_string(id)});
}

void deleteGroup(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("DELETE FROM groups WHERE id=?", {std::to_string(id)});
  deleteMemoriesByGroup(id);
}

std::vector<Group> listGroups() {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::vector<Group> out;
  std::string sql = std::string("SELECT ") + kGroupCols +
                    "FROM groups g ORDER BY g.updated_at DESC";
  query(sql.c_str(), {}, [&](sqlite3_stmt* st) { out.push_back(rowToGroup(st)); });
  return out;
}

std::optional<Group> getGroup(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::optional<Group> out;
  std::string sql = std::string("SELECT ") + kGroupCols + "FROM groups g WHERE g.id=?";
  query(sql.c_str(), {std::to_string(id)},
        [&](sqlite3_stmt* st) { out = rowToGroup(st); });
  return out;
}

void setGroupMembers(int64_t groupId, const std::vector<int64_t>& botIds) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("DELETE FROM group_members WHERE group_id=?", {std::to_string(groupId)});
  for (size_t i = 0; i < botIds.size(); ++i)
    execute("INSERT INTO group_members(group_id, bot_id, position) VALUES(?,?,?)",
            {std::to_string(groupId), std::to_string(botIds[i]), std::to_string(i)});
  execute("UPDATE groups SET updated_at=? WHERE id=?",
          {nowIso(), std::to_string(groupId)});
}

std::vector<GroupMember> listGroupMembers(int64_t groupId) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::vector<GroupMember> out;
  query("SELECT id, group_id, bot_id, position FROM group_members "
        "WHERE group_id=? ORDER BY position ASC",
        {std::to_string(groupId)}, [&](sqlite3_stmt* st) {
          out.push_back({colInt(st, 0), colInt(st, 1), colInt(st, 2),
                         static_cast<int>(colInt(st, 3))});
        });
  return out;
}

GroupMessage appendGroupMessage(int64_t groupId, int64_t botId,
                                const std::string& role, const std::string& content) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::string ts = nowIso();
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(g_db,
      "INSERT INTO group_messages(group_id, bot_id, role, content, created_at) "
      "VALUES(?,?,?,?,?)", -1, &st, nullptr) != SQLITE_OK)
    throw std::runtime_error("prepare failed: " + std::string(sqlite3_errmsg(g_db)));
  sqlite3_bind_int64(st, 1, groupId);
  if (botId > 0) sqlite3_bind_int64(st, 2, botId);
  else sqlite3_bind_null(st, 2);
  sqlite3_bind_text(st, 3, role.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, ts.c_str(), -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE)
    throw std::runtime_error("step failed: " + std::string(sqlite3_errmsg(g_db)));
  execute("UPDATE groups SET updated_at=? WHERE id=?",
          {ts, std::to_string(groupId)});
  return {sqlite3_last_insert_rowid(g_db), groupId, botId, role, content, ts};
}

std::vector<GroupMessage> listGroupMessages(int64_t groupId, int limit) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::vector<GroupMessage> out;
  query("SELECT id, group_id, bot_id, role, content, created_at FROM group_messages "
        "WHERE group_id=? ORDER BY id ASC",
        {std::to_string(groupId)}, [&](sqlite3_stmt* st) {
          out.push_back({colInt(st, 0), colInt(st, 1), colInt(st, 2), colText(st, 3),
                         colText(st, 4), colText(st, 5)});
        });
  if (limit > 0 && out.size() > static_cast<size_t>(limit))
    out.erase(out.begin(), out.end() - limit);
  return out;
}

std::optional<std::string> getSetting(const std::string& key) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::optional<std::string> out;
  query("SELECT value FROM settings WHERE key=?", {key},
        [&](sqlite3_stmt* st) { out = colText(st, 0); });
  return out;
}

void setSetting(const std::string& key, const std::string& value) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("INSERT INTO settings(key,value) VALUES(?,?) "
          "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
          {key, value});
}

Memory addMemory(int64_t botId, const std::string& scope, int64_t groupId,
                 const std::string& content, const std::string& tags,
                 int64_t sourceMsgId) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::string ts = nowIso();
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(g_db,
      "INSERT INTO bot_memories(bot_id,scope,group_id,content,tags,source_msg_id,"
      "created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)", -1, &st, nullptr) != SQLITE_OK)
    throw std::runtime_error("prepare failed: " + std::string(sqlite3_errmsg(g_db)));
  sqlite3_bind_int64(st, 1, botId);
  sqlite3_bind_text(st, 2, scope.c_str(), -1, SQLITE_TRANSIENT);
  // 规格 §3.2：private 记忆 group_id=0 → 库内绑定 SQL NULL；source_msg_id 同理
  if (groupId == 0) sqlite3_bind_null(st, 3);
  else sqlite3_bind_int64(st, 3, groupId);
  sqlite3_bind_text(st, 4, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, tags.c_str(), -1, SQLITE_TRANSIENT);
  if (sourceMsgId == 0) sqlite3_bind_null(st, 6);
  else sqlite3_bind_int64(st, 6, sourceMsgId);
  sqlite3_bind_text(st, 7, ts.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 8, ts.c_str(), -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE)
    throw std::runtime_error("step failed: " + std::string(sqlite3_errmsg(g_db)));
  Memory m;
  m.id = sqlite3_last_insert_rowid(g_db);
  m.bot_id = botId; m.scope = scope; m.group_id = groupId;
  m.content = content; m.tags = tags; m.source_msg_id = sourceMsgId;
  m.created_at = m.updated_at = ts;
  return m;
}

bool updateMemoryContent(int64_t id, const std::string& content) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("UPDATE bot_memories SET content=?, updated_at=? WHERE id=?",
          {content, nowIso(), std::to_string(id)});
  return sqlite3_changes(g_db) > 0;
}

bool deleteMemory(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("DELETE FROM bot_memories WHERE id=?", {std::to_string(id)});
  return sqlite3_changes(g_db) > 0;
}

std::vector<Memory> listMemories(int64_t botId, const std::string& scope,
                                 int64_t groupId, const std::string& q) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::vector<Memory> out;
  std::string sql =
      "SELECT id,bot_id,scope,COALESCE(group_id,0),content,tags,"
      "COALESCE(source_msg_id,0),created_at,updated_at "
      "FROM bot_memories WHERE bot_id=?";
  std::vector<std::string> params = {std::to_string(botId)};
  if (scope == "private" || scope == "group") {
    sql += " AND scope=?";
    params.push_back(scope);
  }
  if (groupId >= 0) {
    sql += " AND group_id=?";
    params.push_back(std::to_string(groupId));
  }
  if (!q.empty()) {
    sql += " AND (content LIKE ? OR tags LIKE ?)";
    params.push_back("%" + q + "%");
    params.push_back("%" + q + "%");
  }
  sql += " ORDER BY updated_at DESC, id DESC";
  query(sql.c_str(), params, [&](sqlite3_stmt* st) { out.push_back(rowToMemory(st)); });
  return out;
}

void deleteMemoriesByBot(int64_t botId) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("DELETE FROM bot_memories WHERE bot_id=?", {std::to_string(botId)});
}

void deleteMemoriesByGroup(int64_t groupId) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("DELETE FROM bot_memories WHERE group_id=?", {std::to_string(groupId)});
}

std::string dataDir() { return g_dataDir; }

std::optional<int64_t> memorySourceSession(int64_t msgId) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::optional<int64_t> out;
  query("SELECT session_id FROM messages WHERE id=?", {std::to_string(msgId)},
        [&](sqlite3_stmt* st) { out = colInt(st, 0); });
  return out;
}

void setBotSkills(int64_t botId,
                  const std::vector<std::pair<std::string, bool>>& skills) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::string ts = nowIso();
  execute("DELETE FROM bot_skills WHERE bot_id=?", {std::to_string(botId)});
  for (auto& [sid, on] : skills) {
    execute("INSERT INTO bot_skills(bot_id,skill_id,enabled,params,created_at,updated_at) "
            "VALUES(?,?,?,?,?,?)",
            {std::to_string(botId), sid, on ? "1" : "0", "{}", ts, ts});
  }
}

std::vector<BotSkill> listBotSkills(int64_t botId) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::vector<BotSkill> out;
  query("SELECT id,bot_id,skill_id,enabled,params,created_at,updated_at "
        "FROM bot_skills WHERE bot_id=? ORDER BY id ASC",
        {std::to_string(botId)},
        [&](sqlite3_stmt* st) { out.push_back(rowToSkill(st)); });
  return out;
}

Reminder addReminder(int64_t botId, const std::string& content,
                     const std::string& dueAt) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::string ts = nowIso();
  execute("INSERT INTO reminders(bot_id,content,due_at,done,created_at) VALUES(?,?,?,0,?)",
          {std::to_string(botId), content, dueAt, ts});
  Reminder r;
  r.id = sqlite3_last_insert_rowid(g_db);
  r.bot_id = botId;
  r.content = content;
  r.due_at = dueAt;
  r.done = false;
  r.created_at = ts;
  return r;
}

std::vector<Reminder> listReminders(bool dueOnly) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::vector<Reminder> out;
  std::string sql = "SELECT id,bot_id,content,due_at,done,created_at FROM reminders";
  if (dueOnly) {
    sql += " WHERE done=0 AND due_at <= ? ORDER BY due_at ASC";
    query(sql.c_str(), {nowIso()},
          [&](sqlite3_stmt* st) { out.push_back(rowToReminder(st)); });
  } else {
    sql += " ORDER BY due_at ASC";
    query(sql.c_str(), {}, [&](sqlite3_stmt* st) { out.push_back(rowToReminder(st)); });
  }
  return out;
}

bool markReminderDone(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("UPDATE reminders SET done=1 WHERE id=?", {std::to_string(id)});
  return sqlite3_changes(g_db) > 0;
}

}  // namespace db
