# Orua

Windows 桌面 AI 陪伴应用：多机器人 + 群聊围观。WPF 壳 + WebView2 前端 + C++ 后端（cpp-httplib / SQLite / WinHTTP），接入任意 OpenAI 兼容 API（如 DeepSeek、通义、Moonshot），无需本地模型。

## 功能

- **多机器人**：创建任意数量的机器人，各自独立的人设（名称 / 头像 / System Prompt / Temperature / 开场白）
- **独立 API 配置**：每个机器人可覆盖 API Base URL / Key / 模型，留空即继承全局默认配置
- **1v1 对话**：新会话可选机器人，随时切换；流式打字机、Markdown + 代码高亮、复制 / 重新生成 / 删除 / 清空、自动标题
- **群聊围观**：拉 2-10 个机器人进群，按顺序自动轮流连聊；实时围观、插话、暂停 / 继续 / 停止、换话题、轮数显示；单轮失败自动跳过并提示，同一机器人连续 3 次失败自动暂停
- **会话导出**：md / json / txt
- **四套主题**：深色（默认，深海军蓝）/ 亮色 / 樱花粉 / 午夜蓝
- **HTTP 代理**：支持显式代理或系统代理
- **安全**：API Key 用 Windows DPAPI（用户作用域）加密落库，接口仅返回掩码
- **插件 Webhook**：后端事件总线支持自定义 Webhook 插件（见 `backend/src/event_bus.h`）
- **后端守护**：WPF 壳自动拉起后端进程，崩溃自动重启，退出时清理

## 架构

```
WPF (net8.0-windows, WebView2, app.local)
  └─ wwwroot/ (index.html + js/app.js + css/theme.css, 虚拟主机)
      └─ HTTP → C++ 后端 (127.0.0.1:8765, 回退 8766-8775)
          ├─ SQLite (data/app.db, WAL) — 会话/机器人/群/消息/设置
          ├─ 群聊引擎 (每群一个线程的状态机, SSE 广播)
          └─ WinHTTP → OpenAI 兼容 API
```

## 运行开发版

```cmd
scripts\run_dev.cmd
```

构建后端 + WPF 并启动（后端由 WPF 壳守护，退出时自动清理）。`run_dev.cmd` 前 `set LLM_MOCK=1` 可用内置假流离线体验全部功能（群聊、流式打字机等），无需真实 API Key。

## 手动构建

```cmd
scripts\build_backend.cmd          # 后端 → bin/backend/backend.exe（含单元测试 backend_tests.exe）
dotnet build AiCompanion.sln -c Debug
```

后端单元测试：

```cmd
bin\backend\backend_tests.exe
```

## 集成测试

```cmd
set LLM_MOCK=1
bin\backend\backend.exe --data data_e2e
```

另开一个终端：

```cmd
powershell -ExecutionPolicy Bypass -File scripts\test_api.ps1
```

输出 `CRUD API TESTS PASSED` 即通过（覆盖 bots/groups CRUD、群 SSE ≥2 轮、插话、会话 bot 绑定、单聊流/重新生成/导出/清空等）。测试用独立数据目录 `data_e2e`，不影响 `data/` 用户数据。

## 数据存储

`data/app.db`（SQLite，WAL）：

| 表 | 说明 |
|---|---|
| bots | 机器人（含 API 覆盖项 api_base_url / api_key_enc / api_model，key 为 DPAPI 密文） |
| sessions | 会话（bot_id 绑定机器人，0 = 默认） |
| messages | 单聊消息 |
| groups / group_members / group_messages | 群 / 成员（position 即发言顺序）/ 群消息（bot_id 为 NULL = 用户插话） |
| settings | 全局默认 API 配置、主题、代理、default_bot_id |

## API 摘要（统一前缀 /api/v1，信封 {code, data|message}）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET/POST | /bots | 机器人列表（含掩码 Key、default_bot_id）/ 创建 |
| PATCH/DELETE | /bots/{id} | 编辑（api_key 空串=清除）/ 删除（默认机器人 → 409 + code 1001） |
| POST | /bots/{id}/enable | 启停 {enabled} |
| GET/POST | /groups | 群列表 / 创建 {name, bot_ids[2-10], topic?} |
| GET/PATCH/DELETE | /groups/{id} | 详情（含 members）/ 编辑（运行中改成员 → 409 + code 1002）/ 删除（运行中先停止） |
| GET | /groups/{id}/messages?limit=200 | 群消息（倒序） |
| POST | /groups/{id}/start\|pause\|resume\|stop\|interject | 群控制（start 可带 {topic}） |
| GET | /groups/{id}/stream | 群流 SSE（status / turn_start / delta / turn_end / interject / error） |
| GET/POST | /sessions | 列表 / 创建 {title?, bot_id?} |
| PATCH/DELETE | /sessions/{id} | 改名 / 切换 bot_id / 删除 |
| GET/DELETE | /messages?session_id= | 消息（倒序）/ 清空；DELETE /messages/{id} 删除单条 |
| POST | /chat/stream | 单聊流式 SSE（delta / done / error） |
| POST | /chat/regenerate | 重新生成最后一条回复 |
| POST | /chat/stop | 中止进行中的生成 |
| POST | /export?session_id=&format=md\|json\|txt | 会话导出 |
| GET/POST | /config | 全局默认 API 配置 / 主题 / 代理 |

错误码：1001=默认机器人不可删除，1002=群状态冲突，1003=成员数越界（2-10），1004=含停用机器人。

## 插件

`backend/src/event_bus.h` 定义事件总线，内置 Webhook 插件可将事件 POST 到外部地址。事件：`chat.completed` / `chat.error` / `session.created` / `session.deleted`。

## 打包

```cmd
dotnet publish src\AiCompanion.App -c Release -r win-x64
```

将 `bin\backend\backend.exe` 放在发布目录的 `backend\` 子目录下（`run_dev.cmd` 的部署步骤即此布局）。

## 验收清单（v2）

1. 窗口标题与 UI 显示「Orua」；标题栏拖动、双击最大化/还原、右键系统菜单可用
2. 创建 ≥3 个机器人（含不同 API 覆盖），新建 1v1 会话可任选机器人流式对话
3. 创建含 2-10 个机器人的群，点开聊后机器人按顺序自动连聊、消息流式展示；暂停 / 继续 / 停止 / 插话 / 换话题全部可用
4. 某机器人 API 不可用时该轮显示失败气泡、群聊不中断；连续 3 次失败自动暂停
5. 重启程序后：机器人 / 群 / 历史消息完整；群状态复位 idle 可重新开聊
6. v1 全部既有功能回归通过；ctest 全绿 + test_api.ps1 全绿
