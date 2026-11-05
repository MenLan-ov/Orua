#include "db.h"

#include <mutex>
#include <stdexcept>

#include "json_util.h"
#include "logger.h"
#include "sqlite3.h"

namespace db {
namespace {

sqlite3* g_db = nullptr;
std::recursive_mutex g_mtx;

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
Persona rowToPersona(sqlite3_stmt* st) {
  Persona p;
  p.id = colInt(st, 0);
  p.name = colText(st, 1);
  p.avatar = colText(st, 2);
  p.greeting = colText(st, 3);
  p.system_prompt = colText(st, 4);
  p.temperature = sqlite3_column_double(st, 5);
  p.is_active = colInt(st, 6) != 0;
  p.created_at = colText(st, 7);
  p.updated_at = colText(st, 8);
  return p;
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
  b.created_at = colText(st, 10);
  b.updated_at = colText(st, 11);
  return b;
}

}  // namespace

bool open(const std::string& dbPath) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  if (g_db) return true;
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
        for (auto& p : listPersonas()) {
          Bot b;
          b.name = p.name;
          b.avatar = p.avatar;
          b.greeting = p.greeting;
          b.system_prompt = p.system_prompt;
          b.temperature = p.temperature;
          Bot nb = createBot(b);
          if (p.is_active && !defSet) {
            setSetting("default_bot_id", std::to_string(nb.id));
            defSet = true;
          }
        }
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
        "WHERE session_id=? ORDER BY id ASC LIMIT ?",
        {std::to_string(sessionId), std::to_string(limit)},
        [&](sqlite3_stmt* st) { out.push_back(rowToMessage(st)); });
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

Persona createPersona(const Persona& p) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::string ts = nowIso();
  execute("INSERT INTO personas(name,avatar,greeting,system_prompt,temperature,"
          "is_active,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)",
          {p.name, p.avatar, p.greeting, p.system_prompt,
           std::to_string(p.temperature), p.is_active ? "1" : "0", ts, ts});
  Persona out = p;
  out.id = sqlite3_last_insert_rowid(g_db);
  out.created_at = out.updated_at = ts;
  if (p.is_active) activatePersona(out.id);
  return out;
}

bool updatePersona(const Persona& p) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("UPDATE personas SET name=?,avatar=?,greeting=?,system_prompt=?,"
          "temperature=?,updated_at=? WHERE id=?",
          {p.name, p.avatar, p.greeting, p.system_prompt,
           std::to_string(p.temperature), nowIso(), std::to_string(p.id)});
  return sqlite3_changes(g_db) > 0;
}

bool deletePersona(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  bool active = false;
  query("SELECT is_active FROM personas WHERE id=?",
        {std::to_string(id)},
        [&](sqlite3_stmt* st) { active = colInt(st, 0) != 0; });
  if (active) return false;
  execute("DELETE FROM personas WHERE id=?", {std::to_string(id)});
  return true;
}

std::vector<Persona> listPersonas() {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::vector<Persona> out;
  query("SELECT id,name,avatar,greeting,system_prompt,temperature,is_active,"
        "created_at,updated_at FROM personas ORDER BY id ASC",
        {}, [&](sqlite3_stmt* st) { out.push_back(rowToPersona(st)); });
  return out;
}

bool activatePersona(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::string ts = nowIso();
  execute("UPDATE personas SET is_active=0");
  execute("UPDATE personas SET is_active=1, updated_at=? WHERE id=?",
          {ts, std::to_string(id)});
  return sqlite3_changes(g_db) > 0;
}

std::optional<Persona> getActivePersona() {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::optional<Persona> out;
  query("SELECT id,name,avatar,greeting,system_prompt,temperature,is_active,"
        "created_at,updated_at FROM personas WHERE is_active=1 LIMIT 1",
        {}, [&](sqlite3_stmt* st) { out = rowToPersona(st); });
  return out;
}

Bot createBot(const Bot& b) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::string ts = nowIso();
  execute("INSERT INTO bots(name,avatar,greeting,system_prompt,temperature,is_enabled,"
          "api_base_url,api_key_enc,api_model,created_at,updated_at) "
          "VALUES(?,?,?,?,?,?,?,?,?,?,?)",
          {b.name, b.avatar, b.greeting, b.system_prompt,
           std::to_string(b.temperature), b.is_enabled ? "1" : "0",
           b.api_base_url, b.api_key_enc, b.api_model, ts, ts});
  Bot out = b;
  out.id = sqlite3_last_insert_rowid(g_db);
  out.created_at = out.updated_at = ts;
  return out;
}

bool updateBot(const Bot& b) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  execute("UPDATE bots SET name=?,avatar=?,greeting=?,system_prompt=?,temperature=?,"
          "is_enabled=?,api_base_url=?,api_key_enc=?,api_model=?,updated_at=? WHERE id=?",
          {b.name, b.avatar, b.greeting, b.system_prompt,
           std::to_string(b.temperature), b.is_enabled ? "1" : "0",
           b.api_base_url, b.api_key_enc, b.api_model, nowIso(), std::to_string(b.id)});
  return sqlite3_changes(g_db) > 0;
}

bool deleteBot(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  auto def = getDefaultBot();   // recursive_mutex 可重入
  if (def && def->id == id) return false;
  execute("UPDATE sessions SET bot_id=0 WHERE bot_id=?", {std::to_string(id)});
  execute("DELETE FROM group_members WHERE bot_id=?", {std::to_string(id)});
  execute("DELETE FROM bots WHERE id=?", {std::to_string(id)});
  return true;
}

std::vector<Bot> listBots() {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::vector<Bot> out;
  query("SELECT id,name,avatar,greeting,system_prompt,temperature,is_enabled,"
        "api_base_url,api_key_enc,api_model,created_at,updated_at "
        "FROM bots ORDER BY id ASC",
        {}, [&](sqlite3_stmt* st) { out.push_back(rowToBot(st)); });
  return out;
}

std::optional<Bot> getBot(int64_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::optional<Bot> out;
  query("SELECT id,name,avatar,greeting,system_prompt,temperature,is_enabled,"
        "api_base_url,api_key_enc,api_model,created_at,updated_at "
        "FROM bots WHERE id=?",
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
        "api_base_url,api_key_enc,api_model,created_at,updated_at "
        "FROM bots ORDER BY id ASC LIMIT 1",
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

}  // namespace db
