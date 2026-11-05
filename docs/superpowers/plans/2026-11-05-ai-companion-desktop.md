# AI 陪伴桌面程序 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建 Windows 桌面 AI 陪伴程序：WPF 壳 + WebView2 渲染的 HTML 聊天界面 + C++ HTTP 后端（cpp-httplib + SQLite 内嵌）直连外部 OpenAI 兼容 API，支持多会话、多主题、流式对话。

**Architecture:** 前端 JS 通过 Fetch/SSE 调用本机 C++ 后端（127.0.0.1:8765），后端组装上下文后用 WinHTTP 流式转发外部 API；所有数据（会话/消息/人设/配置）存本地 SQLite，API Key 用 DPAPI 加密；WPF 负责拉起与守护后端进程，并用虚拟主机 app.local 托管 wwwroot。

**Tech Stack:** C++20（MSVC）+ cpp-httplib + sqlite3 amalgamation + nlohmann/json + doctest + WinHTTP；C# .NET 8 WPF + WebView2；原生 HTML/CSS/JS + marked + highlight.js + DOMPurify。

**Spec:** `docs/superpowers/specs/2026-11-05-ai-companion-desktop-design.md` — 本计划依据该规格编写，实现者需同时阅读两者。

## Global Constraints

- Windows 专属；C++20，MSVC 工具链，CMake ≥ 3.20；.NET 8 SDK。
- 后端监听 127.0.0.1:8765，端口冲突时 8766–8775 依次重试，实际端口写入 `data/config.json` 的 `server.actual_port`。
- 所有业务 API 前缀 `/api/v1/`；统一响应 `{"code":0,"data":...}` / `{"code":<非0>,"message":"..."}`；SSE 事件为 `event: delta|done|error` + 单行 JSON `data:`。
- 数据库路径 `data/app.db`；时间戳 ISO8601 UTC（`nowIso()`）；SQLite 开启 WAL + 外键。
- API Key 经 DPAPI（CryptProtectData，用户作用域）加密后 Base64 存 `settings(key='api_key_enc')`，GET /api/v1/config 只返回掩码 `前6 + "..." + 后4`。
- 外部 API 调用必须支持自定义 HTTP 代理（proxy_enabled/proxy_host/proxy_port；host 为空时读系统代理注册表）。
- 测试运行方式：`ctest --test-dir backend/build -C Release --output-on-failure`；后端集成验证用 `LLM_MOCK=1` 环境变量（内置假流式响应）。
- 每完成一个 Task 必须 git commit（提交信息用各 Task 末尾给出的格式）。
- 仓库根：`D:\One`。所有相对路径以此为基准。

---

## 文件结构总览

```
backend/CMakeLists.txt              # 构建 backend.exe + backend_tests.exe
backend/src/main.cpp                # 入口：参数解析、端口回退、路由注册
backend/src/logger.h/.cpp           # 分级文件日志（滚动 5 个，2MB）
backend/src/json_util.h             # json 别名、okJson/errJson/nowIso/utf8Truncate
backend/src/db.h/.cpp               # SQLite 封装（领域函数，不泄露 SQL）
backend/src/config.h/.cpp           # data/config.json 读写
backend/src/dpapi.h/.cpp            # DPAPI 加解密（Base64 往返）
backend/src/llm_provider.h          # ILlmProvider 抽象 + StreamEvent
backend/src/openai_provider.h/.cpp  # WinHTTP 流式实现 + SSE 解析器 + LLM_MOCK
backend/src/chat_service.h/.cpp     # 上下文组装（system prompt + N 轮截断）
backend/src/event_bus.h/.cpp        # 事件总线 + Webhook 插件分发
backend/src/server.h/.cpp           # 全部 REST/SSE 路由 + CORS + 静态文件
backend/tests/test_main.cpp         # doctest main
backend/tests/test_db.cpp           # DB CRUD / 级联 / 人设 / settings
backend/tests/test_config.cpp       # config 默认值 + 往返
backend/tests/test_dpapi.cpp        # DPAPI 加解密往返
backend/tests/test_openai_parser.cpp# SSE 解析器分片/多行/事件名/[DONE]
backend/tests/test_chat_service.cpp # 上下文组装 + N 轮截断 + utf8Truncate
backend/tests/test_event_bus.cpp    # 订阅/分发 + 载荷格式
backend/third_party/...             # httplib.h / json.hpp / doctest.h / sqlite3.c/.h
src/AiCompanion.App/...             # WPF 壳（Task 11）
scripts/build_backend.cmd           # CMake 构建 + ctest + 拷贝到 bin/backend
scripts/run_dev.cmd                 # 一键启动（Task 12）
scripts/test_api.ps1                # curl 集成测试（Task 7 创建，Task 8 扩展）
README.md                           # 运行步骤 / 依赖 / 打包建议（Task 12）
.gitignore
```

---

### Task 1: 项目脚手架 + 第三方依赖 + 后端骨架（/health 可访问）

**Files:**
- Create: `.gitignore`
- Create: `backend/CMakeLists.txt`
- Create: `backend/src/logger.h`, `backend/src/logger.cpp`
- Create: `backend/src/json_util.h`
- Create: `backend/src/main.cpp`
- Download: `backend/third_party/httplib.h`, `backend/third_party/json.hpp`, `backend/third_party/doctest/doctest.h`, `backend/third_party/sqlite/sqlite3.c`, `backend/third_party/sqlite/sqlite3.h`

**Interfaces:**
- Consumes: 无（首个任务）
- Produces:
  - `logx::init(const std::string& logDir) -> bool`、`logx::info/warn/error(const std::string&)`（`logger.h`）
  - `using json = nlohmann::json;`、`okJson(data)`、`errJson(code,msg)`、`nowIso()`、`utf8Truncate(s,maxChars)`（`json_util.h`）
  - `backend.exe --data <dir>` 启动参数约定；`GET /health` → `{"status":"ok","version":"1.0.0"}`
  - 目录约定：日志在 `<data>/logs/backend.log`

- [ ] **Step 1: 创建 .gitignore**

```gitignore
# 构建产物
bin/
obj/
.vs/
backend/build/
# 运行时数据
data/
# 用户文件
*.user
*.suo
```

- [ ] **Step 2: 下载第三方依赖（4 个文件）**

```bash
mkdir -p backend/third_party/sqlite backend/third_party/doctest
curl.exe -L -o backend/third_party/httplib.h https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.3/httplib.h
curl.exe -L -o backend/third_party/json.hpp https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
curl.exe -L -o backend/third_party/doctest/doctest.h https://raw.githubusercontent.com/doctest/doctest/v2.4.11/doctest/doctest.h
curl.exe -L -o /tmp/sqlite.zip https://sqlite.org/2024/sqlite-amalgamation-3460100.zip
cd /tmp && unzip -o sqlite.zip && cp sqlite-amalgamation-3460100/sqlite3.c sqlite-amalgamation-3460100/sqlite3.h /d/One/backend/third_party/sqlite/
```

验证：`ls -la backend/third_party/ backend/third_party/sqlite/` 五个文件都存在且非空（httplib.h 约 10000+ 行，sqlite3.c 约 240000 行）。

- [ ] **Step 3: 写 logger.h**

```cpp
#pragma once
#include <string>

namespace logx {
bool init(const std::string& logDir);
void info(const std::string& msg);
void warn(const std::string& msg);
void error(const std::string& msg);
}
```

- [ ] **Step 4: 写 logger.cpp**

```cpp
#include "logger.h"
#include <windows.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace logx {
namespace {
std::ofstream g_file;
std::mutex g_mtx;
constexpr int kMaxFiles = 5;
constexpr long long kMaxSize = 2LL * 1024 * 1024;

std::string stamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

void rotate(const std::filesystem::path& base) {
  for (int i = kMaxFiles - 1; i >= 1; --i) {
    auto from = base.string() + (i == 1 ? "" : "." + std::to_string(i - 1));
    auto to = base.string() + "." + std::to_string(i);
    std::error_code ec;
    std::filesystem::remove(to, ec);
    std::filesystem::rename(from, to, ec);
  }
}

void write(const std::string& level, const std::string& msg) {
  std::lock_guard<std::mutex> lock(g_mtx);
  std::string line = "[" + stamp() + "] [" + level + "] " + msg + "\n";
  if (g_file.is_open()) g_file << line << std::flush;
  OutputDebugStringA(line.c_str());
}
}  // namespace

bool init(const std::string& logDir) {
  std::lock_guard<std::mutex> lock(g_mtx);
  try {
    std::filesystem::create_directories(logDir);
    auto base = std::filesystem::path(logDir) / "backend.log";
    if (std::filesystem::exists(base) &&
        std::filesystem::file_size(base) > kMaxSize) {
      g_file.close();
      rotate(base);
    }
    g_file.open(base, std::ios::app);
    return g_file.is_open();
  } catch (...) {
    return false;
  }
}

void info(const std::string& msg) { write("INFO", msg); }
void warn(const std::string& msg) { write("WARN", msg); }
void error(const std::string& msg) { write("ERROR", msg); }
}  // namespace logx
```

- [ ] **Step 5: 写 json_util.h**

```cpp
#pragma once
#include <chrono>
#include <ctime>
#include <string>
#include "json.hpp"

using json = nlohmann::json;

inline json okJson(const json& data) { return json{{"code", 0}, {"data", data}}; }
inline json errJson(int code, const std::string& message) {
  return json{{"code", code}, {"message", message}};
}
inline std::string nowIso() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_s(&tm, &t);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}
// 按 Unicode 码点安全截断（中文 3 字节不会截断出半个字符）
inline std::string utf8Truncate(const std::string& s, size_t maxChars) {
  size_t n = 0, i = 0;
  while (i < s.size() && n < maxChars) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    i += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
    ++n;
  }
  return s.substr(0, i);
}
```

- [ ] **Step 6: 写 main.cpp（骨架：参数解析 + /health + 端口回退）**

```cpp
#include <httplib.h>
#include <string>

#include "json_util.h"
#include "logger.h"

int main(int argc, char** argv) {
  std::string dataDir = "data";
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) == "--data") dataDir = argv[i + 1];
  }
  logx::init(dataDir + "/logs");
  logx::info("backend starting, data dir: " + dataDir);

  httplib::Server svr;
  svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"status\":\"ok\",\"version\":\"1.0.0\"}", "application/json");
  });

  for (int port = 8765; port <= 8775; ++port) {
    logx::info("trying to listen on 127.0.0.1:" + std::to_string(port));
    if (svr.listen("127.0.0.1", port)) {
      logx::info("listening on port " + std::to_string(port));
      return 0;
    }
  }
  logx::error("no available port");
  return 1;
}
```

- [ ] **Step 7: 写 CMakeLists.txt（Task 1 版本，后续任务追加文件时同步修改）**

```cmake
cmake_minimum_required(VERSION 3.20)
project(ai_companion_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)

add_executable(backend
  src/main.cpp
  src/logger.cpp
)
target_include_directories(backend PRIVATE src third_party)
target_link_libraries(backend PRIVATE sqlite3 winhttp crypt32 ws2_32)
```

- [ ] **Step 8: 构建并验证 /health**

```bash
cmake -S backend -B backend/build -A x64
cmake --build backend/build --config Release
./backend/build/Release/backend.exe --data data &
sleep 1
curl.exe -s http://127.0.0.1:8765/health
# 期望输出: {"status":"ok","version":"1.0.0"}
kill %1
```

- [ ] **Step 9: Commit**

```bash
git add .gitignore backend/
git commit -m "chore: 后端脚手架、第三方依赖与 /health 骨架"
```

---

### Task 2: SQLite 数据层（建表 + 领域 CRUD + 播种默认人设）

**Files:**
- Create: `backend/src/db.h`, `backend/src/db.cpp`
- Create: `backend/tests/test_main.cpp`, `backend/tests/test_db.cpp`
- Modify: `backend/CMakeLists.txt`（加入 db.cpp 与 backend_tests 目标）

**Interfaces:**
- Consumes: `json_util.h` 的 `nowIso()`；`logger.h` 的 `logx::error`
- Produces（后续所有任务依赖，签名不得更改）:
  - `db::Session{int64_t id; std::string title; std::string created_at; std::string updated_at;}`
  - `db::Message{int64_t id; int64_t session_id; std::string role; std::string content; std::string created_at;}`
  - `db::Persona{int64_t id; std::string name; std::string avatar; std::string greeting; std::string system_prompt; double temperature; bool is_active; std::string created_at; std::string updated_at;}`
  - `db::open(path)->bool`（幂等，自动建表+播种默认人设）、`db::close()`
  - `db::createSession(title="新对话")`、`db::listSessions()`（updated_at 倒序）、`db::renameSession(id,title)`、`db::deleteSession(id)`（级联）、`db::touchSession(id)`、`db::getSession(id)->optional<Session>`
  - `db::appendMessage(sessionId, role, content)`、`db::listMessages(sessionId, limit=1000)`（**id 升序**，API 层自行倒序）、`db::deleteMessage(id)`、`db::clearMessages(sessionId)`、`db::lastMessageByRole(sessionId, role)->optional<Message>`
  - `db::createPersona(p)`、`db::updatePersona(p)->bool`、`db::deletePersona(id)->bool`（激活中返回 false）、`db::listPersonas()`、`db::activatePersona(id)->bool`、`db::getActivePersona()->optional<Persona>`
  - `db::getSetting(key)->optional<string>`、`db::setSetting(key,value)`

- [ ] **Step 1: 写 db.h（完整接口声明，内容即上方 Interfaces 块的实现）**

```cpp
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace db {

struct Session {
  int64_t id = 0;
  std::string title;
  std::string created_at;
  std::string updated_at;
};
struct Message {
  int64_t id = 0;
  int64_t session_id = 0;
  std::string role;      // "user" | "assistant" | "system"
  std::string content;
  std::string created_at;
};
struct Persona {
  int64_t id = 0;
  std::string name;
  std::string avatar = "🌸";
  std::string greeting;
  std::string system_prompt;
  double temperature = 0.8;
  bool is_active = false;
  std::string created_at;
  std::string updated_at;
};

bool open(const std::string& dbPath);
void close();

Session createSession(const std::string& title = "新对话");
std::vector<Session> listSessions();
void renameSession(int64_t id, const std::string& title);
void deleteSession(int64_t id);
void touchSession(int64_t id);
std::optional<Session> getSession(int64_t id);

Message appendMessage(int64_t sessionId, const std::string& role,
                      const std::string& content);
std::vector<Message> listMessages(int64_t sessionId, int limit = 1000);
void deleteMessage(int64_t id);
void clearMessages(int64_t sessionId);
std::optional<Message> lastMessageByRole(int64_t sessionId,
                                         const std::string& role);

Persona createPersona(const Persona& p);
bool updatePersona(const Persona& p);
bool deletePersona(int64_t id);
std::vector<Persona> listPersonas();
bool activatePersona(int64_t id);
std::optional<Persona> getActivePersona();

std::optional<std::string> getSetting(const std::string& key);
void setSetting(const std::string& key, const std::string& value);

}  // namespace db
```

- [ ] **Step 2: 写 db.cpp（单连接 + recursive_mutex；prepare/bind/step 封装 + 建表 + 播种）**

```cpp
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
```

- [ ] **Step 3: 写测试入口 tests/test_main.cpp**

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

- [ ] **Step 4: 写 tests/test_db.cpp**

```cpp
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

TEST_CASE("db: 打开即建表并播种默认人设") {
  auto path = tmpDb("ac_test_seed.db");
  REQUIRE(db::open(path.string()));
  auto personas = db::listPersonas();
  REQUIRE(personas.size() == 1);
  REQUIRE(personas[0].is_active);
  CHECK(personas[0].name == "默认助手");
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

  Persona p;
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
  CHECK(db::listPersonas().size() == 2);
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
```

- [ ] **Step 5: 更新 CMakeLists.txt（加入 db.cpp 与测试目标）**

在 `add_executable(backend ...)` 的源文件列表中加入 `src/db.cpp`，文件末尾追加：

```cmake
enable_testing()

add_executable(backend_tests
  tests/test_main.cpp
  tests/test_db.cpp
  src/db.cpp
  src/logger.cpp
)
target_include_directories(backend_tests PRIVATE src third_party)
target_link_libraries(backend_tests PRIVATE sqlite3 winhttp crypt32 ws2_32)
add_test(NAME backend_tests COMMAND backend_tests)
```

- [ ] **Step 6: 构建并运行测试**

```bash
cmake -S backend -B backend/build -A x64
cmake --build backend/build --config Release
ctest --test-dir backend/build -C Release --output-on-failure
# 期望: 4 个 TEST_CASE 全部 PASS
```

- [ ] **Step 7: Commit**

```bash
git add backend/
git commit -m "feat: SQLite 数据层（会话/消息/人设/settings CRUD）与单测"
```

### Task 3: config.json 读写 + DPAPI 加解密

**Files:**
- Create: `backend/src/config.h`, `backend/src/config.cpp`
- Create: `backend/src/dpapi.h`, `backend/src/dpapi.cpp`
- Create: `backend/tests/test_config.cpp`, `backend/tests/test_dpapi.cpp`
- Modify: `backend/CMakeLists.txt`（加入 config.cpp、dpapi.cpp 及两个测试文件）

**Interfaces:**
- Consumes: `json_util.h`
- Produces:
  - `cfg::Plugin{name,url,events}`、`cfg::ServerConfig{host="127.0.0.1", port=8765, actual_port=0, keep_rounds=20, theme="dark", temperature=0.8, static_dir="", plugins=[]}`
  - `cfg::load(path)->ServerConfig`（文件缺失/损坏返回默认值并记 warn 日志）、`cfg::save(path, cfg)`（自动创建父目录）
  - `dpapi::protect(plain)->string`（Base64）、`dpapi::unprotect(b64)->string`（失败抛 `std::runtime_error`）

- [ ] **Step 1: 写 config.h**

```cpp
#pragma once
#include <string>
#include <vector>

namespace cfg {
struct Plugin {
  std::string name;
  std::string url;
  std::vector<std::string> events;
};
struct ServerConfig {
  std::string host = "127.0.0.1";
  int port = 8765;
  int actual_port = 0;    // 端口回退后记录实际端口，0 表示未回退
  int keep_rounds = 20;
  std::string theme = "dark";
  double temperature = 0.8;
  std::string static_dir; // 空 = 不提供静态文件（WPF 用虚拟主机，无需此项）
  std::vector<Plugin> plugins;
};
ServerConfig load(const std::string& path);
void save(const std::string& path, const ServerConfig& c);
}  // namespace cfg
```

- [ ] **Step 2: 写 config.cpp**

```cpp
#include "config.h"

#include <filesystem>
#include <fstream>

#include "json_util.h"
#include "logger.h"

namespace cfg {

ServerConfig load(const std::string& path) {
  ServerConfig c;
  std::ifstream f(path);
  if (!f.is_open()) {
    logx::warn("config not found, using defaults: " + path);
    return c;
  }
  try {
    json j = json::parse(f);
    if (j.contains("server")) {
      auto& s = j["server"];
      c.host = s.value("host", c.host);
      c.port = s.value("port", c.port);
      c.actual_port = s.value("actual_port", c.actual_port);
      c.static_dir = s.value("static_dir", c.static_dir);
    }
    if (j.contains("defaults")) {
      auto& d = j["defaults"];
      c.keep_rounds = d.value("keep_rounds", c.keep_rounds);
      c.theme = d.value("theme", c.theme);
      c.temperature = d.value("temperature", c.temperature);
    }
    if (j.contains("plugins") && j["plugins"].is_array()) {
      for (auto& p : j["plugins"]) {
        Plugin pl;
        pl.name = p.value("name", "");
        pl.url = p.value("url", "");
        if (p.contains("events") && p["events"].is_array())
          for (auto& e : p["events"]) pl.events.push_back(e.get<std::string>());
        c.plugins.push_back(std::move(pl));
      }
    }
  } catch (const std::exception& e) {
    logx::warn(std::string("config parse failed, using defaults: ") + e.what());
    return ServerConfig{};
  }
  return c;
}

void save(const std::string& path, const ServerConfig& c) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  json j = {
      {"server",
       {{"host", c.host},
        {"port", c.port},
        {"actual_port", c.actual_port},
        {"static_dir", c.static_dir}}},
      {"defaults",
       {{"keep_rounds", c.keep_rounds},
        {"theme", c.theme},
        {"temperature", c.temperature}}},
      {"plugins", json::array()}};
  for (auto& p : c.plugins)
    j["plugins"].push_back({{"name", p.name}, {"url", p.url}, {"events", p.events}});
  std::ofstream f(path);
  f << j.dump(2);
}

}  // namespace cfg
```

- [ ] **Step 3: 写 dpapi.h**

```cpp
#pragma once
#include <string>

namespace dpapi {
std::string protect(const std::string& plain);   // 返回 Base64
std::string unprotect(const std::string& b64);   // 失败抛 std::runtime_error
}  // namespace dpapi
```

- [ ] **Step 4: 写 dpapi.cpp**

```cpp
#include "dpapi.h"

#include <windows.h>
#include <dpapi.h>

#include <stdexcept>
#include <vector>

namespace dpapi {
namespace {

const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string toB64(const std::vector<unsigned char>& in) {
  std::string out;
  size_t i = 0;
  while (i + 3 <= in.size()) {
    unsigned n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += kB64[(n >> 6) & 63];
    out += kB64[n & 63];
    i += 3;
  }
  size_t rem = in.size() - i;
  if (rem == 1) {
    unsigned n = in[i] << 16;
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += "==";
  } else if (rem == 2) {
    unsigned n = (in[i] << 16) | (in[i + 1] << 8);
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += kB64[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

int val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::vector<unsigned char> fromB64(const std::string& in) {
  std::vector<unsigned char> out;
  unsigned acc = 0;
  int bits = 0;
  for (char ch : in) {
    if (ch == '=') break;
    int v = val(ch);
    if (v < 0) continue;
    acc = (acc << 6) | (unsigned)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((acc >> bits) & 0xFF);
    }
  }
  return out;
}

std::vector<unsigned char> crypt(const std::vector<unsigned char>& in,
                                 bool protectMode) {
  DATA_BLOB src{static_cast<DWORD>(in.size()),
                const_cast<BYTE*>(in.data())};
  DATA_BLOB dst{0, nullptr};
  BOOL ok = protectMode
                ? CryptProtectData(&src, L"AICompanion", nullptr, nullptr,
                                   nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dst)
                : CryptUnprotectData(&src, nullptr, nullptr, nullptr, nullptr,
                                     CRYPTPROTECT_UI_FORBIDDEN, &dst);
  if (!ok) throw std::runtime_error(protectMode ? "CryptProtectData failed"
                                                : "CryptUnprotectData failed");
  std::vector<unsigned char> out(dst.pbData, dst.pbData + dst.cbData);
  LocalFree(dst.pbData);
  return out;
}

}  // namespace

std::string protect(const std::string& plain) {
  if (plain.empty()) return "";
  std::vector<unsigned char> in(plain.begin(), plain.end());
  return toB64(crypt(in, true));
}

std::string unprotect(const std::string& b64) {
  auto raw = crypt(fromB64(b64), false);
  return std::string(raw.begin(), raw.end());
}

}  // namespace dpapi
```

- [ ] **Step 5: 写 tests/test_config.cpp**

```cpp
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>

#include "config.h"

TEST_CASE("config: 缺失文件返回默认值") {
  auto path = (std::filesystem::temp_directory_path() / "ac_no_such_config.json").string();
  std::filesystem::remove(path);
  auto c = cfg::load(path);
  CHECK(c.host == "127.0.0.1");
  CHECK(c.port == 8765);
  CHECK(c.actual_port == 0);
  CHECK(c.keep_rounds == 20);
  CHECK(c.theme == "dark");
  CHECK(c.plugins.empty());
}

TEST_CASE("config: 保存后读取往返一致") {
  auto path = (std::filesystem::temp_directory_path() / "ac_test_config.json").string();
  cfg::ServerConfig c;
  c.port = 9000;
  c.actual_port = 9001;
  c.keep_rounds = 5;
  c.theme = "sakura";
  cfg::Plugin p;
  p.name = "demo";
  p.url = "http://127.0.0.1:9000/hook";
  p.events = {"chat.completed"};
  c.plugins.push_back(p);
  cfg::save(path, c);

  auto got = cfg::load(path);
  CHECK(got.port == 9000);
  CHECK(got.actual_port == 9001);
  CHECK(got.keep_rounds == 5);
  CHECK(got.theme == "sakura");
  REQUIRE(got.plugins.size() == 1);
  CHECK(got.plugins[0].name == "demo");
  CHECK(got.plugins[0].url == "http://127.0.0.1:9000/hook");
  REQUIRE(got.plugins[0].events.size() == 1);
  CHECK(got.plugins[0].events[0] == "chat.completed");
  std::filesystem::remove(path);
}

TEST_CASE("config: 损坏 JSON 回退默认值") {
  auto path = (std::filesystem::temp_directory_path() / "ac_bad_config.json").string();
  std::ofstream f(path);
  f << "{not valid json";
  f.close();
  auto c = cfg::load(path);
  CHECK(c.port == 8765);
  std::filesystem::remove(path);
}
```

- [ ] **Step 6: 写 tests/test_dpapi.cpp**

```cpp
#include <doctest/doctest.h>
#include <string>

#include "dpapi.h"

TEST_CASE("dpapi: 加解密往返") {
  std::string key = "sk-test-abcdef1234567890-中文也支持";
  auto enc = dpapi::protect(key);
  REQUIRE(!enc.empty());
  CHECK(enc != key);              // 密文不等于明文
  CHECK(dpapi::unprotect(enc) == key);
}

TEST_CASE("dpapi: 空串不加密") {
  CHECK(dpapi::protect("").empty());
}

TEST_CASE("dpapi: 非法密文抛异常") {
  CHECK_THROWS_AS(dpapi::unprotect("!!!!not-base64"), std::runtime_error);
}
```

- [ ] **Step 7: 更新 CMakeLists.txt**

`backend` 与 `backend_tests` 的源文件列表分别加入 `src/config.cpp`、`src/dpapi.cpp`；`backend_tests` 再加入 `tests/test_config.cpp`、`tests/test_dpapi.cpp`。

- [ ] **Step 8: 构建并运行测试**

```bash
cmake -S backend -B backend/build -A x64
cmake --build backend/build --config Release
ctest --test-dir backend/build -C Release --output-on-failure
# 期望: 全部 PASS（dpapi 测试在真实 Windows 用户环境下通过）
```

- [ ] **Step 9: Commit**

```bash
git add backend/
git commit -m "feat: config.json 读写与 DPAPI 加解密封装"
```

---

### Task 4: LLM Provider 抽象 + OpenAI 兼容流式客户端（WinHTTP）+ 内置 Mock

**Files:**
- Create: `backend/src/llm_provider.h`
- Create: `backend/src/openai_provider.h`, `backend/src/openai_provider.cpp`
- Create: `backend/tests/test_openai_parser.cpp`
- Modify: `backend/CMakeLists.txt`

**Interfaces:**
- Consumes: `json_util.h`、`logger.h`
- Produces:
  - `llm::ChatMsg{role,content}`、`llm::ChatRequest{messages, temperature, base_url, api_key, model, proxy_enabled, proxy_host, proxy_port}`
  - `llm::StreamEvent{type, content, error_code, message}`（type 仅 "delta" 与 "error"；"done" 由路由层发出）
  - `llm::ILlmProvider::streamChat(const ChatRequest&, StreamCallback, const std::atomic_bool& stop)`
  - `llm::OpenAiCompatProvider`（实现上述接口；`LLM_MOCK=1` 环境变量存在时走内置假流）
  - `llm::SseParser`（`feed(chunk)` 增量解析，`finish()` 收尾；`onEvent(event, data)` 回调）

- [ ] **Step 1: 写 llm_provider.h（接口契约，后续本地推理 provider 实现同一接口）**

```cpp
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace llm {

struct ChatMsg {
  std::string role;     // "system" | "user" | "assistant"
  std::string content;
};
struct ChatRequest {
  std::vector<ChatMsg> messages;
  double temperature = 0.8;
  std::string base_url;    // 如 https://api.deepseek.com/v1
  std::string api_key;
  std::string model;
  bool proxy_enabled = false;
  std::string proxy_host;  // 为空 = 使用系统代理（注册表）
  int proxy_port = 0;
};
struct StreamEvent {
  std::string type;        // "delta" | "error"
  std::string content;
  int error_code = 0;
  std::string message;
};
using StreamCallback = std::function<void(const StreamEvent&)>;

class ILlmProvider {
 public:
  virtual ~ILlmProvider() = default;
  // 同步执行；期间持续回调 delta；出错回调 error；stop 置位后尽快返回
  virtual void streamChat(const ChatRequest& req, StreamCallback cb,
                          const std::atomic_bool& stop) = 0;
};

}  // namespace llm
```

- [ ] **Step 2: 写 openai_provider.h**

```cpp
#pragma once
#include <functional>
#include <string>

#include "llm_provider.h"

namespace llm {

// OpenAI 兼容 SSE 流解析器：行缓冲 + 前缀剥离，兼容各厂商差异
// （data 换行位置不定、[DONE] 位置不定、usage 等额外事件宽容跳过）
class SseParser {
 public:
  std::function<void(const std::string& event, const std::string& data)> onEvent;
  void feed(const std::string& chunk);
  void finish();   // 收尾：flush 未派发的最后事件

 private:
  std::string buf_;
  std::string eventName_ = "message";
  std::string data_;
  bool hasData_ = false;
};

class OpenAiCompatProvider : public ILlmProvider {
 public:
  void streamChat(const ChatRequest& req, StreamCallback cb,
                  const std::atomic_bool& stop) override;
};

}  // namespace llm
```

- [ ] **Step 3: 写 openai_provider.cpp**

```cpp
#include "openai_provider.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "json_util.h"
#include "logger.h"

namespace llm {
namespace {

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

struct Url {
  bool https = true;
  std::string host;
  int port = 443;
  std::string path;   // 不含 /chat/completions 的部分，以 / 开头
};

// 解析 base_url（如 https://api.deepseek.com/v1），无 scheme 时默认 https
Url parseUrl(const std::string& base) {
  Url u;
  std::string rest = trim(base);
  if (rest.rfind("http://", 0) == 0) {
    u.https = false;
    u.port = 80;
    rest = rest.substr(7);
  } else if (rest.rfind("https://", 0) == 0) {
    rest = rest.substr(8);
  }
  auto slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  u.path = slash == std::string::npos ? "" : rest.substr(slash);
  while (!u.path.empty() && u.path.back() == '/') u.path.pop_back();
  auto colon = authority.find(':');
  if (colon != std::string::npos) {
    u.host = authority.substr(0, colon);
    try {
      u.port = std::stoi(authority.substr(colon + 1));
    } catch (...) {
      u.port = u.https ? 443 : 80;
    }
  } else {
    u.host = authority;
  }
  return u;
}

// 自定义代理优先；host 为空时读系统代理注册表（HKCU Internet Settings）
std::string resolveProxy(const ChatRequest& req) {
  if (!req.proxy_enabled) return "";
  if (!req.proxy_host.empty())
    return req.proxy_host + ":" + std::to_string(req.proxy_port);
  std::string v;
  HKEY hk;
  if (RegOpenKeyExA(HKEY_CURRENT_USER,
                    "Software\\Microsoft\\Windows\\CurrentVersion\\Internet "
                    "Settings",
                    0, KEY_READ, &hk) == ERROR_SUCCESS) {
    char buf[512];
    DWORD len = sizeof(buf), type = 0;
    if (RegQueryValueExA(hk, "ProxyServer", nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf), &len) == ERROR_SUCCESS &&
        type == REG_SZ)
      v = buf;
    RegCloseKey(hk);
  }
  if (v.empty()) return "";
  auto pick = [&](const std::string& proto) -> std::string {
    auto p = v.find(proto + "=");
    if (p == std::string::npos) return "";
    auto end = v.find(';', p);
    return v.substr(p + proto.size() + 1,
                    end == std::string::npos ? std::string::npos
                                             : end - p - proto.size() - 1);
  };
  std::string r = pick("https");
  if (r.empty()) r = pick("http");
  if (r.empty()) r = v;
  return r;
}

std::string buildBody(const ChatRequest& req) {
  json body = {{"model", req.model}, {"stream", true}, {"messages", json::array()}};
  for (auto& m : req.messages)
    body["messages"].push_back({{"role", m.role}, {"content", m.content}});
  if (req.temperature > 0) body["temperature"] = req.temperature;
  return body.dump();
}

void emitDelta(StreamCallback cb, const json& j) {
  if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty())
    return;
  auto& ch = j["choices"][0];
  if (!ch.contains("delta") || !ch["delta"].is_object()) return;
  auto& d = ch["delta"];
  if (d.contains("content") && d["content"].is_string()) {
    std::string t = d["content"].get<std::string>();
    if (!t.empty()) cb({"delta", t, 0, ""});
  }
}

void mockStream(StreamCallback cb, const std::atomic_bool& stop) {
  const std::vector<std::string> pieces = {
      "你好呀！", "我是内置 Mock 回复，", "当前 LLM_MOCK=1，", "用于前端联调。",
      "\n\n支持 **Markdown** 渲染哦：\n\n```js\nconsole.log('hello');\n```"};
  for (auto& p : pieces) {
    if (stop) return;
    cb({"delta", p, 0, ""});
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  }
}

}  // namespace

void SseParser::feed(const std::string& chunk) {
  buf_ += chunk;
  size_t pos;
  while ((pos = buf_.find('\n')) != std::string::npos) {
    std::string line = buf_.substr(0, pos);
    buf_.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) {                       // 事件边界
      if (hasData_ && onEvent) onEvent(eventName_, data_);
      eventName_ = "message";
      data_.clear();
      hasData_ = false;
      continue;
    }
    if (line[0] == ':') continue;             // 注释行
    if (line.rfind("event:", 0) == 0) {
      eventName_ = trim(line.substr(6));
    } else if (line.rfind("data:", 0) == 0) {
      if (hasData_) data_ += "\n";
      data_ += trim(line.substr(5));
      hasData_ = true;
    }
    // id:/retry: 等其他字段忽略
  }
}

void SseParser::finish() {
  if (hasData_ && onEvent) onEvent(eventName_, data_);
  hasData_ = false;
}

void OpenAiCompatProvider::streamChat(const ChatRequest& req, StreamCallback cb,
                                      const std::atomic_bool& stop) {
  if (std::getenv("LLM_MOCK")) {
    mockStream(cb, stop);
    return;
  }
  if (req.base_url.empty() || req.api_key.empty() || req.model.empty()) {
    cb({"error", "", 400, "请先在设置中填写 API Base URL、API Key 与模型名称"});
    return;
  }

  Url url = parseUrl(req.base_url);
  std::string fullPath = url.path + "/chat/completions";
  std::string auth = "Authorization: Bearer " + req.api_key + "\r\n";

  HINTERNET hSession = WinHttpOpen(L"AICompanion/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    cb({"error", "", 500, "WinHttpOpen 失败"});
    return;
  }
  WinHttpSetTimeouts(hSession, 10000, 10000, 30000, 60000);

  HINTERNET hConn = WinHttpConnect(hSession, widen(url.host).c_str(), url.port, 0);
  HINTERNET hReq = hConn ? WinHttpOpenRequest(hConn, L"POST", widen(fullPath).c_str(),
                                              nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              url.https ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
  if (!hConn || !hReq) {
    cb({"error", "", 500, "无法建立连接（WinHttpConnect/OpenRequest 失败）"});
    if (hReq) WinHttpCloseHandle(hReq);
    if (hConn) WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);
    return;
  }

  std::string proxy = resolveProxy(req);
  if (!proxy.empty()) {
    std::wstring wProxy = widen(proxy);
    WINHTTP_PROXY_INFO pi;
    pi.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
    pi.lpszProxy = const_cast<wchar_t*>(wProxy.c_str());
    pi.lpszProxyBypass = L"<local>";
    WinHttpSetOption(hReq, WINHTTP_OPTION_PROXY, &pi, sizeof(pi));
  }

  std::wstring hdrs =
      L"Content-Type: application/json\r\n" + widen(auth) + L"Accept: text/event-stream\r\n";
  std::string body = buildBody(req);
  BOOL sent = WinHttpSendRequest(hReq, hdrs.c_str(), static_cast<DWORD>(hdrs.size()),
                                 const_cast<char*>(body.data()),
                                 static_cast<DWORD>(body.size()),
                                 static_cast<DWORD>(body.size()), 0);
  if (!sent) {
    DWORD err = GetLastError();
    cb({"error", "", 500, "无法连接 API 服务器（错误码 " + std::to_string(err) + "）"});
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);
    return;
  }
  if (!WinHttpReceiveResponse(hReq, nullptr)) {
    cb({"error", "", 500, "等待上游响应失败"});
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);
    return;
  }

  DWORD status = 0, sz = sizeof(status);
  WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                     WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                     WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    std::string errBody;
    char buf[2048];
    DWORD read = 0;
    while (WinHttpQueryDataAvailable(hReq, &sz) && sz > 0 &&
           WinHttpReadData(hReq, buf, (std::min)(sz, (DWORD)sizeof(buf)), &read) &&
           read > 0)
      errBody.append(buf, read);
    std::string msg;
    int code = 500;
    if (status == 401 || status == 403) {
      code = 401;
      msg = "API Key 无效或没有权限";
    } else if (status == 404) {
      code = 404;
      msg = "接口不存在，请检查 Base URL 是否包含 /v1";
    } else if (status == 429) {
      code = 429;
      msg = "请求过于频繁，请稍后再试";
    } else {
      msg = "上游服务错误（HTTP " + std::to_string(status) + "）";
    }
    if (!errBody.empty()) {
      try {
        json j = json::parse(errBody);
        if (j.contains("error") && j["error"].is_object() &&
            j["error"].contains("message"))
          msg = j["error"]["message"].get<std::string>();
      } catch (...) {
      }
    }
    cb({"error", "", code, msg});
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);
    return;
  }

  SseParser parser;
  parser.onEvent = [&](const std::string& /*ev*/, const std::string& data) {
    if (data == "[DONE]") return;
    try {
      emitDelta(cb, json::parse(data));
    } catch (...) {
      // 忽略无法解析的事件（如 usage 统计、非标准扩展）
    }
  };

  char buf[8192];
  DWORD read = 0;
  for (;;) {
    if (stop) {
      logx::info("upstream stream aborted by stop flag");
      break;
    }
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
    DWORD want = (std::min)(avail, (DWORD)sizeof(buf));
    if (!WinHttpReadData(hReq, buf, want, &read) || read == 0) break;
    parser.feed(std::string(buf, read));
  }
  parser.finish();

  WinHttpCloseHandle(hReq);
  WinHttpCloseHandle(hConn);
  WinHttpCloseHandle(hSession);
  logx::info("upstream stream ended");
}

}  // namespace llm
```

- [ ] **Step 4: 写 tests/test_openai_parser.cpp**

```cpp
#include <doctest/doctest.h>
#include <string>
#include <vector>

#include "openai_provider.h"

TEST_CASE("sse: 标准 OpenAI 流（事件边界 + [DONE]）") {
  llm::SseParser p;
  std::vector<std::pair<std::string, std::string>> got;
  p.onEvent = [&](const std::string& e, const std::string& d) {
    got.push_back({e, d});
  };
  p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"你\"}}]}\n\n");
  p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"好\"}}]}\n\n");
  p.feed("data: [DONE]\n\n");
  p.finish();
  REQUIRE(got.size() == 3);
  CHECK(got[0].first == "message");
  CHECK(got[0].second.find("你") != std::string::npos);
  CHECK(got[2].second == "[DONE]");
}

TEST_CASE("sse: 跨块分片的行被正确拼回") {
  llm::SseParser p;
  std::vector<std::string> datas;
  p.onEvent = [&](const std::string&, const std::string& d) { datas.push_back(d); };
  p.feed("data: {\"cho");                     // 半行
  p.feed("ices\":[{\"delta\":{\"content\":\"喵\"}}]}\n\n");
  p.finish();
  REQUIRE(datas.size() == 1);
  CHECK(datas[0].find("喵") != std::string::npos);
}

TEST_CASE("sse: 多行 data 拼接 + 自定义事件名 + 注释行") {
  llm::SseParser p;
  std::vector<std::pair<std::string, std::string>> got;
  p.onEvent = [&](const std::string& e, const std::string& d) {
    got.push_back({e, d});
  };
  p.feed(": keep-alive comment\n");
  p.feed("event: usage\ndata: {\"a\":1}\ndata: {\"b\":2}\n\n");
  p.finish();
  REQUIRE(got.size() == 1);
  CHECK(got[0].first == "usage");
  CHECK(got[0].second == "{\"a\":1}\n{\"b\":2}");
}

TEST_CASE("sse: \\r\\n 行尾与 event 字段混排") {
  llm::SseParser p;
  std::vector<std::pair<std::string, std::string>> got;
  p.onEvent = [&](const std::string& e, const std::string& d) {
    got.push_back({e, d});
  };
  p.feed("event: delta\r\ndata: {\"x\":\"y\"}\r\n\r\n");
  p.finish();
  REQUIRE(got.size() == 1);
  CHECK(got[0].first == "delta");
  CHECK(got[0].second == "{\"x\":\"y\"}");
}
```

- [ ] **Step 5: 更新 CMakeLists.txt**

`backend` 与 `backend_tests` 加入 `src/openai_provider.cpp`；`backend_tests` 再加入 `tests/test_openai_parser.cpp`。

- [ ] **Step 6: 构建并运行测试**

```bash
cmake -S backend -B backend/build -A x64
cmake --build backend/build --config Release
ctest --test-dir backend/build -C Release --output-on-failure
# 期望: 全部 PASS
```

- [ ] **Step 7: Commit**

```bash
git add backend/
git commit -m "feat: LLM Provider 抽象与 OpenAI 兼容流式客户端（WinHTTP + Mock）"
```

---

### Task 5: ChatService 上下文组装（system prompt + N 轮截断）

**Files:**
- Create: `backend/src/chat_service.h`, `backend/src/chat_service.cpp`
- Create: `backend/tests/test_chat_service.cpp`
- Modify: `backend/CMakeLists.txt`

**Interfaces:**
- Consumes: `db::Persona`、`db::Message`、`llm::ChatMsg`、`json_util.h` 的 `utf8Truncate`
- Produces:
  - `chat::buildContext(const db::Persona&, const std::vector<db::Message>& history, int keep_rounds, const std::string& newUser) -> std::vector<llm::ChatMsg>`
  - 规则：结果以 `{system, persona.system_prompt}` 开头；`keep_rounds > 0` 时从历史中倒数第 `keep_rounds` 条 user 消息开始截取（system 角色消息不参与轮数计数也不进入结果）；结尾追加 `{user, newUser}`

- [ ] **Step 1: 写 chat_service.h**

```cpp
#pragma once
#include <string>
#include <vector>

#include "db.h"
#include "llm_provider.h"

namespace chat {

// keep_rounds <= 0 表示不限轮数
std::vector<llm::ChatMsg> buildContext(const db::Persona& persona,
                                       const std::vector<db::Message>& history,
                                       int keep_rounds,
                                       const std::string& newUser);

}  // namespace chat
```

- [ ] **Step 2: 写 chat_service.cpp**

```cpp
#include "chat_service.h"

namespace chat {

std::vector<llm::ChatMsg> buildContext(const db::Persona& persona,
                                       const std::vector<db::Message>& history,
                                       int keep_rounds,
                                       const std::string& newUser) {
  std::vector<llm::ChatMsg> out;
  out.push_back({"system", persona.system_prompt});

  // 定位截断起点：历史中倒数第 keep_rounds 条 user 消息的下标
  std::vector<size_t> userIdx;
  for (size_t i = 0; i < history.size(); ++i)
    if (history[i].role == "user") userIdx.push_back(i);
  size_t start = 0;
  if (keep_rounds > 0 && userIdx.size() > static_cast<size_t>(keep_rounds))
    start = userIdx[userIdx.size() - keep_rounds];

  for (size_t i = start; i < history.size(); ++i) {
    if (history[i].role == "user" || history[i].role == "assistant")
      out.push_back({history[i].role, history[i].content});
  }
  out.push_back({"user", newUser});
  return out;
}

}  // namespace chat
```

- [ ] **Step 3: 写 tests/test_chat_service.cpp**

```cpp
#include <doctest/doctest.h>

#include "chat_service.h"
#include "json_util.h"

namespace {
db::Persona makePersona() {
  db::Persona p;
  p.name = "测试";
  p.system_prompt = "你是测试助手";
  return p;
}
db::Message msg(int64_t id, const std::string& role) {
  db::Message m;
  m.id = id;
  m.role = role;
  m.content = "content-" + std::to_string(id);
  return m;
}
}  // namespace

TEST_CASE("chat: 空历史 = system + 新 user") {
  auto ctx = chat::buildContext(makePersona(), {}, 20, "你好");
  REQUIRE(ctx.size() == 2);
  CHECK(ctx[0].role == "system");
  CHECK(ctx[0].content == "你是测试助手");
  CHECK(ctx[1].role == "user");
  CHECK(ctx[1].content == "你好");
}

TEST_CASE("chat: N 轮截断保留最后 keep_rounds 轮") {
  // 历史: u1 a1 u2 a2 u3 a3（3 轮）
  std::vector<db::Message> h = {msg(1, "user"),    msg(2, "assistant"),
                                msg(3, "user"),    msg(4, "assistant"),
                                msg(5, "user"),    msg(6, "assistant")};
  auto ctx = chat::buildContext(makePersona(), h, 2, "新问题");
  // 期望: system + u2 a2 u3 a3 + 新 user = 6 条
  REQUIRE(ctx.size() == 6);
  CHECK(ctx[1].content == "content-3");
  CHECK(ctx[4].content == "content-6");
  CHECK(ctx[5].role == "user");
  CHECK(ctx[5].content == "新问题");
}

TEST_CASE("chat: keep_rounds=0 表示不限轮数") {
  std::vector<db::Message> h = {msg(1, "user"), msg(2, "assistant")};
  auto ctx = chat::buildContext(makePersona(), h, 0, "再聊");
  REQUIRE(ctx.size() == 4);
  CHECK(ctx[1].content == "content-1");
}

TEST_CASE("chat: keep_rounds 大于实际轮数时不截断") {
  std::vector<db::Message> h = {msg(1, "user"), msg(2, "assistant")};
  auto ctx = chat::buildContext(makePersona(), h, 10, "hi");
  REQUIRE(ctx.size() == 4);
}

TEST_CASE("chat: 历史中的 system 角色消息被跳过") {
  std::vector<db::Message> h = {msg(1, "system"), msg(2, "user"), msg(3, "assistant")};
  auto ctx = chat::buildContext(makePersona(), h, 10, "hi");
  REQUIRE(ctx.size() == 4);  // system(人设) + user + assistant + 新user
  CHECK(ctx[0].content == "你是测试助手");
  CHECK(ctx[1].content == "content-2");
}

TEST_CASE("util: utf8Truncate 不截断中文中间") {
  CHECK(utf8Truncate("你好世界", 2) == "你好");
  CHECK(utf8Truncate("abc", 2) == "ab");
  CHECK(utf8Truncate("你好", 10) == "你好");
}
```

- [ ] **Step 4: 更新 CMakeLists.txt**

`backend` 与 `backend_tests` 加入 `src/chat_service.cpp`；`backend_tests` 再加入 `tests/test_chat_service.cpp`。

- [ ] **Step 5: 构建并运行测试**

```bash
cmake -S backend -B backend/build -A x64
cmake --build backend/build --config Release
ctest --test-dir backend/build -C Release --output-on-failure
# 期望: 全部 PASS
```

- [ ] **Step 6: Commit**

```bash
git add backend/
git commit -m "feat: ChatService 上下文组装与 N 轮截断"
```

### Task 6: 事件总线 + Webhook 插件分发

**Files:**
- Create: `backend/src/event_bus.h`, `backend/src/event_bus.cpp`
- Create: `backend/tests/test_event_bus.cpp`
- Modify: `backend/CMakeLists.txt`

**Interfaces:**
- Consumes: `cfg::Plugin`、`json_util.h`
- Produces:
  - `ev::EventBus`：`loadPlugins(vector<cfg::Plugin>)`、`subscribe(Listener)`、`emit(type, data)`、`instance()`（进程级单例）
  - 事件载荷统一为 `{"type":..., "ts":<ISO8601 UTC>, "data":...}`；Webhook 分发为 fire-and-forget（2s 超时，失败仅记日志）；插件 URL 仅支持 `http://`（本机插件）
  - 事件类型：`chat.completed`（data: `{session_id, length}`）、`chat.error`（data: `{session_id, code, message}`）、`session.created`（data: `{id,title}`）、`session.deleted`（data: `{id}`）、`persona.activated`（data: `{id,name}`）

- [ ] **Step 1: 写 event_bus.h**

```cpp
#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "json_util.h"

namespace ev {

using Listener = std::function<void(const std::string& type, const json& data)>;

class EventBus {
 public:
  static EventBus& instance();

  void loadPlugins(const std::vector<cfg::Plugin>& plugins);
  void subscribe(Listener l);
  // 同步通知本进程监听者；异步 fire-and-forget 分发到匹配事件的插件 Webhook
  void emit(const std::string& type, const json& data);

 private:
  std::mutex mtx_;
  std::vector<Listener> listeners_;
  std::vector<cfg::Plugin> plugins_;
};

}  // namespace ev
```

- [ ] **Step 2: 写 event_bus.cpp**

```cpp
#include "event_bus.h"

#include <algorithm>
#include <thread>

#include <httplib.h>

#include "logger.h"

namespace ev {
namespace {

struct ParsedUrl {
  std::string host;
  int port = 80;
  std::string path = "/";
};

ParsedUrl parseUrl(const std::string& u) {
  std::string s = u;
  if (s.rfind("http://", 0) == 0) s = s.substr(7);
  auto slash = s.find('/');
  std::string hostport = slash == std::string::npos ? s : s.substr(0, slash);
  ParsedUrl out;
  out.path = slash == std::string::npos ? "/" : s.substr(slash);
  auto colon = hostport.find(':');
  out.host = colon == std::string::npos ? hostport : hostport.substr(0, colon);
  if (colon != std::string::npos) {
    try {
      out.port = std::stoi(hostport.substr(colon + 1));
    } catch (...) {
      out.port = 80;
    }
  }
  return out;
}

}  // namespace

EventBus& EventBus::instance() {
  static EventBus bus;
  return bus;
}

void EventBus::loadPlugins(const std::vector<cfg::Plugin>& plugins) {
  std::lock_guard<std::mutex> lk(mtx_);
  plugins_ = plugins;
  logx::info("plugins loaded: " + std::to_string(plugins_.size()));
}

void EventBus::subscribe(Listener l) {
  std::lock_guard<std::mutex> lk(mtx_);
  listeners_.push_back(std::move(l));
}

void EventBus::emit(const std::string& type, const json& data) {
  json payload = {{"type", type}, {"ts", nowIso()}, {"data", data}};
  std::vector<Listener> ls;
  std::vector<cfg::Plugin> targets;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    ls = listeners_;
    for (auto& p : plugins_)
      if (std::find(p.events.begin(), p.events.end(), type) != p.events.end())
        targets.push_back(p);
  }
  for (auto& l : ls) {
    try {
      l(type, data);
    } catch (...) {
    }
  }
  for (auto& p : targets) {
    std::string body = payload.dump();
    std::thread([p, body]() {
      try {
        auto u = parseUrl(p.url);
        httplib::Client cli(u.host, u.port);
        cli.set_connection_timeout(2);
        cli.set_read_timeout(2);
        cli.set_write_timeout(2);
        auto res = cli.Post(u.path.c_str(), body, "application/json");
        if (!res || res->status != 200)
          logx::warn("plugin webhook failed: " + p.name);
      } catch (const std::exception& e) {
        logx::warn(std::string("plugin webhook exception: ") + e.what());
      }
    }).detach();
  }
}

}  // namespace ev
```

- [ ] **Step 3: 写 tests/test_event_bus.cpp（含真实本地 HTTP 分发验证）**

```cpp
#include <doctest/doctest.h>
#include <chrono>
#include <string>
#include <thread>

#include <httplib.h>

#include "event_bus.h"

TEST_CASE("event_bus: 本进程监听者收到事件") {
  ev::EventBus bus;
  std::string gotType;
  int gotSid = 0;
  bus.subscribe([&](const std::string& t, const json& d) {
    gotType = t;
    gotSid = d.value("session_id", 0);
  });
  bus.emit("chat.completed", {{"session_id", 7}, {"length", 42}});
  CHECK(gotType == "chat.completed");
  CHECK(gotSid == 7);
}

TEST_CASE("event_bus: 本进程监听者收到全部事件（过滤只发生在 Webhook 层）") {
  ev::EventBus bus;
  int calls = 0;
  bus.subscribe([&](const std::string&, const json&) { ++calls; });
  bus.emit("other.event", json::object());
  CHECK(calls == 1);
}

TEST_CASE("event_bus: webhook 分发到本地 HTTP 服务（载荷格式正确）") {
  httplib::Server s;
  std::string gotBody;
  s.Post("/hook", [&](const httplib::Request& req, httplib::Response& res) {
    gotBody = req.body;
    res.set_content("{}", "application/json");
  });
  const int port = 18900;
  std::thread t([&] { s.listen("127.0.0.1", port); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  ev::EventBus bus;
  cfg::Plugin p;
  p.name = "test-plugin";
  p.url = "http://127.0.0.1:18900/hook";
  p.events = {"chat.completed"};
  bus.loadPlugins({p});
  bus.emit("chat.completed", {{"session_id", 7}, {"length", 5}});
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  REQUIRE(!gotBody.empty());
  auto j = json::parse(gotBody);
  CHECK(j["type"] == "chat.completed");
  CHECK(j["ts"].is_string());
  CHECK(j["data"]["session_id"] == 7);

  s.stop();
  t.join();
}

TEST_CASE("event_bus: 插件未订阅的事件不分发") {
  httplib::Server s;
  int hits = 0;
  s.Post("/hook", [&](const httplib::Request&, httplib::Response& res) {
    ++hits;
    res.set_content("{}", "application/json");
  });
  const int port = 18901;
  std::thread t([&] { s.listen("127.0.0.1", port); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  ev::EventBus bus;
  cfg::Plugin p;
  p.name = "test-plugin";
  p.url = "http://127.0.0.1:18901/hook";
  p.events = {"session.created"};
  bus.loadPlugins({p});
  bus.emit("chat.completed", json::object());
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(hits == 0);

  s.stop();
  t.join();
}
```

- [ ] **Step 4: 更新 CMakeLists.txt**

`backend` 与 `backend_tests` 加入 `src/event_bus.cpp`；`backend_tests` 再加入 `tests/test_event_bus.cpp`。

- [ ] **Step 5: 构建并运行测试**

```bash
cmake -S backend -B backend/build -A x64
cmake --build backend/build --config Release
ctest --test-dir backend/build -C Release --output-on-failure
# 期望: 全部 PASS（webhook 测试需本机端口 18900/18901 空闲）
```

- [ ] **Step 6: Commit**

```bash
git add backend/
git commit -m "feat: 事件总线与 Webhook 插件分发"
```

---

### Task 7: REST 路由层（CRUD + 配置 + 导出 + CORS + 静态文件）+ main 接线 + 集成脚本

**Files:**
- Create: `backend/src/server.h`, `backend/src/server.cpp`
- Modify: `backend/src/main.cpp`（重写为正式入口）
- Create: `scripts/test_api.ps1`（CRUD 部分；Task 8 扩展聊天流部分）
- Modify: `backend/CMakeLists.txt`（加入 server.cpp）
- Modify: `.gitignore`（追加 `data_e2e/`）

**Interfaces:**
- Consumes: `db::*`、`cfg::*`、`dpapi::*`、`json_util.h`、`logger.h`
- Produces:
  - `srv::registerRoutes(httplib::Server& svr, const std::string& staticDir)` — 注册 /health 及全部 CRUD 路由、CORS、错误处理、静态挂载（Task 8 在函数内追加聊天路由）
  - 前端依赖的全部响应格式（见规格 §4）

- [ ] **Step 1: 写 server.h**

```cpp
#pragma once
#include <string>

#include <httplib.h>

namespace srv {
void registerRoutes(httplib::Server& svr, const std::string& staticDir);
}  // namespace srv
```

- [ ] **Step 2: 写 server.cpp（CRUD 路由；聊天路由在 Task 8 追加）**

```cpp
#include "server.h"

#include <ctime>

#include "config.h"
#include "db.h"
#include "dpapi.h"
#include "event_bus.h"
#include "json_util.h"
#include "logger.h"

namespace srv {
namespace {

json parseBody(const httplib::Request& req, httplib::Response& res) {
  try {
    return json::parse(req.body);
  } catch (...) {
    res.status = 400;
    res.set_content(errJson(400, "请求体不是合法 JSON").dump(), "application/json");
    return json();
  }
}

std::string maskKey(const std::string& key) {
  if (key.size() <= 10) return "****";
  return key.substr(0, 6) + "..." + key.substr(key.size() - 4);
}

json personaToJson(const db::Persona& p) {
  return {{"id", p.id},
          {"name", p.name},
          {"avatar", p.avatar},
          {"greeting", p.greeting},
          {"system_prompt", p.system_prompt},
          {"temperature", p.temperature},
          {"is_active", p.is_active},
          {"created_at", p.created_at},
          {"updated_at", p.updated_at}};
}

json sessionToJson(const db::Session& s) {
  return {{"id", s.id},
          {"title", s.title},
          {"created_at", s.created_at},
          {"updated_at", s.updated_at}};
}

json messageToJson(const db::Message& m) {
  return {{"id", m.id},
          {"session_id", m.session_id},
          {"role", m.role},
          {"content", m.content},
          {"created_at", m.created_at}};
}

void registerConfigRoutes(httplib::Server& svr) {
  svr.Get("/api/v1/config", [](const httplib::Request&, httplib::Response& res) {
    std::string keyEnc = db::getSetting("api_key_enc").value_or("");
    std::string masked;
    if (!keyEnc.empty()) {
      try {
        masked = maskKey(dpapi::unprotect(keyEnc));
      } catch (...) {
        masked = "****";
      }
    }
    json data = {{"base_url", db::getSetting("base_url").value_or("")},
                 {"api_key", masked},
                 {"model", db::getSetting("model").value_or("")},
                 {"keep_rounds", db::getSetting("keep_rounds").value_or("20")},
                 {"theme", db::getSetting("theme").value_or("dark")},
                 {"proxy_enabled", db::getSetting("proxy_enabled").value_or("0")},
                 {"proxy_host", db::getSetting("proxy_host").value_or("")},
                 {"proxy_port", db::getSetting("proxy_port").value_or("0")}};
    res.set_content(okJson(data).dump(), "application/json");
  });

  svr.Post("/api/v1/config", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status != 0) return;
    if (b.contains("base_url")) db::setSetting("base_url", b["base_url"].get<std::string>());
    if (b.contains("api_key")) {
      std::string key = b["api_key"].get<std::string>();
      if (!key.empty()) db::setSetting("api_key_enc", dpapi::protect(key));
    }
    if (b.contains("model")) db::setSetting("model", b["model"].get<std::string>());
    if (b.contains("keep_rounds")) db::setSetting("keep_rounds", std::to_string(b["keep_rounds"].get<int>()));
    if (b.contains("theme")) db::setSetting("theme", b["theme"].get<std::string>());
    if (b.contains("proxy_enabled")) db::setSetting("proxy_enabled", b["proxy_enabled"].get<bool>() ? "1" : "0");
    if (b.contains("proxy_host")) db::setSetting("proxy_host", b["proxy_host"].get<std::string>());
    if (b.contains("proxy_port")) db::setSetting("proxy_port", std::to_string(b["proxy_port"].get<int>()));
    res.set_content(okJson(json::object()).dump(), "application/json");
  });
}

void registerPersonaRoutes(httplib::Server& svr) {
  svr.Get("/api/v1/personas", [](const httplib::Request&, httplib::Response& res) {
    json arr = json::array();
    for (auto& p : db::listPersonas()) arr.push_back(personaToJson(p));
    res.set_content(okJson(arr).dump(), "application/json");
  });

  svr.Post("/api/v1/personas", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status != 0) return;
    if (!b.value("name", "").empty() == false || b.value("system_prompt", "").empty()) {
      res.status = 400;
      res.set_content(errJson(400, "name 与 system_prompt 为必填").dump(), "application/json");
      return;
    }
    db::Persona p;
    p.name = b["name"].get<std::string>();
    p.system_prompt = b["system_prompt"].get<std::string>();
    p.avatar = b.value("avatar", "🌸");
    p.greeting = b.value("greeting", "");
    p.temperature = b.value("temperature", 0.8);
    if (b.contains("id") && b["id"].get<int64_t>() > 0) {
      p.id = b["id"].get<int64_t>();
      if (!db::updatePersona(p)) {
        res.status = 404;
        res.set_content(errJson(404, "人设不存在").dump(), "application/json");
        return;
      }
    } else {
      p = db::createPersona(p);
    }
    res.set_content(okJson(personaToJson(p)).dump(), "application/json");
  });

  svr.Delete(R"(/api/v1/personas/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    if (!db::deletePersona(id)) {
      res.status = 409;
      res.set_content(errJson(409, "不能删除当前激活的人设").dump(), "application/json");
      return;
    }
    res.set_content(okJson(json::object()).dump(), "application/json");
  });

  svr.Post(R"(/api/v1/personas/(\d+)/activate)", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    if (!db::activatePersona(id)) {
      res.status = 404;
      res.set_content(errJson(404, "人设不存在").dump(), "application/json");
      return;
    }
    // 可选 query: ?session_id=N，若该会话为空则插入开场白
    auto persona = db::getActivePersona();
    if (req.has_param("session_id") && persona && !persona->greeting.empty()) {
      int64_t sid = std::stoll(req.get_param_value("session_id"));
      auto sess = db::getSession(sid);
      if (sess && db::listMessages(sid).empty()) {
        db::appendMessage(sid, "assistant", persona->greeting);
        db::touchSession(sid);
      }
    }
    ev::EventBus::instance().emit("persona.activated",
                                  {{"id", id}, {"name", persona ? persona->name : ""}});
    res.set_content(okJson(json::object()).dump(), "application/json");
  });
}

void registerSessionRoutes(httplib::Server& svr) {
  svr.Get("/api/v1/sessions", [](const httplib::Request&, httplib::Response& res) {
    json arr = json::array();
    for (auto& s : db::listSessions()) arr.push_back(sessionToJson(s));
    res.set_content(okJson(arr).dump(), "application/json");
  });

  svr.Post("/api/v1/sessions", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status != 0) return;
    auto s = db::createSession(b.value("title", "新对话"));
    ev::EventBus::instance().emit("session.created", {{"id", s.id}, {"title", s.title}});
    res.set_content(okJson(sessionToJson(s)).dump(), "application/json");
  });

  svr.Patch(R"(/api/v1/sessions/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status != 0) return;
    int64_t id = std::stoll(req.matches[1].str());
    std::string title = b.value("title", "");
    if (title.empty()) {
      res.status = 400;
      res.set_content(errJson(400, "title 不能为空").dump(), "application/json");
      return;
    }
    db::renameSession(id, title);
    res.set_content(okJson(json::object()).dump(), "application/json");
  });

  svr.Delete(R"(/api/v1/sessions/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    db::deleteSession(id);
    ev::EventBus::instance().emit("session.deleted", {{"id", id}});
    res.set_content(okJson(json::object()).dump(), "application/json");
  });
}

void registerMessageRoutes(httplib::Server& svr) {
  svr.Get("/api/v1/messages", [](const httplib::Request& req, httplib::Response& res) {
    int64_t sid = req.has_param("session_id") ? std::stoll(req.get_param_value("session_id")) : 0;
    if (!sid) {
      res.status = 400;
      res.set_content(errJson(400, "缺少 session_id").dump(), "application/json");
      return;
    }
    int limit = req.has_param("limit") ? std::stoi(req.get_param_value("limit")) : 100;
    auto msgs = db::listMessages(sid, limit);
    json arr = json::array();
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) arr.push_back(messageToJson(*it));
    res.set_content(okJson(arr).dump(), "application/json");
  });

  // 清空会话消息（query 版）
  svr.Delete("/api/v1/messages", [](const httplib::Request& req, httplib::Response& res) {
    int64_t sid = req.has_param("session_id") ? std::stoll(req.get_param_value("session_id")) : 0;
    if (!sid) {
      res.status = 400;
      res.set_content(errJson(400, "缺少 session_id").dump(), "application/json");
      return;
    }
    db::clearMessages(sid);
    res.set_content(okJson(json::object()).dump(), "application/json");
  });

  svr.Delete(R"(/api/v1/messages/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    db::deleteMessage(id);
    res.set_content(okJson(json::object()).dump(), "application/json");
  });
}

void registerExportRoute(httplib::Server& svr) {
  svr.Post("/api/v1/export", [](const httplib::Request& req, httplib::Response& res) {
    int64_t sid = req.has_param("session_id") ? std::stoll(req.get_param_value("session_id")) : 0;
    std::string fmt = req.has_param("format") ? req.get_param_value("format") : "md";
    if (!sid) {
      res.status = 400;
      res.set_content(errJson(400, "缺少 session_id").dump(), "application/json");
      return;
    }
    auto sess = db::getSession(sid);
    if (!sess) {
      res.status = 404;
      res.set_content(errJson(404, "会话不存在").dump(), "application/json");
      return;
    }
    auto msgs = db::listMessages(sid);

    std::string body, mime, ext;
    if (fmt == "json") {
      mime = "application/json";
      ext = "json";
      json arr = json::array();
      for (auto& m : msgs)
        arr.push_back({{"role", m.role}, {"content", m.content}, {"created_at", m.created_at}});
      body = json{{"title", sess->title}, {"exported_at", nowIso()}, {"messages", arr}}.dump(2);
    } else if (fmt == "txt") {
      mime = "text/plain; charset=utf-8";
      ext = "txt";
      body = "会话：" + sess->title + "\n导出时间：" + nowIso() + "\n\n";
      for (auto& m : msgs) {
        std::string who = m.role == "user" ? "用户" : (m.role == "assistant" ? "AI" : "系统");
        body += "[" + who + "] " + m.created_at + "\n" + m.content + "\n\n";
      }
    } else {
      mime = "text/markdown; charset=utf-8";
      ext = "md";
      body = "# " + sess->title + "\n\n> 导出时间：" + nowIso() + "\n\n";
      for (auto& m : msgs) {
        std::string who = m.role == "user" ? "**用户**" : (m.role == "assistant" ? "**AI**" : "**系统**");
        body += who + "（" + m.created_at + "）\n\n" + m.content + "\n\n---\n\n";
      }
    }
    std::string fname = "session_" + std::to_string(sid) + "_" + std::to_string(std::time(nullptr)) + "." + ext;
    res.set_header("Content-Disposition", "attachment; filename=\"" + fname + "\"");
    res.set_content(body, mime);
  });
}

}  // namespace

void registerRoutes(httplib::Server& svr, const std::string& staticDir) {
  // CORS：允许 WebView2 虚拟主机与本地浏览器调试来源
  svr.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
    std::string origin = req.get_header_value("Origin");
    bool allowed = origin == "https://app.local" ||
                   origin.rfind("http://localhost:", 0) == 0 ||
                   origin.rfind("http://127.0.0.1:", 0) == 0;
    if (allowed) {
      res.set_header("Access-Control-Allow-Origin", origin);
      res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
      res.set_header("Access-Control-Allow-Headers", "Content-Type");
    }
    if (req.method == "OPTIONS") {
      res.status = 204;
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"status\":\"ok\",\"version\":\"1.0.0\"}", "application/json");
  });

  registerConfigRoutes(svr);
  registerPersonaRoutes(svr);
  registerSessionRoutes(svr);
  registerMessageRoutes(svr);
  registerExportRoute(svr);

  svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
    res.set_content(errJson(404, "接口不存在").dump(), "application/json");
  });

  if (!staticDir.empty()) {
    svr.set_mount_point("/", staticDir);
    svr.set_file_extension_and_mimetype_mapping("css", "text/css");
    svr.set_file_extension_and_mimetype_mapping("js", "text/javascript");
    svr.set_file_extension_and_mimetype_mapping("html", "text/html");
    svr.set_file_extension_and_mimetype_mapping("json", "application/json");
    svr.set_file_extension_and_mimetype_mapping("png", "image/png");
    svr.set_file_extension_and_mimetype_mapping("svg", "image/svg+xml");
    svr.set_file_extension_and_mimetype_mapping("woff2", "font/woff2");
    logx::info("static file serving enabled: " + staticDir);
  }
}

}  // namespace srv
```

（`registerPersonaRoutes` 中 activate 处理器使用了 `ev::EventBus`，因此头部需 include `event_bus.h`，见上方 include 列表。）

- [ ] **Step 3: 重写 main.cpp（正式入口：配置加载 + DB + 端口回退 + 路由注册）**

```cpp
#include <httplib.h>
#include <string>

#include "config.h"
#include "db.h"
#include "logger.h"
#include "server.h"

int main(int argc, char** argv) {
  std::string dataDir = "data";
  std::string staticDir;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    if (k == "--data") dataDir = argv[i + 1];
    else if (k == "--static") staticDir = argv[i + 1];
  }
  logx::init(dataDir + "/logs");
  logx::info("backend starting, data dir: " + dataDir);

  auto cfgPath = dataDir + "/config.json";
  auto sc = cfg::load(cfgPath);
  if (!staticDir.empty()) sc.static_dir = staticDir;

  if (!db::open(dataDir + "/app.db")) {
    logx::error("db open failed, exit");
    return 1;
  }

  httplib::Server svr;
  srv::registerRoutes(svr, sc.static_dir);

  for (int port = sc.port; port <= sc.port + 10; ++port) {
    logx::info("trying to listen on " + sc.host + ":" + std::to_string(port));
    if (svr.listen(sc.host, port)) {
      sc.actual_port = port;
      cfg::save(cfgPath, sc);
      logx::info("listening on port " + std::to_string(port));
      db::close();
      return 0;
    }
  }
  logx::error("no available port");
  return 1;
}
```

- [ ] **Step 4: 更新 CMakeLists.txt**

`backend` 源文件加入 `src/server.cpp`（`backend_tests` 不需要 server.cpp）。

- [ ] **Step 5: 写 scripts/test_api.ps1（CRUD 部分，聊天流在 Task 8 扩展）**

```powershell
$ErrorActionPreference = 'Stop'
$base = 'http://127.0.0.1:8765'

function Assert-True($cond, $msg) {
  if (-not $cond) { throw "FAIL: $msg" }
}

# health
$h = Invoke-RestMethod "$base/health"
Assert-True ($h.status -eq 'ok') 'health'

# 人设列表（种子默认人设）
$p = Invoke-RestMethod "$base/api/v1/personas"
Assert-True ($p.code -eq 0) 'personas list code'
Assert-True ($p.data.Count -ge 1) 'personas seeded'

# 会话创建
$s = Invoke-RestMethod "$base/api/v1/sessions" -Method Post -Body '{"title":"E2E测试会话"}' -ContentType 'application/json'
Assert-True ($s.code -eq 0) 'session create'
$sid = $s.data.id

# 重命名
Invoke-RestMethod "$base/api/v1/sessions/$sid" -Method Patch -Body '{"title":"改名后"}' -ContentType 'application/json' | Out-Null
$list = Invoke-RestMethod "$base/api/v1/sessions"
Assert-True (($list.data | Where-Object id -eq $sid).title -eq '改名后') 'session rename'

# 配置读写（key 留空不变更）
$cfg = @{ base_url='https://api.example.com/v1'; model='demo-model'; keep_rounds=5; theme='sakura'; proxy_enabled=$false; proxy_host=''; proxy_port=0 } | ConvertTo-Json
Invoke-RestMethod "$base/api/v1/config" -Method Post -Body $cfg -ContentType 'application/json' | Out-Null
$c = Invoke-RestMethod "$base/api/v1/config"
Assert-True ($c.data.model -eq 'demo-model') 'config model roundtrip'
Assert-True ($c.data.theme -eq 'sakura') 'config theme roundtrip'
Assert-True ($c.data.keep_rounds -eq '5') 'config keep_rounds roundtrip'

# 清理：删除测试会话
Invoke-RestMethod "$base/api/v1/sessions/$sid" -Method Delete | Out-Null
$list = Invoke-RestMethod "$base/api/v1/sessions"
Assert-True (($list.data | Where-Object id -eq $sid) -eq $null) 'session delete'

Write-Host 'CRUD API TESTS PASSED'
```

- [ ] **Step 6: 更新 .gitignore（追加一行）**

```gitignore
data_e2e/
```

- [ ] **Step 7: 构建 + 启动后端 + 运行集成脚本**

```bash
cmake -S backend -B backend/build -A x64
cmake --build backend/build --config Release
ctest --test-dir backend/build -C Release --output-on-failure
# 启动后端（后台）
./backend/build/Release/backend.exe --data data_e2e &
sleep 1
powershell.exe -ExecutionPolicy Bypass -File scripts/test_api.ps1
# 期望输出: CRUD API TESTS PASSED
kill %1
```

- [ ] **Step 8: Commit**

```bash
git add backend/ scripts/ .gitignore
git commit -m "feat: REST 路由层（CRUD/配置/导出/CORS/静态）与集成测试脚本"
```

---

### Task 8: SSE 流式聊天路由（stream / regenerate / stop）

**Files:**
- Modify: `backend/src/server.cpp`（追加聊天路由 + runChat 辅助 + 停止标志表）
- Modify: `backend/src/chat_service.cpp`（`newUser` 为空时不追加 user 消息 —— 供 regenerate 复用）
- Modify: `backend/tests/test_chat_service.cpp`（补充空 newUser 用例）
- Modify: `scripts/test_api.ps1`（扩展聊天流/导出/重生成断言）
- Modify: `backend/src/main.cpp`（加载插件清单到事件总线）

**Interfaces:**
- Consumes: `chat::buildContext`、`llm::OpenAiCompatProvider`、`db::*`、`dpapi::unprotect`、`ev::EventBus::instance()`
- Produces: `POST /api/v1/chat/stream`、`POST /api/v1/chat/regenerate`、`POST /api/v1/chat/stop`（SSE 事件格式见规格 §4）

- [ ] **Step 1: 修改 chat_service.cpp（空 newUser 不追加）**

将 `chat_service.cpp` 中结尾的：

```cpp
  out.push_back({"user", newUser});
  return out;
```

替换为：

```cpp
  if (!newUser.empty()) out.push_back({"user", newUser});
  return out;
```

- [ ] **Step 2: 修改 tests/test_chat_service.cpp（追加用例）**

在文件末尾追加：

```cpp
TEST_CASE("chat: newUser 为空时不追加（regenerate 场景）") {
  std::vector<db::Message> h = {msg(1, "user"), msg(2, "assistant")};
  auto ctx = chat::buildContext(makePersona(), h, 10, "");
  REQUIRE(ctx.size() == 3);  // system + user + assistant，不重复追加 user
  CHECK(ctx[2].content == "content-2");
}
```

- [ ] **Step 3: 修改 server.cpp —— 文件头部追加 include 与共享状态**

在 `#include "logger.h"` 之后追加：

```cpp
#include <atomic>
#include <map>

#include "chat_service.h"
#include "event_bus.h"
#include "llm_provider.h"
#include "openai_provider.h"
```

在 `namespace srv { namespace {` 内、`parseBody` 之前追加：

```cpp
std::mutex g_stopMtx;
std::map<int64_t, std::shared_ptr<std::atomic_bool>> g_stopFlags;

int64_t parseSessionId(const httplib::Request& req, httplib::Response& res) {
  json b = parseBody(req, res);
  if (res.status != 0) return 0;
  int64_t sid = b.value("session_id", (int64_t)0);
  if (!sid) {
    res.status = 400;
    res.set_content(errJson(400, "缺少 session_id").dump(), "application/json");
    return 0;
  }
  return sid;
}

// 核心流式管线：组装上下文 → 调用 provider → SSE 转发 → 落库
void runChat(int64_t sessionId, const std::string& content, bool appendUser,
             httplib::Response& res) {
  auto persona = db::getActivePersona();
  if (!persona) {
    res.status = 500;
    res.set_content(errJson(500, "没有激活的人设").dump(), "application/json");
    return;
  }
  std::string key;
  std::string keyEnc = db::getSetting("api_key_enc").value_or("");
  if (!keyEnc.empty()) {
    try {
      key = dpapi::unprotect(keyEnc);
    } catch (const std::exception& e) {
      logx::error(std::string("dpapi unprotect failed: ") + e.what());
    }
  }
  int keep = 20;
  try {
    keep = std::stoi(db::getSetting("keep_rounds").value_or("20"));
  } catch (...) {
  }
  int proxyPort = 0;
  try {
    proxyPort = std::stoi(db::getSetting("proxy_port").value_or("0"));
  } catch (...) {
  }

  // 首条消息自动生成标题
  if (appendUser) {
    auto sess = db::getSession(sessionId);
    if (sess && sess->title == "新对话") db::renameSession(sessionId, utf8Truncate(content, 20));
    db::appendMessage(sessionId, "user", content);
    db::touchSession(sessionId);
  }

  auto history = db::listMessages(sessionId);
  auto ctx = chat::buildContext(*persona, history, keep, content);

  llm::ChatRequest req;
  req.messages = ctx;
  req.temperature = persona->temperature;
  req.base_url = db::getSetting("base_url").value_or("");
  req.api_key = key;
  req.model = db::getSetting("model").value_or("");
  req.proxy_enabled = db::getSetting("proxy_enabled").value_or("0") == "1";
  req.proxy_host = db::getSetting("proxy_host").value_or("");
  req.proxy_port = proxyPort;

  auto stop = std::make_shared<std::atomic_bool>(false);
  {
    std::lock_guard<std::mutex> lk(g_stopMtx);
    g_stopFlags[sessionId] = stop;
  }

  llm::OpenAiCompatProvider provider;
  std::string full;
  bool hadError = false;
  int errCode = 0;
  std::string errMsg;

  res.set_header("Cache-Control", "no-cache");
  res.set_chunked_content_provider(
      "text/event-stream", [&](size_t, httplib::DataSink& sink) {
        provider.streamChat(
            req,
            [&](const llm::StreamEvent& e) {
              if (!sink.is_writable()) {
                stop->store(true);
                return;
              }
              if (e.type == "delta") {
                full += e.content;
                json j = {{"content", e.content}};
                std::string sse = "event: delta\ndata: " + j.dump() + "\n\n";
                sink.write(sse.data(), sse.size());
              } else if (e.type == "error") {
                hadError = true;
                errCode = e.error_code;
                errMsg = e.message;
                json j = {{"code", e.error_code}, {"message", e.message}};
                std::string sse = "event: error\ndata: " + j.dump() + "\n\n";
                sink.write(sse.data(), sse.size());
              }
            },
            *stop);
        {
          std::lock_guard<std::mutex> lk(g_stopMtx);
          g_stopFlags.erase(sessionId);
        }
        if (!hadError && !full.empty()) {
          db::appendMessage(sessionId, "assistant", full);
          db::touchSession(sessionId);
          ev::EventBus::instance().emit(
              "chat.completed",
              {{"session_id", sessionId}, {"length", full.size()}});
        } else if (hadError) {
          ev::EventBus::instance().emit(
              "chat.error", {{"session_id", sessionId},
                             {"code", errCode},
                             {"message", errMsg}});
        }
        if (sink.is_writable()) {
          json done = {{"session_id", sessionId}};
          std::string sse = "event: done\ndata: " + done.dump() + "\n\n";
          sink.write(sse.data(), sse.size());
        }
        return true;
      });
}

void registerChatRoutes(httplib::Server& svr) {
  svr.Post("/api/v1/chat/stream", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status != 0) return;
    int64_t sid = b.value("session_id", (int64_t)0);
    std::string content = b.value("content", "");
    if (!sid) {
      res.status = 400;
      res.set_content(errJson(400, "缺少 session_id").dump(), "application/json");
      return;
    }
    if (content.empty()) {
      res.status = 400;
      res.set_content(errJson(400, "消息内容为空").dump(), "application/json");
      return;
    }
    runChat(sid, content, true, res);
  });

  svr.Post("/api/v1/chat/regenerate", [](const httplib::Request& req, httplib::Response& res) {
    int64_t sid = parseSessionId(req, res);
    if (!sid) return;
    auto lastU = db::lastMessageByRole(sid, "user");
    if (!lastU) {
      res.status = 400;
      res.set_content(errJson(400, "没有可重新生成的消息").dump(), "application/json");
      return;
    }
    auto lastA = db::lastMessageByRole(sid, "assistant");
    if (lastA) db::deleteMessage(lastA->id);
    runChat(sid, lastU->content, false, res);
  });

  svr.Post("/api/v1/chat/stop", [](const httplib::Request& req, httplib::Response& res) {
    int64_t sid = parseSessionId(req, res);
    if (!sid) return;
    {
      std::lock_guard<std::mutex> lk(g_stopMtx);
      auto it = g_stopFlags.find(sid);
      if (it != g_stopFlags.end()) it->second->store(true);
    }
    res.set_content(okJson(json{{"stopped", true}}).dump(), "application/json");
  });
}
```

- [ ] **Step 4: 修改 server.cpp —— registerRoutes 中挂载聊天路由**

在 `registerRoutes` 的 `registerExportRoute(svr);` 之后、`set_error_handler` 之前加入：

```cpp
  registerChatRoutes(svr);
```

（Task 7 的 activate 处理器已使用 `ev::EventBus`，此处无需额外修改。）

- [ ] **Step 5: 修改 main.cpp —— 加载插件清单**

在 `srv::registerRoutes(svr, sc.static_dir);` 之前加入：

```cpp
  ev::EventBus::instance().loadPlugins(sc.plugins);
```

并在 include 区加入 `#include "event_bus.h"`。

- [ ] **Step 6: 扩展 scripts/test_api.ps1（在"清理"段之前插入）**

```powershell
# 聊天流（LLM_MOCK 假流）
$payload = @{ session_id=$sid; content='你好呀' } | ConvertTo-Json
$tmp = Join-Path $env:TEMP 'ac_sse.txt'
& curl.exe -s -N -X POST "$base/api/v1/chat/stream" -H "Content-Type: application/json" -d $payload -o $tmp
$sse = Get-Content $tmp -Raw
Remove-Item $tmp
Assert-True ($sse -match 'event: delta') 'sse delta event'
Assert-True ($sse -match 'event: done') 'sse done event'

# 消息已落库（user + assistant 各一条）
$m = Invoke-RestMethod "$base/api/v1/messages?session_id=$sid&limit=10"
Assert-True ($m.data.Count -eq 2) "messages count=$($m.data.Count)"
Assert-True (($m.data | Where-Object role -eq 'assistant').content.Length -gt 0) 'assistant message stored'

# 重新生成（删除最后一条 assistant 并重跑，仍应有 2 条）
$reg = @{ session_id=$sid } | ConvertTo-Json
$tmp2 = Join-Path $env:TEMP 'ac_sse2.txt'
& curl.exe -s -N -X POST "$base/api/v1/chat/regenerate" -H "Content-Type: application/json" -d $reg -o $tmp2
$sse2 = Get-Content $tmp2 -Raw
Remove-Item $tmp2
Assert-True ($sse2 -match 'event: done') 'regenerate done'
$m2 = Invoke-RestMethod "$base/api/v1/messages?session_id=$sid&limit=10"
Assert-True ($m2.data.Count -eq 2) 'regenerate keeps 2 messages'

# 导出 md 与 json
$tmp3 = Join-Path $env:TEMP 'ac_export.md'
& curl.exe -s -X POST "$base/api/v1/export?session_id=$sid&format=md" -o $tmp3
$md = Get-Content $tmp3 -Raw
Remove-Item $tmp3
Assert-True ($md -match '用户') 'export md contains user label'

$tmp4 = Join-Path $env:TEMP 'ac_export.json'
& curl.exe -s -X POST "$base/api/v1/export?session_id=$sid&format=json" -o $tmp4
$j = Get-Content $tmp4 -Raw | ConvertFrom-Json
Remove-Item $tmp4
Assert-True ($j.messages.Count -eq 2) 'export json message count'

# 清空会话消息
Invoke-RestMethod "$base/api/v1/messages?session_id=$sid" -Method Delete | Out-Null
$m3 = Invoke-RestMethod "$base/api/v1/messages?session_id=$sid"
Assert-True ($m3.data.Count -eq 0) 'messages cleared'
```

- [ ] **Step 7: 构建 + 单测 + 集成测试**

```bash
cmake -S backend -B backend/build -A x64
cmake --build backend/build --config Release
ctest --test-dir backend/build -C Release --output-on-failure
# 期望: 全部 PASS
LLM_MOCK=1 ./backend/build/Release/backend.exe --data data_e2e &
sleep 1
powershell.exe -ExecutionPolicy Bypass -File scripts/test_api.ps1
# 期望输出: CRUD API TESTS PASSED
kill %1
```

- [ ] **Step 8: Commit**

```bash
git add backend/ scripts/
git commit -m "feat: SSE 流式聊天接口（stream/regenerate/stop）与集成断言"
```

### Task 9: 前端骨架（index.html + 多主题 theme.css + vendor 本地打包）

**Files:**
- Create: `src/AiCompanion.App/wwwroot/index.html`
- Create: `src/AiCompanion.App/wwwroot/css/theme.css`
- Create: `src/AiCompanion.App/wwwroot/js/app.js`（占位 stub，Task 10 全量替换）
- Download: `src/AiCompanion.App/wwwroot/vendor/`（marked / highlight.js + 主题 css / DOMPurify）

**Interfaces:**
- Consumes: 无（首个前端任务）
- Produces:
  - 页面元素 id 约定（Task 10 的 app.js 依赖，不得改名）：`#session-list`、`#btn-new-session`、`#persona-switcher`、`#btn-settings`、`#chat-title`、`#btn-export`、`#btn-clear`、`#messages`、`#input`、`#btn-send`、`#btn-stop`、`#stream-hint`、`#banner`、`#toast`、设置弹窗 `#modal-settings` 内 `#set-base-url`/`#set-api-key`/`#set-model`/`#set-keep-rounds`/`#set-theme`/`#set-proxy-enabled`/`#set-proxy-host`/`#set-proxy-port`/`#btn-settings-save`/`#btn-settings-cancel`；人设弹窗 `#modal-persona` 内 `#pe-name`/`#pe-avatar`/`#pe-greeting`/`#pe-prompt`/`#pe-temp`/`#btn-persona-save`/`#btn-persona-cancel`；标题栏 `#btn-min`/`#btn-max`/`#btn-close`
  - 主题约定：`<html data-theme="dark|light|sakura|midnight">`，全部颜色走 CSS 变量
  - 全局变量：`window.__API_BASE__`（WPF 注入，浏览器调试时回退 `http://127.0.0.1:8765`）

- [ ] **Step 1: 下载 vendor 库（4 个文件）**

```bash
mkdir -p src/AiCompanion.App/wwwroot/vendor src/AiCompanion.App/wwwroot/css src/AiCompanion.App/wwwroot/js
curl.exe -L -o src/AiCompanion.App/wwwroot/vendor/marked.min.js https://cdn.jsdelivr.net/npm/marked@12.0.2/marked.min.js
curl.exe -L -o src/AiCompanion.App/wwwroot/vendor/highlight.min.js https://cdn.jsdelivr.net/npm/@highlightjs/cdn-assets@11.9.0/highlight.min.js
curl.exe -L -o src/AiCompanion.App/wwwroot/vendor/highlight-github-dark.min.css https://cdn.jsdelivr.net/npm/@highlightjs/cdn-assets@11.9.0/styles/github-dark.min.css
curl.exe -L -o src/AiCompanion.App/wwwroot/vendor/purify.min.js https://cdn.jsdelivr.net/npm/dompurify@3.1.6/dist/purify.min.js
```

验证：4 个文件均非空（`ls -la` 确认每个 > 10KB）。

- [ ] **Step 2: 写 index.html**

```html
<!DOCTYPE html>
<html lang="zh-CN" data-theme="dark">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>AI 陪伴</title>
  <link rel="stylesheet" href="css/theme.css">
  <link rel="stylesheet" href="vendor/highlight-github-dark.min.css">
</head>
<body>
  <div id="app">
    <header id="titlebar">
      <div class="brand">✦ AI 陪伴</div>
      <div class="tb-actions">
        <button class="tb-btn" id="btn-min" title="最小化">─</button>
        <button class="tb-btn" id="btn-max" title="最大化">□</button>
        <button class="tb-btn tb-close" id="btn-close" title="关闭">✕</button>
      </div>
    </header>
    <div id="main">
      <aside id="sidebar">
        <div class="side-head">
          <button id="btn-new-session" class="btn-primary full">＋ 新对话</button>
        </div>
        <div id="session-list" class="session-list"></div>
        <div class="side-foot">
          <div id="persona-switcher" class="persona-row"></div>
          <button id="btn-settings" class="btn-ghost full">⚙ 设置</button>
        </div>
      </aside>
      <section id="chat">
        <div id="chat-head">
          <span id="chat-title">新对话</span>
          <div class="chat-actions">
            <button id="btn-export" class="btn-ghost">导出</button>
            <button id="btn-clear" class="btn-ghost">清空</button>
          </div>
        </div>
        <div id="messages"></div>
        <div id="composer">
          <textarea id="input" rows="3" placeholder="说点什么吧…（Enter 发送，Shift+Enter 换行）"></textarea>
          <div class="composer-bar">
            <span id="stream-hint" class="hint hidden">正在生成…</span>
            <button id="btn-send" class="btn-primary">发送</button>
            <button id="btn-stop" class="btn-danger hidden">停止</button>
          </div>
        </div>
      </section>
    </div>
  </div>

  <div id="banner" class="banner hidden"></div>
  <div id="toast" class="toast hidden"></div>

  <div id="modal-settings" class="modal hidden">
    <div class="modal-box">
      <h3>设置</h3>
      <label>API Base URL</label>
      <input id="set-base-url" placeholder="https://api.deepseek.com/v1">
      <label>API Key</label>
      <input id="set-api-key" type="password" placeholder="sk-...（留空保持不变）">
      <label>模型名称</label>
      <input id="set-model" placeholder="deepseek-chat">
      <label>保留上下文轮数（0 = 不限）</label>
      <input id="set-keep-rounds" type="number" min="0" max="200">
      <label>主题</label>
      <select id="set-theme">
        <option value="dark">深色</option>
        <option value="light">亮色</option>
        <option value="sakura">樱花粉</option>
        <option value="midnight">午夜蓝</option>
      </select>
      <label class="chk"><input type="checkbox" id="set-proxy-enabled"> 使用 HTTP 代理</label>
      <label>代理地址（留空 = 使用系统代理）</label>
      <input id="set-proxy-host" placeholder="127.0.0.1">
      <label>代理端口</label>
      <input id="set-proxy-port" type="number" placeholder="7890">
      <div class="modal-actions">
        <button id="btn-settings-cancel" class="btn-ghost">取消</button>
        <button id="btn-settings-save" class="btn-primary">保存</button>
      </div>
    </div>
  </div>

  <div id="modal-persona" class="modal hidden">
    <div class="modal-box">
      <h3 id="persona-modal-title">编辑人设</h3>
      <label>名称</label><input id="pe-name">
      <label>头像（emoji）</label><input id="pe-avatar" placeholder="🌸">
      <label>开场白</label><input id="pe-greeting" placeholder="切换人设时的第一句话（可留空）">
      <label>System Prompt</label><textarea id="pe-prompt" rows="4"></textarea>
      <label>Temperature（0.1 - 2.0）</label>
      <input id="pe-temp" type="number" min="0.1" max="2" step="0.1" value="0.8">
      <div class="modal-actions">
        <button id="btn-persona-cancel" class="btn-ghost">取消</button>
        <button id="btn-persona-save" class="btn-primary">保存</button>
      </div>
    </div>
  </div>

  <script src="vendor/marked.min.js"></script>
  <script src="vendor/highlight.min.js"></script>
  <script src="vendor/purify.min.js"></script>
  <script src="js/app.js"></script>
</body>
</html>
```

- [ ] **Step 3: 写 css/theme.css（4 套主题变量 + 全部组件样式）**

```css
/* ============ 主题变量 ============ */
:root[data-theme="dark"] {
  --bg: #0f1117;
  --bg-panel: #171a23;
  --bg-elevated: #1f2330;
  --bg-hover: #262b3a;
  --text: #e8eaf0;
  --text-dim: #9aa1b5;
  --accent: #6c8cff;
  --accent-strong: #4f6ef7;
  --bubble-user: #4f6ef7;
  --bubble-user-text: #ffffff;
  --bubble-ai: #232838;
  --bubble-ai-text: #e8eaf0;
  --danger: #e5534b;
  --border: #2a3040;
  --shadow: 0 8px 32px rgba(0, 0, 0, 0.45);
  --scrollbar: #343b4e;
}
:root[data-theme="light"] {
  --bg: #f5f6fa;
  --bg-panel: #ffffff;
  --bg-elevated: #ffffff;
  --bg-hover: #eef0f6;
  --text: #2b2f3a;
  --text-dim: #8a90a0;
  --accent: #5b7cfa;
  --accent-strong: #4f6ef7;
  --bubble-user: #4f6ef7;
  --bubble-user-text: #ffffff;
  --bubble-ai: #eef0f6;
  --bubble-ai-text: #2b2f3a;
  --danger: #d64541;
  --border: #e2e5ee;
  --shadow: 0 8px 32px rgba(30, 40, 80, 0.12);
  --scrollbar: #c8ccd8;
}
:root[data-theme="sakura"] {
  --bg: #fff3f6;
  --bg-panel: #fffbfc;
  --bg-elevated: #ffffff;
  --bg-hover: #ffe9ef;
  --text: #4a3540;
  --text-dim: #b08b98;
  --accent: #f48fb1;
  --accent-strong: #ec6a97;
  --bubble-user: #ec6a97;
  --bubble-user-text: #ffffff;
  --bubble-ai: #ffe9ef;
  --bubble-ai-text: #4a3540;
  --danger: #e5534b;
  --border: #f8dbe5;
  --shadow: 0 8px 32px rgba(236, 106, 151, 0.15);
  --scrollbar: #f0c3d1;
}
:root[data-theme="midnight"] {
  --bg: #0a1024;
  --bg-panel: #101a38;
  --bg-elevated: #16224a;
  --bg-hover: #1d2c5c;
  --text: #dbe4ff;
  --text-dim: #7d8bb8;
  --accent: #38bdf8;
  --accent-strong: #0ea5e9;
  --bubble-user: #0ea5e9;
  --bubble-user-text: #ffffff;
  --bubble-ai: #16224a;
  --bubble-ai-text: #dbe4ff;
  --danger: #e5534b;
  --border: #1d2c5c;
  --shadow: 0 8px 32px rgba(0, 0, 0, 0.5);
  --scrollbar: #24336b;
}

/* ============ 基础 ============ */
* { box-sizing: border-box; margin: 0; padding: 0; }
html, body { height: 100%; overflow: hidden; }
body {
  font-family: "Segoe UI", "Microsoft YaHei UI", "PingFang SC", sans-serif;
  background: var(--bg);
  color: var(--text);
  font-size: 14px;
  transition: background 0.25s, color 0.25s;
}
.hidden { display: none !important; }
button { font-family: inherit; cursor: pointer; border: none; }
input, textarea, select {
  font-family: inherit;
  background: var(--bg-elevated);
  color: var(--text);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 8px 10px;
  font-size: 14px;
  outline: none;
}
input:focus, textarea:focus, select:focus { border-color: var(--accent); }

/* ============ 布局 ============ */
#app { display: flex; flex-direction: column; height: 100vh; }
#titlebar {
  height: 42px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 12px;
  background: var(--bg-panel);
  border-bottom: 1px solid var(--border);
  user-select: none;
  -webkit-user-select: none;
}
.brand { font-weight: 700; letter-spacing: 1px; }
.tb-actions { display: flex; gap: 4px; -webkit-app-region: no-drag; }
.tb-btn {
  width: 34px; height: 26px;
  background: transparent;
  color: var(--text-dim);
  border-radius: 6px;
  font-size: 12px;
}
.tb-btn:hover { background: var(--bg-hover); color: var(--text); }
.tb-close:hover { background: var(--danger); color: #fff; }
#main { flex: 1; display: grid; grid-template-columns: 250px 1fr; min-height: 0; }

/* ============ 侧边栏 ============ */
#sidebar {
  background: var(--bg-panel);
  border-right: 1px solid var(--border);
  display: flex;
  flex-direction: column;
  padding: 12px;
  gap: 10px;
  min-height: 0;
}
.btn-primary {
  background: var(--accent-strong);
  color: #fff;
  border-radius: 8px;
  padding: 9px 12px;
  font-size: 14px;
  transition: filter 0.15s;
}
.btn-primary:hover { filter: brightness(1.12); }
.btn-ghost {
  background: transparent;
  color: var(--text-dim);
  border-radius: 8px;
  padding: 7px 10px;
  font-size: 13px;
}
.btn-ghost:hover { background: var(--bg-hover); color: var(--text); }
.btn-danger { background: var(--danger); color: #fff; border-radius: 8px; padding: 9px 12px; }
.full { width: 100%; }
.session-list { flex: 1; overflow-y: auto; display: flex; flex-direction: column; gap: 4px; }
.session-item {
  padding: 9px 10px;
  border-radius: 8px;
  cursor: pointer;
  color: var(--text-dim);
  display: flex;
  align-items: center;
  gap: 6px;
  white-space: nowrap;
}
.session-item:hover { background: var(--bg-hover); }
.session-item.active { background: var(--bg-elevated); color: var(--text); }
.session-item .s-title { flex: 1; overflow: hidden; text-overflow: ellipsis; }
.session-item .s-actions { display: none; gap: 2px; }
.session-item:hover .s-actions { display: flex; }
.s-actions button {
  background: transparent;
  color: var(--text-dim);
  font-size: 11px;
  padding: 2px 5px;
  border-radius: 4px;
}
.s-actions button:hover { background: var(--bg-hover); color: var(--text); }
.s-actions .s-del:hover { color: var(--danger); }
.side-foot { display: flex; flex-direction: column; gap: 8px; }
.persona-row { display: flex; gap: 6px; overflow-x: auto; padding-bottom: 2px; }
.persona-chip {
  display: flex; align-items: center; gap: 5px;
  background: var(--bg-elevated);
  border: 1px solid var(--border);
  border-radius: 999px;
  padding: 4px 10px;
  font-size: 12px;
  color: var(--text-dim);
  white-space: nowrap;
  cursor: pointer;
}
.persona-chip:hover { border-color: var(--accent); }
.persona-chip.active { border-color: var(--accent); color: var(--accent); }
.persona-chip .chip-edit { font-size: 10px; opacity: 0.6; }

/* ============ 聊天区 ============ */
#chat { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
#chat-head {
  height: 52px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 20px;
  border-bottom: 1px solid var(--border);
  background: var(--bg-panel);
}
#chat-title { font-weight: 600; }
#messages { flex: 1; overflow-y: auto; padding: 20px 24px; display: flex; flex-direction: column; gap: 14px; }
#messages::-webkit-scrollbar, .session-list::-webkit-scrollbar, textarea::-webkit-scrollbar { width: 8px; }
#messages::-webkit-scrollbar-thumb, .session-list::-webkit-scrollbar-thumb, textarea::-webkit-scrollbar-thumb {
  background: var(--scrollbar);
  border-radius: 4px;
}
.msg { display: flex; flex-direction: column; max-width: 76%; }
.msg.user { align-self: flex-end; align-items: flex-end; }
.msg.assistant { align-self: flex-start; align-items: flex-start; }
.msg .who { font-size: 11px; color: var(--text-dim); margin-bottom: 3px; }
.bubble {
  padding: 10px 14px;
  border-radius: 14px;
  line-height: 1.65;
  word-break: break-word;
  box-shadow: var(--shadow);
  min-width: 48px;
}
.msg.user .bubble { background: var(--bubble-user); color: var(--bubble-user-text); border-bottom-right-radius: 4px; }
.msg.assistant .bubble { background: var(--bubble-ai); color: var(--bubble-ai-text); border-bottom-left-radius: 4px; }
.msg-meta { display: flex; gap: 6px; margin-top: 4px; font-size: 11px; color: var(--text-dim); opacity: 0; transition: opacity 0.15s; }
.msg:hover .msg-meta { opacity: 1; }
.msg-meta button {
  background: transparent; color: var(--text-dim);
  font-size: 11px; padding: 1px 6px; border-radius: 4px;
}
.msg-meta button:hover { color: var(--text); background: var(--bg-hover); }
.msg-meta .m-del:hover { color: var(--danger); }
.msg .err-note { color: var(--danger); font-size: 12px; margin-top: 4px; }

/* 打字机光标 */
.typing::after {
  content: "▍";
  color: var(--accent);
  animation: blink 0.9s steps(1) infinite;
}
@keyframes blink { 50% { opacity: 0; } }

/* Markdown 内容 */
.msg-content pre {
  background: rgba(0, 0, 0, 0.28);
  border-radius: 8px;
  padding: 12px;
  overflow-x: auto;
  margin: 8px 0;
}
.msg-content code {
  font-family: Consolas, "Cascadia Code", monospace;
  font-size: 13px;
}
.msg-content p { margin: 4px 0; }
.msg-content ul, .msg-content ol { padding-left: 20px; margin: 4px 0; }
.msg-content blockquote { border-left: 3px solid var(--accent); padding-left: 10px; color: var(--text-dim); margin: 6px 0; }
.msg.user .msg-content code { color: #fff; }

/* ============ 输入区 ============ */
#composer { padding: 12px 20px 16px; background: var(--bg-panel); border-top: 1px solid var(--border); }
#composer textarea {
  width: 100%;
  resize: none;
  border-radius: 12px;
  padding: 10px 12px;
  line-height: 1.5;
}
.composer-bar { display: flex; align-items: center; justify-content: flex-end; gap: 10px; margin-top: 8px; }
.hint { font-size: 12px; color: var(--accent); }

/* ============ 弹窗 / 横幅 / 提示 ============ */
.modal {
  position: fixed; inset: 0;
  background: rgba(0, 0, 0, 0.5);
  display: flex; align-items: center; justify-content: center;
  z-index: 50;
}
.modal-box {
  width: 440px; max-height: 82vh; overflow-y: auto;
  background: var(--bg-panel);
  border: 1px solid var(--border);
  border-radius: 14px;
  padding: 22px;
  display: flex; flex-direction: column; gap: 6px;
  box-shadow: var(--shadow);
}
.modal-box h3 { margin-bottom: 10px; }
.modal-box label { font-size: 12px; color: var(--text-dim); margin-top: 8px; }
.modal-box .chk { display: flex; align-items: center; gap: 6px; font-size: 13px; color: var(--text); }
.modal-actions { display: flex; justify-content: flex-end; gap: 8px; margin-top: 16px; }
.banner {
  position: fixed; top: 48px; left: 50%; transform: translateX(-50%);
  background: var(--danger); color: #fff;
  padding: 8px 18px; border-radius: 999px;
  font-size: 13px; z-index: 60;
  box-shadow: var(--shadow);
}
.toast {
  position: fixed; bottom: 110px; left: 50%; transform: translateX(-50%);
  background: var(--bg-elevated); color: var(--text);
  border: 1px solid var(--border);
  padding: 9px 18px; border-radius: 10px;
  font-size: 13px; z-index: 60;
  box-shadow: var(--shadow);
}
.empty-tip { color: var(--text-dim); text-align: center; margin-top: 32px; font-size: 13px; }
```

- [ ] **Step 4: 写 js/app.js 占位 stub（Task 10 全量替换）**

```js
// 占位：Task 10 实现完整逻辑
console.log('app.js stub');
```

- [ ] **Step 5: 验证页面布局（后端静态托管 + 浏览器打开）**

```bash
LLM_MOCK=1 ./backend/build/Release/backend.exe --data data_e2e --static src/AiCompanion.App/wwwroot &
sleep 1
start http://127.0.0.1:8765/
# 人工检查: 左侧会话栏、右侧聊天区、标题栏、输入框均按主题渲染；
# 打开设置弹窗（按钮暂无逻辑，直接改 html data-theme 属性切换 4 套主题目测配色）
```

检查完成后 `kill %1`。

- [ ] **Step 6: Commit**

```bash
git add src/AiCompanion.App/wwwroot/
git commit -m "feat: 前端骨架与多主题样式（dark/light/sakura/midnight）"
```

---

### Task 10: 前端逻辑 app.js（会话/人设/设置/SSE 流式/打字机/Markdown/消息操作/桥接）

**Files:**
- Modify: `src/AiCompanion.App/wwwroot/js/app.js`（全量实现）

**Interfaces:**
- Consumes: Task 9 的 DOM id 约定、`window.__API_BASE__`；后端 API（规格 §4）；vendor 库 `marked` / `hljs` / `DOMPurify`
- Produces:
  - `App.registerCommand(name, fn)` 命令注册表 + `App.on/emit` 事件 + `App.api.get/post/patch/del`
  - 命令清单（插件扩展点）：`boot`、`backend-status`、`new-session`、`select-session`、`rename-session`、`delete-session`、`send-message`、`regenerate`、`stop-stream`、`copy-message`、`delete-message`、`clear-session`、`export-session`、`activate-persona`、`open-persona-editor`、`save-persona`、`delete-persona`、`open-settings`、`save-settings`

- [ ] **Step 1: 写 app.js（完整实现）**

```js
'use strict';

const API_BASE = window.__API_BASE__ || 'http://127.0.0.1:8765';

/* ================= 核心对象：命令注册表 + 事件 + API ================= */
const App = {
  state: {
    sessions: [],
    currentSessionId: null,
    personas: [],
    activePersonaId: null,
    config: null,
    streaming: false,
    abortCtrl: null,
  },
  commands: new Map(),
  listeners: {},

  registerCommand(name, fn) { this.commands.set(name, fn); },
  run(name, ...args) {
    const fn = this.commands.get(name);
    if (fn) { try { return fn(...args); } catch (e) { console.error('[cmd]', name, e); toast(e.message || String(e)); } }
    else console.warn('unknown command:', name);
  },
  on(type, fn) { (this.listeners[type] ||= []).push(fn); },
  emit(type, data) { (this.listeners[type] || []).forEach(fn => { try { fn(data); } catch (e) { console.error(e); } }); },

  api: {
    async req(method, path, body) {
      const opts = { method, headers: {} };
      if (body !== undefined) {
        opts.headers['Content-Type'] = 'application/json';
        opts.body = JSON.stringify(body);
      }
      const res = await fetch(API_BASE + path, opts);
      if (!res.ok) {
        let msg = 'HTTP ' + res.status;
        try { msg = (await res.json()).message || msg; } catch (_) {}
        throw new Error(msg);
      }
      const j = await res.json();
      if (j.code !== 0) throw new Error(j.message || '请求失败');
      return j.data;
    },
    get: (p) => App.api.req('GET', p),
    post: (p, b) => App.api.req('POST', p, b),
    patch: (p, b) => App.api.req('PATCH', p, b),
    del: (p) => App.api.req('DELETE', p),
  },
};

/* ================= DOM 与工具 ================= */
const $ = (id) => document.getElementById(id);
const messagesEl = $('messages');
const inputEl = $('input');
const sessionListEl = $('session-list');
const personaRowEl = $('persona-switcher');

function escapeHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
          .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}
let toastTimer = null;
function toast(msg) {
  const t = $('toast');
  t.textContent = msg;
  t.classList.remove('hidden');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.add('hidden'), 2600);
}
function setBanner(msg) {
  const b = $('banner');
  if (!msg) { b.classList.add('hidden'); return; }
  b.textContent = msg;
  b.classList.remove('hidden');
}
function renderMarkdown(text) {
  const raw = marked.parse(text, { breaks: true });
  const clean = DOMPurify.sanitize(raw);
  const div = document.createElement('div');
  div.innerHTML = clean;
  div.querySelectorAll('pre code').forEach(block => {
    try { hljs.highlightElement(block); } catch (_) {}
  });
  return div.innerHTML;
}
function applyTheme(theme) {
  document.documentElement.dataset.theme = theme || 'dark';
}
function nearBottom() {
  return messagesEl.scrollHeight - messagesEl.scrollTop - messagesEl.clientHeight < 90;
}
function scrollToBottom(force = false) {
  if (force || nearBottom()) messagesEl.scrollTop = messagesEl.scrollHeight;
}

/* ================= 打字机 ================= */
class Typewriter {
  constructor(el, onDrain) {
    this.el = el;
    this.onDrain = onDrain || null;
    this.buf = '';
    this.timer = null;
    this.finished = false;
    this.tick = this.tick.bind(this);
  }
  push(text) {
    this.buf += text;
    if (!this.timer) this.timer = setInterval(this.tick, 24);
  }
  tick() {
    if (!this.buf) {
      if (this.finished && this.timer) {
        clearInterval(this.timer);
        this.timer = null;
        if (this.onDrain) this.onDrain();
      }
      return;
    }
    const n = Math.min(4, this.buf.length);
    this.el.textContent += this.buf.slice(0, n);
    this.buf = this.buf.slice(n);
  }
  finish() { // 标记完成；缓冲全部排空后回调 onDrain
    this.finished = true;
    if (!this.timer) {
      this.el.textContent += this.buf;
      this.buf = '';
      if (this.onDrain) this.onDrain();
    }
  }
}

/* ================= 消息气泡 ================= */
function addBubble(role, text, { streaming = false, id = null } = {}) {
  const wrap = document.createElement('div');
  wrap.className = 'msg ' + role;
  wrap.dataset.msgId = id || '';

  const who = document.createElement('div');
  who.className = 'who';
  who.textContent = role === 'user' ? '我' : (App.state.personas.find(p => p.id === App.state.activePersonaId)?.name || 'AI');

  const bubble = document.createElement('div');
  bubble.className = 'bubble';
  const content = document.createElement('div');
  content.className = 'msg-content' + (streaming ? ' typing' : '');
  if (!streaming) content.innerHTML = renderMarkdown(text);
  bubble.appendChild(content);
  wrap.appendChild(who);
  wrap.appendChild(bubble);

  const meta = document.createElement('div');
  meta.className = 'msg-meta';
  meta.innerHTML =
    '<span class="m-time">' + new Date().toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit' }) + '</span>' +
    '<button class="m-copy" data-action="copy">复制</button>' +
    (role === 'assistant' && !streaming ? '<button class="m-regen" data-action="regenerate">重新生成</button>' : '') +
    '<button class="m-del" data-action="delete">删除</button>';
  wrap.appendChild(meta);

  const empty = messagesEl.querySelector('.empty-tip');
  if (empty) empty.remove();
  messagesEl.appendChild(wrap);
  scrollToBottom(true);
  return {
    root: wrap,
    contentEl: content,
    finalize(fullText) {
      content.classList.remove('typing');
      content.innerHTML = renderMarkdown(fullText);
      wrap.querySelector('.m-regen')?.remove();
      const regen = document.createElement('button');
      regen.className = 'm-regen';
      regen.dataset.action = 'regenerate';
      regen.textContent = '重新生成';
      meta.insertBefore(regen, meta.querySelector('.m-del'));
    },
    showError(msg) {
      content.classList.remove('typing');
      const note = document.createElement('div');
      note.className = 'err-note';
      note.textContent = '⚠ ' + msg;
      wrap.appendChild(note);
    },
  };
}

/* ================= SSE 消费 ================= */
async function consumeSse(path, payload, { onDelta, onDone, onError }) {
  App.state.abortCtrl = new AbortController();
  let res;
  try {
    res = await fetch(API_BASE + path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
      signal: App.state.abortCtrl.signal,
    });
  } catch (e) {
    if (e.name === 'AbortError') return;
    throw new Error('无法连接后端服务');
  }
  if (!res.ok || !res.body) {
    let msg = 'HTTP ' + res.status;
    try { msg = (await res.json()).message || msg; } catch (_) {}
    throw new Error(msg);
  }
  const reader = res.body.getReader();
  const dec = new TextDecoder();
  let buf = '';
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    buf += dec.decode(value, { stream: true });
    let idx;
    while ((idx = buf.indexOf('\n\n')) >= 0) {
      const block = buf.slice(0, idx);
      buf = buf.slice(idx + 2);
      let ev = 'message', data = '';
      for (const line of block.split('\n')) {
        const t = line.replace(/\r$/, '');
        if (t.startsWith('event:')) ev = t.slice(6).trim();
        else if (t.startsWith('data:')) data += (data ? '\n' : '') + t.slice(5).trim();
      }
      if (!data) continue;
      let j;
      try { j = JSON.parse(data); } catch (_) { continue; }
      if (ev === 'delta') onDelta(j.content || '');
      else if (ev === 'done') onDone(j);
      else if (ev === 'error') onError(j);
    }
  }
}

function setStreamingUI(on) {
  App.state.streaming = on;
  $('btn-send').classList.toggle('hidden', on);
  $('btn-stop').classList.toggle('hidden', !on);
  $('stream-hint').classList.toggle('hidden', !on);
}

/* ================= 会话管理 ================= */
async function refreshSessions(silent = false) {
  App.state.sessions = await App.api.get('/api/v1/sessions');
  if (!App.state.sessions.length) App.state.currentSessionId = null;
  else if (!App.state.sessions.some(s => s.id === App.state.currentSessionId))
    App.state.currentSessionId = App.state.sessions[0].id;
  renderSessions();
  if (!silent && App.state.currentSessionId) loadMessages(App.state.currentSessionId);
}

function renderSessions() {
  sessionListEl.innerHTML = '';
  if (!App.state.sessions.length) {
    sessionListEl.innerHTML = '<div class="empty-tip">还没有会话<br>点击"＋ 新对话"开始</div>';
    return;
  }
  for (const s of App.state.sessions) {
    const item = document.createElement('div');
    item.className = 'session-item' + (s.id === App.state.currentSessionId ? ' active' : '');
    item.innerHTML =
      '<span class="s-title"></span>' +
      '<span class="s-actions">' +
      '<button data-sact="rename" title="重命名">✎</button>' +
      '<button class="s-del" data-sact="delete" title="删除">✕</button>' +
      '</span>';
    item.querySelector('.s-title').textContent = s.title;
    item.addEventListener('click', (e) => {
      if (e.target.closest('.s-actions')) return;
      App.run('select-session', s.id);
    });
    item.querySelector('[data-sact="rename"]').addEventListener('click', () => App.run('rename-session', s.id));
    item.querySelector('[data-sact="delete"]').addEventListener('click', () => App.run('delete-session', s.id));
    sessionListEl.appendChild(item);
  }
  $('chat-title').textContent =
    App.state.sessions.find(s => s.id === App.state.currentSessionId)?.title || '新对话';
}

async function loadMessages(sessionId) {
  const msgs = await App.api.get(`/api/v1/messages?session_id=${sessionId}&limit=200`);
  messagesEl.innerHTML = '';
  if (!msgs.length) {
    messagesEl.innerHTML = '<div class="empty-tip">开始新的对话吧 ✦</div>';
    return;
  }
  for (const m of msgs) addBubble(m.role, m.content, { id: m.id });
  scrollToBottom(true);
}

/* ================= 人设 ================= */
async function refreshPersonas() {
  App.state.personas = await App.api.get('/api/v1/personas');
  App.state.activePersonaId = App.state.personas.find(p => p.is_active)?.id || null;
  renderPersonas();
}
function renderPersonas() {
  personaRowEl.innerHTML = '';
  for (const p of App.state.personas) {
    const chip = document.createElement('button');
    chip.className = 'persona-chip' + (p.id === App.state.activePersonaId ? ' active' : '');
    chip.innerHTML =
      '<span>' + escapeHtml(p.avatar) + '</span><span>' + escapeHtml(p.name) + '</span>' +
      '<span class="chip-edit" data-pact="edit">✎</span>' +
      (p.id === App.state.activePersonaId ? '' : '<span class="chip-edit" data-pact="del">✕</span>');
    chip.addEventListener('click', (e) => {
      if (e.target.closest('[data-pact="edit"]')) { App.run('open-persona-editor', p); return; }
      if (e.target.closest('[data-pact="del"]')) { App.run('delete-persona', p.id); return; }
      App.run('activate-persona', p.id);
    });
    personaRowEl.appendChild(chip);
  }
  const add = document.createElement('button');
  add.className = 'persona-chip';
  add.textContent = '＋ 新人设';
  add.addEventListener('click', () => App.run('open-persona-editor', null));
  personaRowEl.appendChild(add);
}

/* ================= 命令实现 ================= */
App.registerCommand('boot', async () => {
  try {
    App.state.config = await App.api.get('/api/v1/config');
  } catch (_) {
    App.state.config = { theme: 'dark' };
    setBanner('后端服务未连接，请重新打开程序');
    return;
  }
  applyTheme(App.state.config.theme);
  await Promise.all([refreshPersonas(), refreshSessions()]);
  const last = localStorage.getItem('last_session');
  if (last && App.state.sessions.some(s => String(s.id) === last)) {
    App.state.currentSessionId = Number(last);
    renderSessions();
    loadMessages(App.state.currentSessionId);
  }
});

App.registerCommand('backend-status', (payload) => {
  if (payload.status === 'down') setBanner('后端连接中断：' + (payload.detail || '正在重试…'));
  else setBanner('');
});

App.registerCommand('new-session', async () => {
  const s = await App.api.post('/api/v1/sessions', {});
  App.state.currentSessionId = s.id;
  localStorage.setItem('last_session', String(s.id));
  await refreshSessions();
  loadMessages(s.id);
});

App.registerCommand('select-session', async (id) => {
  App.state.currentSessionId = id;
  localStorage.setItem('last_session', String(id));
  renderSessions();
  await loadMessages(id);
});

App.registerCommand('rename-session', async (id) => {
  const cur = App.state.sessions.find(s => s.id === id);
  const title = prompt('会话名称：', cur?.title || '');
  if (title === null || !title.trim()) return;
  await App.api.patch(`/api/v1/sessions/${id}`, { title: title.trim() });
  await refreshSessions();
});

App.registerCommand('delete-session', async (id) => {
  if (!confirm('删除该会话及其全部消息？')) return;
  await App.api.del(`/api/v1/sessions/${id}`);
  await refreshSessions();
  if (App.state.currentSessionId) loadMessages(App.state.currentSessionId);
  else { messagesEl.innerHTML = '<div class="empty-tip">开始新的对话吧 ✦</div>'; }
});

App.registerCommand('send-message', async () => {
  if (App.state.streaming) return;
  const text = inputEl.value.trim();
  if (!text) return;
  const sid = App.state.currentSessionId;
  if (!sid) { toast('请先创建会话'); return; }
  if (!App.state.config || !App.state.config.model || !App.state.config.base_url) {
    toast('请先在设置中填写 API 配置');
    App.run('open-settings');
    return;
  }
  inputEl.value = '';
  addBubble('user', text);
  setStreamingUI(true);
  const bubble = addBubble('assistant', '', { streaming: true });
  const tw = new Typewriter(bubble.contentEl, () => { bubble.finalize(full); scrollToBottom(); });
  let full = '';
  let errored = false;
  try {
    await consumeSse('/api/v1/chat/stream', { session_id: sid, content: text }, {
      onDelta: (d) => { full += d; tw.push(d); scrollToBottom(); },
      onDone: () => {
        tw.finish();
        App.run('refresh-after-chat');
      },
      onError: (e) => {
        errored = true;
        tw.finish();
        bubble.showError(e.message || '生成失败');
        App.run('refresh-after-chat');
      },
    });
  } catch (e) {
    if (!errored) {
      tw.finish();
      bubble.showError(e.message);
      App.run('refresh-after-chat');
    }
  }
});

App.registerCommand('refresh-after-chat', async () => {
  setStreamingUI(false);
  await refreshSessions(true);
  if (App.state.currentSessionId) loadMessages(App.state.currentSessionId);
});

App.registerCommand('stop-stream', async () => {
  if (!App.state.streaming) return;
  const sid = App.state.currentSessionId;
  try { App.state.abortCtrl?.abort(); } catch (_) {}
  if (sid) App.api.post('/api/v1/chat/stop', { session_id: sid }).catch(() => {});
  App.run('refresh-after-chat');
});

App.registerCommand('regenerate', async () => {
  if (App.state.streaming) { toast('正在生成中…'); return; }
  const sid = App.state.currentSessionId;
  if (!sid) return;
  setStreamingUI(true);
  const bubble = addBubble('assistant', '', { streaming: true });
  const tw = new Typewriter(bubble.contentEl, () => { bubble.finalize(full); scrollToBottom(); });
  let full = '';
  try {
    await consumeSse('/api/v1/chat/regenerate', { session_id: sid }, {
      onDelta: (d) => { full += d; tw.push(d); scrollToBottom(); },
      onDone: () => { tw.finish(); App.run('refresh-after-chat'); },
      onError: (e) => { tw.finish(); bubble.showError(e.message || '生成失败'); App.run('refresh-after-chat'); },
    });
  } catch (e) {
    tw.finish();
    bubble.showError(e.message);
    App.run('refresh-after-chat');
  }
});

App.registerCommand('copy-message', async (msgId) => {
  const el = messagesEl.querySelector(`.msg[data-msg-id="${msgId}"] .msg-content`);
  if (!el) return;
  try {
    await navigator.clipboard.writeText(el.innerText);
    toast('已复制');
  } catch (_) {
    toast('复制失败');
  }
});

App.registerCommand('delete-message', async (msgId) => {
  if (!confirm('删除这条消息？')) return;
  await App.api.del(`/api/v1/messages/${msgId}`);
  messagesEl.querySelector(`.msg[data-msg-id="${msgId}"]`)?.remove();
  toast('已删除');
});

App.registerCommand('clear-session', async () => {
  const sid = App.state.currentSessionId;
  if (!sid) return;
  if (!confirm('清空当前会话的全部消息？')) return;
  await App.api.del(`/api/v1/messages?session_id=${sid}`);
  messagesEl.innerHTML = '<div class="empty-tip">开始新的对话吧 ✦</div>';
});

App.registerCommand('export-session', async () => {
  const sid = App.state.currentSessionId;
  if (!sid) { toast('请先选择会话'); return; }
  const fmt = prompt('导出格式：md / json / txt', 'md');
  if (!['md', 'json', 'txt'].includes(fmt)) return;
  const res = await fetch(`${API_BASE}/api/v1/export?session_id=${sid}&format=${fmt}`, { method: 'POST' });
  if (!res.ok) { toast('导出失败'); return; }
  const blob = await res.blob();
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = `chat_${sid}.${fmt}`;
  a.click();
  URL.revokeObjectURL(a.href);
});

App.registerCommand('activate-persona', async (id) => {
  const sid = App.state.currentSessionId;
  const q = sid ? `?session_id=${sid}` : '';
  await App.api.post(`/api/v1/personas/${id}/activate${q}`);
  await refreshPersonas();
  if (sid) loadMessages(sid);
});

App.registerCommand('open-persona-editor', (p) => {
  $('persona-modal-title').textContent = p ? '编辑人设' : '新建人设';
  $('pe-name').value = p?.name || '';
  $('pe-avatar').value = p?.avatar || '🌸';
  $('pe-greeting').value = p?.greeting || '';
  $('pe-prompt').value = p?.system_prompt || '';
  $('pe-temp').value = p?.temperature ?? 0.8;
  $('modal-persona').classList.remove('hidden');
  $('modal-persona').dataset.personaId = p?.id || '';
});

App.registerCommand('save-persona', async () => {
  const name = $('pe-name').value.trim();
  const promptText = $('pe-prompt').value.trim();
  if (!name || !promptText) { toast('名称与 System Prompt 不能为空'); return; }
  const body = {
    name,
    avatar: $('pe-avatar').value.trim() || '🌸',
    greeting: $('pe-greeting').value.trim(),
    system_prompt: promptText,
    temperature: Number($('pe-temp').value) || 0.8,
  };
  const id = $('modal-persona').dataset.personaId;
  if (id) body.id = Number(id);
  await App.api.post('/api/v1/personas', body);
  $('modal-persona').classList.add('hidden');
  await refreshPersonas();
  toast('人设已保存');
});

App.registerCommand('delete-persona', async (id) => {
  if (!confirm('删除这个人设？')) return;
  try {
    await App.api.del(`/api/v1/personas/${id}`);
  } catch (e) {
    toast(e.message);
    return;
  }
  await refreshPersonas();
});

App.registerCommand('open-settings', () => {
  const c = App.state.config || {};
  $('set-base-url').value = c.base_url || '';
  $('set-api-key').value = '';
  $('set-api-key').placeholder = c.api_key ? `当前 Key：${c.api_key}（留空保持不变）` : 'sk-...';
  $('set-model').value = c.model || '';
  $('set-keep-rounds').value = c.keep_rounds ?? 20;
  $('set-theme').value = c.theme || 'dark';
  $('set-proxy-enabled').checked = c.proxy_enabled === '1' || c.proxy_enabled === true;
  $('set-proxy-host').value = c.proxy_host || '';
  $('set-proxy-port').value = c.proxy_port && c.proxy_port !== '0' ? c.proxy_port : '';
  $('modal-settings').classList.remove('hidden');
});

App.registerCommand('save-settings', async () => {
  const body = {
    base_url: $('set-base-url').value.trim(),
    api_key: $('set-api-key').value.trim(),
    model: $('set-model').value.trim(),
    keep_rounds: Number($('set-keep-rounds').value) || 0,
    theme: $('set-theme').value,
    proxy_enabled: $('set-proxy-enabled').checked,
    proxy_host: $('set-proxy-host').value.trim(),
    proxy_port: Number($('set-proxy-port').value) || 0,
  };
  await App.api.post('/api/v1/config', body);
  App.state.config = await App.api.get('/api/v1/config');
  applyTheme(App.state.config.theme);
  $('modal-settings').classList.add('hidden');
  toast('设置已保存');
});

/* ================= 事件绑定 ================= */
document.addEventListener('DOMContentLoaded', () => App.run('boot'));

$('btn-send').addEventListener('click', () => App.run('send-message'));
$('btn-stop').addEventListener('click', () => App.run('stop-stream'));
$('btn-new-session').addEventListener('click', () => App.run('new-session'));
$('btn-settings').addEventListener('click', () => App.run('open-settings'));
$('btn-export').addEventListener('click', () => App.run('export-session'));
$('btn-clear').addEventListener('click', () => App.run('clear-session'));
$('btn-settings-save').addEventListener('click', () => App.run('save-settings'));
$('btn-settings-cancel').addEventListener('click', () => $('modal-settings').classList.add('hidden'));
$('btn-persona-save').addEventListener('click', () => App.run('save-persona'));
$('btn-persona-cancel').addEventListener('click', () => $('modal-persona').classList.add('hidden'));

inputEl.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
    e.preventDefault();
    App.run('send-message');
  }
});

messagesEl.addEventListener('click', (e) => {
  const btn = e.target.closest('[data-action]');
  if (!btn) return;
  const msgEl = btn.closest('.msg');
  const msgId = msgEl?.dataset.msgId;
  if (btn.dataset.action === 'copy' && msgId) App.run('copy-message', msgId);
  else if (btn.dataset.action === 'delete' && msgId) App.run('delete-message', msgId);
  else if (btn.dataset.action === 'regenerate') App.run('regenerate');
});

/* WPF 桥接：窗口按钮 + 后端状态 */
const wv = window.chrome && window.chrome.webview;
if (wv) {
  $('btn-min').addEventListener('click', () => wv.postMessage({ type: 'window', payload: { action: 'minimize' } }));
  $('btn-max').addEventListener('click', () => wv.postMessage({ type: 'window', payload: { action: 'maximize' } }));
  $('btn-close').addEventListener('click', () => wv.postMessage({ type: 'window', payload: { action: 'close' } }));
  const tb = $('titlebar');
  tb.addEventListener('mousedown', (e) => {
    if (e.target.closest('.tb-actions')) return;
    if (e.detail === 2) wv.postMessage({ type: 'window', payload: { action: 'maximize' } });
    else wv.postMessage({ type: 'window', payload: { action: 'drag' } });
  });
  wv.addEventListener('message', (e) => {
    try {
      const m = typeof e.data === 'string' ? JSON.parse(e.data) : e.data;
      if (m.type === 'backend-status') App.run('backend-status', m.payload);
    } catch (_) {}
  });
}
```

- [ ] **Step 2: Mock 模式端到端验证（浏览器）**

```bash
cmake --build backend/build --config Release
LLM_MOCK=1 ./backend/build/Release/backend.exe --data data_e2e --static src/AiCompanion.App/wwwroot &
sleep 1
start http://127.0.0.1:8765/
```

人工清单（全部通过才算完成）：
1. 页面加载即显示会话列表与人设 chip（默认人设激活态）
2. "＋ 新对话" → 出现会话，发送"你好" → user 气泡立即出现，AI 气泡打字机逐字出现，完成后渲染 Markdown（mock 回复含 `**加粗**` 与代码块，代码块应有高亮）
3. 发送第二条消息，自动滚动到底部
4. AI 气泡悬停 → 复制 / 重新生成 / 删除 可用；重新生成后仅一条 assistant 消息
5. 设置面板：修改主题 → 保存后全局配色立即切换（4 套主题逐一验证）；keep_rounds、代理项可保存并回显
6. 新建人设（含开场白）→ 激活 → 空会话自动出现开场白气泡
7. 导出 md → 下载文件内容包含对话
8. 清空会话 → 消息区回到空态；删除会话 → 列表更新

检查完成 `kill %1`。

- [ ] **Step 3: Commit**

```bash
git add src/AiCompanion.App/wwwroot/js/app.js
git commit -m "feat: 前端聊天逻辑（SSE 流式/打字机/Markdown/会话人设设置/桥接）"
```

---

### Task 11: WPF 壳（WebView2 初始化 + 后端进程守护 + 无边框窗口）

**Files:**
- Create: `src/AiCompanion.App/AiCompanion.App.csproj`
- Create: `src/AiCompanion.App/App.xaml`, `src/AiCompanion.App/App.xaml.cs`
- Create: `src/AiCompanion.App/MainWindow.xaml`, `src/AiCompanion.App/MainWindow.xaml.cs`
- Create: `src/AiCompanion.App/BackendProcess.cs`
- Create: `AiCompanion.sln`

**Interfaces:**
- Consumes: Task 9/10 的 wwwroot、`window.__API_BASE__` 注入约定、后端 `--data` 参数与 `/health`
- Produces:
  - `BackendProcess(string exePath, string dataDir, int apiPort)`：`StartAsync()`（缺失 exe 时仅等待外部后端就绪）、`Dispose()`、事件 `Action<string,string> StatusChanged`（status ∈ starting/up/down）
  - WebView2 消息协议：JS→WPF `{type:"window", payload:{action:"minimize|maximize|close|drag"}}`；WPF→JS `{type:"backend-status", payload:{status:"up|down", detail}}`
  - 环境变量覆盖（开发调试用）：`AI_COMPANION_DATA`（数据目录）、`AI_COMPANION_BACKEND`（backend.exe 路径）

- [ ] **Step 1: 写 AiCompanion.App.csproj**

```xml
<Project Sdk="Microsoft.NET.Sdk">

  <PropertyGroup>
    <OutputType>WinExe</OutputType>
    <TargetFramework>net8.0-windows</TargetFramework>
    <Nullable>enable</Nullable>
    <ImplicitUsings>enable</ImplicitUsings>
    <UseWPF>true</UseWPF>
    <AssemblyName>AiCompanion</AssemblyName>
    <RootNamespace>AiCompanion.App</RootNamespace>
  </PropertyGroup>

  <ItemGroup>
    <PackageReference Include="Microsoft.Web.WebView2" Version="1.0.2903.40" />
  </ItemGroup>

  <ItemGroup>
    <Content Include="wwwroot\**\*" CopyToOutputDirectory="PreserveNewest" />
  </ItemGroup>

</Project>
```

- [ ] **Step 2: 写 App.xaml 与 App.xaml.cs**

```xml
<Application x:Class="AiCompanion.App.App"
             xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
             xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
             StartupUri="MainWindow.xaml">
</Application>
```

```csharp
using System.Windows;

namespace AiCompanion.App;

public partial class App : Application
{
}
```

- [ ] **Step 3: 写 BackendProcess.cs**

```csharp
using System.Diagnostics;
using System.Net.Http;

namespace AiCompanion.App;

/// <summary>后端进程守护：拉起、健康检查、崩溃自动重启（≤3 次，1s/2s/4s 退避）、退出清理。</summary>
public sealed class BackendProcess : IDisposable
{
    public event Action<string, string>? StatusChanged;

    private readonly string _exePath;
    private readonly string _dataDir;
    private Process? _proc;
    private CancellationTokenSource? _cts;

    public BackendProcess(string exePath, string dataDir, int apiPort)
    {
        _exePath = exePath;
        _dataDir = dataDir;
        ApiPort = apiPort;
    }

    public int ApiPort { get; }

    public async Task StartAsync(CancellationToken ct = default)
    {
        _cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        var token = _cts.Token;
        if (!File.Exists(_exePath))
        {
            // 开发模式：backend.exe 未随程序输出，假定由外部（run_dev.cmd）启动
            await WaitHealthyAsync(TimeSpan.FromSeconds(15), token);
            return;
        }
        _ = Task.Run(() => RunLoopAsync(token));
        await WaitHealthyAsync(TimeSpan.FromSeconds(15), token);
    }

    private async Task RunLoopAsync(CancellationToken ct)
    {
        int restarts = 0;
        while (!ct.IsCancellationRequested)
        {
            StatusChanged?.Invoke("starting", "");
            try
            {
                var psi = new ProcessStartInfo
                {
                    FileName = _exePath,
                    Arguments = $"--data \"{_dataDir}\"",
                    UseShellExecute = false,
                    CreateNoWindow = true,
                };
                _proc = Process.Start(psi);
                if (_proc is null)
                {
                    StatusChanged?.Invoke("down", "无法启动后端进程");
                    return;
                }
                await _proc.WaitForExitAsync(ct);
            }
            catch (OperationCanceledException)
            {
                Kill();
                return;
            }
            if (ct.IsCancellationRequested) return;

            if (restarts >= 3)
            {
                StatusChanged?.Invoke("down", "后端多次崩溃，请检查 logs 后手动重启程序");
                return;
            }
            restarts++;
            StatusChanged?.Invoke("down", $"后端异常退出，正在重启（{restarts}/3）…");
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(Math.Pow(2, restarts - 1)), ct);
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }

    private async Task WaitHealthyAsync(TimeSpan timeout, CancellationToken ct)
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(2) };
        var sw = Stopwatch.StartNew();
        while (sw.Elapsed < timeout && !ct.IsCancellationRequested)
        {
            try
            {
                var resp = await http.GetAsync($"http://127.0.0.1:{ApiPort}/health", ct);
                if (resp.IsSuccessStatusCode)
                {
                    StatusChanged?.Invoke("up", "");
                    return;
                }
            }
            catch
            {
                // 后端尚未就绪，继续轮询
            }
            await Task.Delay(300, ct);
        }
        throw new TimeoutException($"后端 {timeout.TotalSeconds}s 内未就绪");
    }

    private void Kill()
    {
        try { _proc?.Kill(entireProcessTree: true); } catch { }
        _proc = null;
    }

    public void Dispose()
    {
        try { _cts?.Cancel(); } catch { }
        Kill();
        _cts?.Dispose();
    }
}
```

- [ ] **Step 4: 写 MainWindow.xaml**

```xml
<Window x:Class="AiCompanion.App.MainWindow"
        xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        xmlns:wv2="clr-namespace:Microsoft.Web.WebView2.Wpf;assembly=Microsoft.Web.WebView2.Wpf"
        Title="AI 陪伴"
        Height="760" Width="1080" MinHeight="600" MinWidth="860"
        WindowStyle="None" ResizeMode="CanResize"
        Background="#0f1117"
        Loaded="OnLoaded" Closing="OnClosing">
    <Grid>
        <wv2:WebView2 x:Name="WebView" />
    </Grid>
</Window>
```

- [ ] **Step 5: 写 BridgeProtocol.cs 与 MainWindow.xaml.cs**

先写 `BridgeProtocol.cs`（消息协议常量，与规格 §8.3 对应）：

```csharp
namespace AiCompanion.App;

/// <summary>WebView2 双向消息协议常量（新增交互需在此登记）。</summary>
public static class BridgeProtocol
{
    public const string MsgWindow = "window";              // JS → WPF：窗口操作
    public const string MsgBackendStatus = "backend-status"; // WPF → JS：后端状态

    // window 消息的 action 取值
    public const string ActionMinimize = "minimize";
    public const string ActionMaximize = "maximize";
    public const string ActionClose = "close";
    public const string ActionDrag = "drag";
}
```

再写 `MainWindow.xaml.cs`：

```csharp
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using Microsoft.Web.WebView2.Core;

namespace AiCompanion.App;

public partial class MainWindow : Window
{
    private BackendProcess? _backend;

    public MainWindow()
    {
        InitializeComponent();
    }

    private static string WwwrootDir => Path.Combine(AppContext.BaseDirectory, "wwwroot");
    private static string BackendExe =>
        Environment.GetEnvironmentVariable("AI_COMPANION_BACKEND")
        ?? Path.Combine(AppContext.BaseDirectory, "backend", "backend.exe");
    private static string DataDir =>
        Environment.GetEnvironmentVariable("AI_COMPANION_DATA")
        ?? Path.Combine(AppContext.BaseDirectory, "data");

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        EnableRoundedCorners();
        try
        {
            _backend = new BackendProcess(BackendExe, DataDir, ReadApiPort());
            _backend.StatusChanged += OnBackendStatusChanged;
            await _backend.StartAsync();
        }
        catch (Exception ex)
        {
            MessageBox.Show(this,
                "后端启动失败：" + ex.Message + "\n\n请确认 backend.exe 已构建并位于程序目录。",
                "AI 陪伴", MessageBoxButton.OK, MessageBoxImage.Error);
        }
        await InitWebViewAsync();
    }

    private static int ReadApiPort()
    {
        var cfgPath = Path.Combine(DataDir, "config.json");
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(cfgPath));
            var srv = doc.RootElement.GetProperty("server");
            int port = srv.GetProperty("port").GetInt32();
            if (srv.TryGetProperty("actual_port", out var ap) && ap.GetInt32() > 0)
                return ap.GetInt32();
            return port;
        }
        catch
        {
            return 8765;
        }
    }

    private void OnBackendStatusChanged(string status, string detail)
    {
        if (status == "down")
            PostToJs(new { type = BridgeProtocol.MsgBackendStatus, payload = new { status = "down", detail } });
        else if (status == "up")
            PostToJs(new { type = BridgeProtocol.MsgBackendStatus, payload = new { status = "up", detail = "" } });
    }

    private void PostToJs(object msg)
    {
        try
        {
            Dispatcher.Invoke(() =>
                WebView.CoreWebView2?.PostWebMessageAsJson(JsonSerializer.Serialize(msg)));
        }
        catch { }
    }

    private async Task InitWebViewAsync()
    {
        var env = await CoreWebView2Environment.CreateAsync(
            null, Path.Combine(AppContext.BaseDirectory, "WebView2Data"));
        await WebView.EnsureCoreWebView2Async(env);
        WebView.CoreWebView2.SetVirtualHostNameToFolderMapping(
            "app.local", WwwrootDir, CoreWebView2HostResourceAccessKind.Allow);
        int port = _backend?.ApiPort ?? ReadApiPort();
        await WebView.CoreWebView2.AddScriptToExecuteOnDocumentCreatedAsync(
            $"window.__API_BASE__ = 'http://127.0.0.1:{port}';");
        WebView.CoreWebView2.WebMessageReceived += OnWebMessageReceived;
        WebView.CoreWebView2.Navigate("https://app.local/index.html");
    }

    private void OnWebMessageReceived(object? sender, CoreWebView2WebMessageReceivedEventArgs e)
    {
        try
        {
            using var doc = JsonDocument.Parse(e.WebMessageAsJson);
            if (!doc.RootElement.TryGetProperty("type", out var typeEl) ||
                typeEl.GetString() != BridgeProtocol.MsgWindow)
                return;
            var action = doc.RootElement.GetProperty("payload").GetProperty("action").GetString();
            Dispatcher.Invoke(() =>
            {
                switch (action)
                {
                    case BridgeProtocol.ActionMinimize: WindowState = WindowState.Minimized; break;
                    case BridgeProtocol.ActionMaximize: WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized; break;
                    case BridgeProtocol.ActionClose: Close(); break;
                    case BridgeProtocol.ActionDrag:
                        if (Mouse.LeftButton == MouseButtonState.Pressed) DragMove();
                        break;
                }
            });
        }
        catch { }
    }

    private void EnableRoundedCorners()
    {
        try
        {
            var hwnd = new WindowInteropHelper(this).Handle;
            const int dwmwaWindowCornerPreference = 33;
            const int dwmwcpRound = 2;
            int pref = dwmwcpRound;
            _ = DwmSetWindowAttribute(hwnd, dwmwaWindowCornerPreference, ref pref, sizeof(int));
        }
        catch { }
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    private void OnClosing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        _backend?.Dispose();
    }
}
```

- [ ] **Step 6: 创建解决方案 AiCompanion.sln**

```bash
dotnet new sln -n AiCompanion -o .
dotnet sln add src/AiCompanion.App/AiCompanion.App.csproj
```

- [ ] **Step 7: 构建 + 运行验证**

```bash
dotnet build AiCompanion.sln -c Debug
# 布局: 把 backend.exe 拷到 WPF 输出目录供其拉起
mkdir -p src/AiCompanion.App/bin/Debug/net8.0-windows/backend
cp backend/build/Release/backend.exe src/AiCompanion.App/bin/Debug/net8.0-windows/backend/
LLM_MOCK=1 ./src/AiCompanion.App/bin/Debug/net8.0-windows/AiCompanion.exe &
```

人工清单：
1. 窗口无边框、Win11 圆角，可拖拽/最小化/最大化/关闭（HTML 标题栏按钮）
2. 程序启动后自动拉起后端并加载聊天页（无报错弹窗）
3. 发一条消息，流式回复正常
4. `taskkill /im backend.exe /f` → 前端出现"连接中断"横幅 → 后端被自动重启（≤15s）→ 横幅消失
5. 关闭窗口 → backend.exe 进程随之退出（`tasklist | findstr backend` 为空）

- [ ] **Step 8: Commit**

```bash
git add src/AiCompanion.App/ AiCompanion.sln
git commit -m "feat: WPF 壳（WebView2/无边框窗口/后端守护自动重启）"
```

---

### Task 12: 一键脚本、README 与最终验收

**Files:**
- Create: `scripts/build_backend.cmd`
- Create: `scripts/run_dev.cmd`
- Create: `README.md`

**Interfaces:**
- Consumes: 全部已完成组件
- Produces: 开发态一键运行体验；完整使用文档与打包建议

- [ ] **Step 1: 写 scripts/build_backend.cmd**

```bat
@echo off
setlocal
cd /d "%~dp0.."

cmake -S backend -B backend\build -A x64
if errorlevel 1 exit /b 1
cmake --build backend\build --config Release
if errorlevel 1 exit /b 1
ctest --test-dir backend\build -C Release --output-on-failure
if errorlevel 1 exit /b 1

if not exist bin\backend mkdir bin\backend
copy /y backend\build\Release\backend.exe bin\backend\ >nul
copy /y backend\build\Release\backend_tests.exe bin\backend\ >nul
echo [OK] backend built and tested
```

- [ ] **Step 2: 写 scripts/run_dev.cmd**

```bat
@echo off
setlocal
cd /d "%~dp0.."

call scripts\build_backend.cmd
if errorlevel 1 exit /b 1

dotnet build AiCompanion.sln -c Debug
if errorlevel 1 exit /b 1

rem 将后端部署到 WPF 输出目录（生产布局：app.exe 同级 backend\backend.exe）
set APPDIR=src\AiCompanion.App\bin\Debug\net8.0-windows
if not exist "%APPDIR%\backend" mkdir "%APPDIR%\backend"
copy /y bin\backend\backend.exe "%APPDIR%\backend\" >nul

rem 开发调试：设 LLM_MOCK=1 使用内置假流；不设则需在设置面板填入真实 API
set AI_COMPANION_DATA=%CD%\data
set AI_COMPANION_BACKEND=%CD%\bin\backend\backend.exe

echo 启动桌面程序（关闭窗口后自动清理后端进程）...
dotnet run --project src\AiCompanion.App -c Debug --no-build

taskkill /im backend.exe /f >nul 2>&1
endlocal
```

- [ ] **Step 3: 写 README.md**

```markdown
# AI 陪伴（AI Companion Desktop）

Windows 桌面 AI 陪伴程序：WPF 壳 + WebView2 渲染的 HTML 聊天界面 + C++ 后端（cpp-httplib + SQLite 内嵌），直连任意 OpenAI 兼容云端 API。

## 特性
- 流式对话（SSE + 打字机效果），完成后 Markdown 渲染与代码高亮
- 多会话管理（自动标题、重命名、删除、导出 md/json/txt）
- 自定义人设（名称/emoji 头像/开场白/System Prompt/temperature），一键切换
- 上下文记忆：可配置保留最近 N 轮（0 = 不限）
- 多主题（深色/亮色/樱花粉/午夜蓝）
- API Key 经 Windows DPAPI 加密存储；支持 HTTP 代理（含系统代理）
- 后端崩溃自动重启（≤3 次退避）；运行日志 data/logs/backend.log
- 插件 Webhook：编辑 data/config.json 的 plugins 数组，接收 chat.completed 等事件

## 环境依赖
| 依赖 | 说明 |
|---|---|
| .NET 8 SDK | https://dotnet.microsoft.com/download |
| Visual Studio 2022 Build Tools | 勾选"使用 C++ 的桌面开发"（MSVC + CMake） |
| CMake ≥ 3.20 | Build Tools 自带 |
| WebView2 Runtime | Win11 自带；Win10 装 Evergreen Runtime：https://developer.microsoft.com/microsoft-edge/webview2/ |

## 快速开始
```bat
scripts\run_dev.cmd
```
首次运行：打开"设置"填入 API Base URL / API Key / 模型名（如 `https://api.deepseek.com/v1` + `deepseek-chat`）。

联调模式（无真实 API）：先 `set LLM_MOCK=1` 再运行 run_dev.cmd，得到内置假流回复。

## 手动构建
```bat
scripts\build_backend.cmd                :: C++ 后端（含单测）
dotnet build AiCompanion.sln -c Release  :: WPF
```

## 集成测试
```bat
set LLM_MOCK=1
bin\backend\backend.exe --data data_e2e
powershell -ExecutionPolicy Bypass -File scripts\test_api.ps1
```

## 数据与日志
- 数据库：`data/app.db`（SQLite WAL；表：sessions / messages / personas / settings）
- 配置：`data/config.json`（端口、默认值、插件清单）
- 日志：`data/logs/backend.log*`（滚动 5 个 × 2MB）

## API 摘要（127.0.0.1:8765）
`/health`；`/api/v1/config`；`/api/v1/personas[/{id}[/activate]]`；`/api/v1/sessions[/{id}]`；`/api/v1/messages[?session_id&limit]`；`/api/v1/chat/stream|regenerate|stop`（SSE）；`/api/v1/export?session_id&format=md|json|txt`。详见规格文档 docs/superpowers/specs/。

## 插件（Webhook）
在 data/config.json 中登记：
```json
{ "plugins": [ { "name": "示例", "url": "http://127.0.0.1:9000/hook", "events": ["chat.completed", "chat.error"] } ] }
```
后端以 fire-and-forget 方式 POST `{"type":..., "ts":..., "data":...}` 到 url（2s 超时，失败仅记日志）。事件类型：chat.completed / chat.error / session.created / session.deleted / persona.activated。

## 打包建议（发布给他人使用）
1. 后端已静态链接 CRT（/MT），直接复用 backend.exe。
2. WPF 自包含发布：
   ```bat
   dotnet publish src\AiCompanion.App -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -o bin\app
   xcopy /e /i src\AiCompanion.App\wwwroot bin\app\wwwroot
   mkdir bin\app\backend & copy bin\backend\backend.exe bin\app\backend\
   ```
3. 用 Inno Setup 将 bin\app 打包为安装程序（Startup 勾选 CreateNoWindow，安装到 %LOCALAPPDATA%\AICompanion）。
4. Win10 目标机需预装 WebView2 Evergreen Runtime（Inno Setup 可静默安装）。

## FAQ
- **8765 被占用？** 后端自动尝试 8766-8775，实际端口写入 data/config.json 的 server.actual_port，WPF 自动读取。
- **连不上 API？** 检查 Base URL 是否含 /v1；需要代理时在设置里开启并填代理地址（留空 = 系统代理）。
- **消息发不出去？** 查看 data/logs/backend.log。
```

- [ ] **Step 4: 最终端到端验收**

```bash
# 全量测试
cmake --build backend/build --config Release
ctest --test-dir backend/build -C Release --output-on-failure
set LLM_MOCK=1
bin\backend\backend.exe --data data_e2e &
powershell.exe -ExecutionPolicy Bypass -File scripts/test_api.ps1
kill %1
scripts\run_dev.cmd   # 人工走查 Task 10 Step 2 + Task 11 Step 7 全部清单项
```

- [ ] **Step 5: Commit**

```bash
git add scripts/ README.md
git commit -m "docs: 一键运行脚本、README 与打包建议"
```

---

## 计划自审记录（已修复项）

1. **规格覆盖**：对照规格 §4–§12 逐节检查，全部有对应任务；发现规格 §3/§8.3 提到的 `BridgeProtocol.cs` 在计划中缺失 → 已补入 Task 11 Step 5，消息协议常量与 MainWindow 代码统一。
2. **占位符扫描**：无 TBD/TODO；Task 9 的 app.js stub 是分阶段交付物（Task 10 全量替换），非计划缺陷。
3. **类型一致性**：
   - `test_db.cpp` 中 `db::getSession(...) == std::nullopt` 无法编译（Session 无 operator==）→ 改为 `!...has_value()`。
   - `test_event_bus.cpp` 第二个用例名称与断言语义不符 → 重命名为"本进程监听者收到全部事件（过滤只发生在 Webhook 层）"。
   - Task 10 app.js 打字机 `finalize` 用固定 60ms 延时，缓冲未排空时 Markdown 渲染会截断尾部文本 → Typewriter 增加 `onDrain` 回调，排空后再渲染。
   - Task 7 server.cpp 中 activate 处理器残留占位标签、缺 `#include "event_bus.h"` 与 `<ctime>` → 已清理并补齐。
   - 跨任务接口核验：`db::`/`cfg::`/`llm::`/`chat::`/`ev::`/`dpapi::` 的签名与字段在任务间引用完全一致；前端 DOM id 与 app.js 逐一对应；WPF 消息协议与 app.js 桥接一致。
4. **规格同步**：上游 HTTPS 客户端改用 WinHTTP（Schannel，零 OpenSSL 依赖），规格 §2 与目录结构已同步修订；vendor 增加 DOMPurify。

## 执行交接

按任务顺序执行（1→12），每个任务以各自"验证"步骤通过 + commit 为完成标志。前端任务（9/10）与 WPF 任务（11）的验证包含人工清单，需在 Windows 桌面环境执行。执行方式二选一：

1. **子代理驱动（推荐）**：每个任务派发独立子代理实现 + 两阶段审查，迭代快、上下文干净。
2. **本会话内联执行**：使用 executing-plans 技能，分批执行并在检查点暂停审查。




