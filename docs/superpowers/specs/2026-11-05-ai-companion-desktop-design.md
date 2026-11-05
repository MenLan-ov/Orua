# AI 陪伴桌面程序 — 设计规格

- 日期：2026-11-05
- 状态：已确认（待用户复审）
- 交付形态：开发态可运行（含一键启动脚本），打包为后续事项

## 1. 目标与范围

### 1.1 目标
Windows 桌面 AI 陪伴程序：用户连接外部 OpenAI 兼容大模型 API，通过自定义人设、上下文记忆、流式对话获得陪伴式聊天体验。对话记录、人设、配置全部存储在本地 SQLite。

### 1.2 核心功能（v1）
1. 配置 API Base URL、API Key（DPAPI 加密存储）、模型名称、HTTP 代理
2. 自定义人设：名称、头像（emoji）、开场白、System Prompt、temperature；支持增删改、切换、激活
3. 多会话管理：新建、切换、重命名、删除会话；首条消息自动生成标题
4. 上下文记忆：可设置保留最近 N 轮对话（默认 20）
5. 流式输出：SSE 传输，前端打字机效果，完成后 Markdown 渲染（含代码高亮）
6. 消息操作：重新生成、复制、删除单条、中止生成、清空会话
7. 导出对话：Markdown / JSON / TXT
8. 多主题 UI（默认深色，设置内切换）
9. WPF 启动时自动拉起后端，后端崩溃自动重启（最多 3 次，指数退避），运行日志落盘

### 1.3 非目标（v1 不做）
- 本地模型推理（接口已预留 provider 抽象）
- TTS 语音、系统托盘、开机自启
- 滚动摘要式长期记忆
- 安装包打包（README 给出打包建议）

## 2. 技术栈与架构

```
┌───────────────────────────── AiCompanion.sln ─────────────────────────────┐
│  ┌─ WPF 壳 (C# / .NET 8) ───────────────────────────────────────────┐     │
│  │  MainWindow（无边框圆角窗 + WebView2）                             │     │
│  │  ├─ 启动时拉起 backend.exe，轮询 /health 就绪后加载页面             │     │
│  │  ├─ 崩溃自动重启（≤3 次退避），经 postWebMessageAsJson 通知前端     │     │
│  │  └─ wwwroot/ 经 SetVirtualHostNameToFolderMapping 映射为本地站点    │     │
│  └───────────────┬────────────────────────────────────────────────────┘    │
│                  │ HTTP · localhost:8765 (REST + SSE)                       │
│  ┌───────────────▼───────────────────────────────────────────────┐        │
│  │  backend.exe (C++20 · cpp-httplib · sqlite3 内嵌)              │        │
│  │  ├─ 路由层 /api/v1/*                                            │        │
│  │  ├─ ChatService：上下文组装（system prompt + 最近 N 轮）         │        │
│  │  ├─ ILlmProvider → OpenAiCompatProvider（httplib::Client+SSL）  │        │
│  │  ├─ EventBus → 插件 Webhook 分发（可选）                         │        │
│  │  └─ 存储层：data/app.db (SQLite WAL)、data/config.json          │        │
│  └───────────────┬───────────────────────────────────────────────┘        │
│                  │ HTTPS · /v1/chat/completions (stream=true)              │
│              ┌───▼────────────────────────────┐                             │
│              │  任意 OpenAI 兼容云端 API        │                             │
│              │  (OpenAI/DeepSeek/Kimi/Qwen…)   │                             │
│              └────────────────────────────────┘                             │
```

选型理由：
- **cpp-httplib**：单头文件、内建 HTTP Client（含 SSL）、原生 chunked/SSE 流式，单用户桌面场景性能绰绰有余。
- **sqlite3 amalgamation**：单 .c/.h 静态编译进 exe，零外部运行时依赖。
- **nlohmann/json**：单头文件 JSON，用于 API 序列化与 config.json。
- **doctest**：单头文件测试框架，`ctest` 运行核心逻辑断言测试。
- **WebView2 + 本地静态页面**：UI 全用 HTML/CSS/JS，无框架，依赖最少。

## 3. 目录结构

```
D:\One\
├── AiCompanion.sln
├── README.md                        # 运行步骤、依赖、打包建议、FAQ
├── .gitignore
├── scripts\
│   ├── run_dev.cmd                  # 一键启动（构建后端→起后端→起 WPF）
│   ├── build_backend.cmd            # CMake 构建 backend.exe
│   └── test_api.ps1                 # curl 集成测试序列（含 mock 模式说明）
├── src\
│   └── AiCompanion.App\             # WPF 壳
│       ├── AiCompanion.App.csproj   # net8.0-windows + WebView2 NuGet
│       ├── App.xaml / App.xaml.cs
│       ├── MainWindow.xaml / .cs    # WebView2 初始化、窗口圆角/拖拽/缩放
│       ├── BackendProcess.cs        # 拉起/健康检查/自动重启/退出清理
│       ├── BridgeProtocol.cs        # WebView2 双向消息协议（类型化）
│       └── wwwroot\                 # CopyToOutputDirectory
│           ├── index.html
│           ├── css\theme.css        # 多主题 CSS 变量
│           ├── js\app.js            # 聊天逻辑 + SSE 流式渲染 + 命令注册表
│           └── vendor\
│               ├── marked.min.js    # 本地打包，离线可用
│               └── highlight.min.js # + 高亮主题 css
├── backend\
│   ├── CMakeLists.txt
│   ├── src\
│   │   ├── main.cpp                 # 入口 + 路由注册
│   │   ├── server.h/.cpp            # REST/SSE 路由处理器
│   │   ├── db.h/.cpp                # SQLite 封装 + 建表迁移
│   │   ├── chat_service.h/.cpp      # 上下文组装（N 轮截断）
│   │   ├── llm_provider.h           # ILlmProvider 抽象接口
│   │   ├── openai_provider.h/.cpp   # OpenAI 兼容流式实现（含代理）
│   │   ├── event_bus.h/.cpp         # 事件总线 + Webhook 插件分发
│   │   ├── dpapi.h/.cpp             # Windows DPAPI 加解密封装
│   │   ├── config.h/.cpp            # config.json 读写
│   │   ├── logger.h/.cpp            # 分级文件日志（滚动 5 个）
│   │   └── json_util.h              # nlohmann 别名与辅助函数
│   ├── tests\
│   │   ├── test_db.cpp              # DB CRUD 断言测试
│   │   ├── test_chat_service.cpp    # 上下文组装/N 轮截断测试
│   │   └── test_json.cpp
│   └── third_party\
│       ├── httplib.h
│       ├── json.hpp
│       ├── doctest.h
│       └── sqlite\sqlite3.c / sqlite3.h
└── data\                            # 运行时生成（git 忽略）
    ├── app.db
    ├── config.json
    └── logs\backend.log*
```

## 4. API 契约（后端 localhost:8765，前缀 /api/v1）

统一响应格式：成功 `{"code":0,"data":...}`，失败 `{"code":<非0>,"message":"..."}`。SSE 事件除外。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/health` | `{"status":"ok","version":"1.0.0"}` |
| GET | `/api/v1/config` | 读取配置；api_key 返回掩码 `sk-***abc`，proxy 等一并返回 |
| POST | `/api/v1/config` | 保存配置；api_key 传空字符串表示不变更 |
| GET | `/api/v1/personas` | 人设列表（含 is_active） |
| POST | `/api/v1/personas` | 新建；带 id 时更新 |
| DELETE | `/api/v1/personas/{id}` | 删除（不允许删除当前激活人设） |
| POST | `/api/v1/personas/{id}/activate` | 激活；若当前会话无消息则插入 greeting 开场白 |
| GET | `/api/v1/sessions` | 会话列表（按 updated_at 倒序） |
| POST | `/api/v1/sessions` | 新建会话 `{title?}` |
| PATCH | `/api/v1/sessions/{id}` | 重命名 `{title}` |
| DELETE | `/api/v1/sessions/{id}` | 删除会话（级联删消息） |
| GET | `/api/v1/messages?session_id=N&limit=M` | 历史消息（倒序返回，limit 默认 100） |
| DELETE | `/api/v1/messages?session_id=N` | 清空会话消息 |
| DELETE | `/api/v1/messages/{id}` | 删除单条 |
| POST | `/api/v1/chat/stream` | SSE 流式对话 `{session_id, content}` |
| POST | `/api/v1/chat/regenerate` | SSE 重新生成 `{session_id}`（删最后一条 assistant，用最后一条 user 重跑） |
| POST | `/api/v1/chat/stop` | 中止当前会话的生成 |
| POST | `/api/v1/export?session_id=N&format=md\|json\|txt` | 导出；响应为文件流（Content-Disposition） |

SSE 事件格式（`/api/v1/chat/stream`、`/api/v1/chat/regenerate`）：

```
event: delta     data: {"content":"文本增量"}
event: done      data: {"assistant_message_id":123,"content":"完整回复","session_id":1}
event: error     data: {"code":401,"message":"API Key 无效"}
```

CORS：后端允许 `https://app.local/*`（WebView2 虚拟主机）来源，供前端 Fetch 直连。

## 5. 数据流

1. 用户输入 → 前端 `POST /api/v1/chat/stream`
2. 后端：读当前人设（system_prompt）+ 该会话最近 N 轮消息组装上下文 → 落库 user 消息
3. `OpenAiCompatProvider` 经 httplib::Client（可选代理）请求外部 API `stream=true`
4. 解析上游 SSE delta → 重组为统一 `delta` 事件转发前端
5. 前端打字机渲染（纯文本）；`done` 后 Markdown 渲染（marked + highlight），落库 assistant 消息
6. 事件总线同步发出 `chat.completed` 等事件 → 分发给已注册插件 Webhook（fire-and-forget）

## 6. 数据库表结构（SQLite，data/app.db）

```sql
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
```

- 首次启动自动建表 + 播种默认人设（"默认助手"）。
- API Key：DPAPI（CryptProtectData，当前用户作用域）加密 → Base64 → `settings(key='api_key_enc')`。
- 所有时间戳 ISO8601（UTC）。

## 7. 配置文件（data/config.json）

```json
{
  "server": { "host": "127.0.0.1", "port": 8765 },
  "defaults": {
    "keep_rounds": 20,
    "theme": "dark",
    "temperature": 0.8
  },
  "plugins": [
    { "name": "示例插件", "url": "http://127.0.0.1:9000/hook", "events": ["chat.completed"] }
  ]
}
```

- 仅存非敏感配置；API 配置（base_url/api_key/model/proxy）存 SQLite settings 表。
- `plugins` 数组即插件清单：后端事件总线按 `events` 列表将事件 POST 到 `url`（JSON、2s 超时、失败仅记日志）。

## 8. 可扩展性与插件接口设计

用户要求：接口设计为将来增加功能与插件留好缝。

### 8.1 后端（C++）
1. **`ILlmProvider` 抽象**（`llm_provider.h`）：
   ```cpp
   struct StreamEvent { std::string type; std::string content; int error_code; std::string message; };
   using StreamCallback = std::function<void(const StreamEvent&)>;
   class ILlmProvider {
   public:
     virtual ~ILlmProvider() = default;
     virtual void streamChat(const ChatRequest&, StreamCallback, const std::atomic_bool& stop) = 0;
   };
   ```
   将来加本地推理 = 新增一个 `LocalInferenceProvider` 实现并在工厂注册，路由层零改动。
2. **`EventBus` + Webhook 插件**：v1 只做内置事件总线 + config.json 插件清单分发（语言无关，任何进程可接收）。事件类型：`chat.completed`、`chat.error`、`session.created`、`session.deleted`、`persona.activated`。事件 JSON 统一含 `{type, ts, data}`。
3. **REST 版本化**：所有业务接口走 `/api/v1/`，未来破坏性变更走 `/api/v2/` 并存。
4. **存储层接口**：`db.h` 只暴露领域函数（`createSession/appendMessage/listMessages/...`），不泄露 SQL 细节；将来换存储实现只改 `db.cpp`。

### 8.2 前端（JS）
- `app.js` 提供 `App.registerCommand(name, handler)` 命令注册表 + 简单事件发射器；UI 操作（复制/删除/重生成等）全部注册为命令，将来扩展按钮/快捷键/插件脚本只需注册新命令。
- 与 WPF 的桥接消息统一 `{type, payload}` 协议（`BridgeProtocol.cs` 定义类型），前后端各自白名单校验。

### 8.3 WPF（C#）
- `BackendProcess.cs` 与 UI 解耦：仅负责进程生命周期并通过事件上报状态（Starting/Running/Restarting/Stopped），状态变化经 `postWebMessageAsJson` 推给前端。
- `BridgeProtocol.cs` 集中定义全部 WebView2 消息类型，新增交互只需在此登记。

## 9. 错误处理与健壮性

| 场景 | 处理 |
|---|---|
| 后端未启动/崩溃 | WPF 自动重启（≤3 次，1s/2s/4s 退避），前端显示"连接中断"横幅；3 次失败后弹窗提示手动启动 |
| API 401/403 | SSE `error` 事件 → 前端红色气泡"API Key 无效" |
| API 429/5xx/超时 | `error` 事件提示并保留已渲染内容，可重新生成 |
| 上游断流 | `error` 事件；已落库的 user 消息保留 |
| 配置缺失 | 前端检测后引导打开设置面板，禁止发送 |
| 日志 | `data/logs/backend.log`，分级，滚动保留 5 个文件；DPAPI 失败等异常全部落日志 |

## 10. 测试策略

- **C++ 单元测试**（doctest + ctest）：DB CRUD、级联删除、上下文组装与 N 轮截断、JSON 序列化、DPAPI 往返。
- **后端集成**（`scripts/test_api.ps1`）：health → 建会话 → 流式对话（本地 mock）→ 导出 → 清空 → 删除。文档说明如何起 mock 上游（后端支持 `LLM_MOCK=1` 环境变量，内置假流式响应）。
- **前端**：手动清单（打字机、主题切换、会话管理、复制/删除/重生成、设置保存）。
- **WPF**：手动验证启动拉起、崩溃重启、优雅退出。

## 11. 构建与运行

- 依赖：.NET 8 SDK、Visual Studio 2022 Build Tools（MSVC C++20）、CMake ≥3.20、WebView2 Runtime（Win11 自带）。
- `scripts\build_backend.cmd`：CMake（Release）→ `backend\build\Release\backend.exe` 拷贝到 `bin\backend\`。
- `dotnet build` 输出 WPF 到 `bin\app\`，wwwroot 随拷贝。
- `scripts\run_dev.cmd`：一键构建 + 启动。
- 打包建议（README）：Inno Setup（后端静态链接 /MT，WPF `dotnet publish -r win-x64 --self-contained`）。

## 12. 风险与备注

- **外部 API 兼容性**：不同厂商 SSE 格式略有差异（data 换行、`[DONE]` 位置）。`OpenAiCompatProvider` 的 SSE 解析器按"行缓冲 + 前缀剥离"实现，不依赖固定换行；对已知差异（如 Kimi 的 usage 事件）宽容跳过。
- **WebView2 环境**：Win11 自带；README 说明 Win10 需装 Evergreen Runtime。
- **端口冲突**：8765 被占时后端换 8766 重试并写 config，WPF 读取 config 得知实际端口。
