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
  return {colInt(st, 0), colText(st, 1), colText(st, 2), colText(st, 3)};
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
    bool hasPersona = false;
    query("SELECT COUNT(*) FROM personas", {},
          [&](sqlite3_stmt* st) { hasPersona = colInt(st, 0) > 0; });
    if (!hasPersona) {
      Persona d;
      d.name = "默认助手";
      d.greeting = "你好呀，我是你的 AI 陪伴，有什么想聊的吗？";
      d.system_prompt = "你是一个温暖、耐心的 AI 陪伴助手，用自然友好的语气和用户聊天。";
      d.is_active = true;
      createPersona(d);
    }
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

Session createSession(const std::string& title) {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::string ts = nowIso();
  execute("INSERT INTO sessions(title, created_at, updated_at) VALUES(?,?,?)",
          {title, ts, ts});
  return {sqlite3_last_insert_rowid(g_db), title, ts, ts};
}

std::vector<Session> listSessions() {
  std::lock_guard<std::recursive_mutex> lock(g_mtx);
  std::vector<Session> out;
  query("SELECT id,title,created_at,updated_at FROM sessions ORDER BY updated_at DESC",
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
  query("SELECT id,title,created_at,updated_at FROM sessions WHERE id=?",
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
