# Orua

Windows 桌面 AI 陪伴应用：多 Bot + 群聊围观。WPF 壳 + WebView2 前端 + C++ 后端（cpp-httplib / SQLite / WinHTTP），接入任意 OpenAI 兼容 API（如 DeepSeek、通义、Moonshot），无需本地模型。

## 功能

- **多 Bot**：创建任意数量的 Bot，各自独立的人设（名称 / 头像 / System Prompt / Temperature / 开场白）
- **独立 API 配置**：每个 Bot 可覆盖 API Base URL / Key / 模型，留空即继承全局默认配置
- **1v1 对话**：新会话可选 Bot，随时切换；流式打字机、Markdown + 代码高亮、复制 / 重新生成 / 删除 / 清空、自动标题
- **群聊围观**：拉 2-10 个 Bot 进群，按顺序自动轮流连聊；实时围观、插话、暂停 / 继续 / 停止、换话题、轮数显示；单轮失败自动跳过并提示，同一 Bot 连续 3 次失败自动暂停
- **会话导出**：md / json / txt
- **四套主题**：深色（默认，深海军蓝）/ 亮色 / 樱花粉 / 午夜蓝
- **HTTP 代理**：支持显式代理或系统代理
- **安全**：API Key 用 Windows DPAPI（用户作用域）加密落库，接口仅返回掩码
- **插件 Webhook**：后端事件总线支持自定义 Webhook 插件（见 `backend/src/event_bus.h`）
- **后端守护**：WPF 壳自动拉起后端进程，崩溃自动重启，退出时清理
- **Bot 长期记忆**：私聊 / 群聊各自记忆，对话中自动提取、检索并注入（提取/检索失败静默降级）
- **记忆库页面**：浏览 / 搜索 / 编辑 / 删除任意 Bot 的记忆，删除后立即停止注入
- **图片头像上传**：本地上传图片（128×128 PNG ≤256KB），列表 / 详情 / 聊天气泡 / 群聊气泡 / 成员面板统一显示
- **Bot 编辑器新字段**：最大 tokens 与 API 类型（API 类型预留「OpenAI 兼容」）

## 架构

```
WPF (net8.0-windows, WebView2, app.local)
  └─ wwwroot/ (index.html + js/app.js + css/theme.css, 虚拟主机)
      └─ HTTP → C++ 后端 (127.0.0.1:8765, 回退 8766-8775)
          ├─ SQLite (data/app.db, WAL) — 会话/Bot/群/消息/记忆/设置
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

输出 `CRUD API TESTS PASSED` 即通过（覆盖 bots/groups CRUD、群 SSE ≥2 轮、插话、会话 bot 绑定、单聊流/重新生成/导出/清空、私聊/群聊记忆提取、记忆 CRUD/搜索、头像上传、memory_extraction 配置等）。测试用独立数据目录 `data_e2e`，不影响 `data/` 用户数据。

## 数据存储

`data/app.db`（SQLite，WAL）：

| 表 | 说明 |
|---|---|
| bots | Bot（含 API 覆盖项 api_base_url / api_key_enc / api_model，key 为 DPAPI 密文） |
| sessions | 会话（bot_id 绑定 Bot，0 = 默认） |
| messages | 单聊消息 |
| groups / group_members / group_messages | 群 / 成员（position 即发言顺序）/ 群消息（bot_id 为 NULL = 用户插话） |
| settings | 全局默认 API 配置、主题、代理、default_bot_id、memory_extraction（记忆提取开关） |
| bot_memories | Bot 记忆（bot_id / scope=private/group / group_id / content / tags / source_msg_id） |

## API 摘要（统一前缀 /api/v1，信封 {code, data|message}）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET/POST | /bots | Bot 列表（含掩码 Key、default_bot_id）/ 创建 |
| PATCH/DELETE | /bots/{id} | 编辑（api_key 空串=清除）/ 删除（默认 Bot → 409 + code 1001） |
| POST | /bots/{id}/enable | 启停 {enabled} |
| GET | /bots/{id}/memories?scope=&group_id=&q= | 记忆列表 / 搜索（scope=private/group，q 匹配内容/标签） |
| PUT | /bots/{id}/memories/{memId} | 编辑记忆 {content} |
| DELETE | /bots/{id}/memories/{memId} | 删除记忆 |
| POST | /bots/{id}/avatar | 上传头像 {image_base64}（PNG ≤256KB） |
| GET | /bots/{id}/avatar | 头像图片 |
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
| GET/POST | /config | 全局默认 API 配置 / 主题 / 代理 / 记忆提取开关 |

错误码：1001=默认 Bot 不可删除，1002=群状态冲突，1003=成员数越界（2-10），1004=含停用 Bot。

## 插件

`backend/src/event_bus.h` 定义事件总线，内置 Webhook 插件可将事件 POST 到外部地址。事件：`chat.completed` / `chat.error` / `session.created` / `session.deleted`。

## 打包

```cmd
dotnet publish src\AiCompanion.App -c Release -r win-x64
```

将 `bin\backend\backend.exe` 放在发布目录的 `backend\` 子目录下（`run_dev.cmd` 的部署步骤即此布局）。

## 验收清单

### v2

1. 窗口标题与 UI 显示「Orua」；标题栏拖动、双击最大化/还原、右键系统菜单可用
2. 创建 ≥3 个 Bot（含不同 API 覆盖），新建 1v1 会话可任选 Bot 流式对话
3. 创建含 2-10 个 Bot 的群，点开聊后 Bot 按顺序自动连聊、消息流式展示；暂停 / 继续 / 停止 / 插话 / 换话题全部可用
4. 某 Bot API 不可用时该轮显示失败气泡、群聊不中断；连续 3 次失败自动暂停
5. 重启程序后：Bot / 群 / 历史消息完整；群状态复位 idle 可重新开聊
6. v1 全部既有功能回归通过；ctest 全绿 + test_api.ps1 全绿

### v3

1. 全 UI（含 README、验收清单）无旧称字样，统一显示「Bot」
2. 新建 Bot 弹窗含全部字段（名字/头像/API 类型/地址/Key/模型/温度/最大 tokens）+ 开场白 + 提示词/人设；创建后可在详情页随时修改
3. 头像上传后，列表/详情/聊天气泡/群聊气泡/成员面板显示图片头像；无图 Bot 回退 emoji
4. 私聊数轮后，记忆库出现提取的记忆（真实 API 或 LLM_MOCK 下均可验证）
5. 私聊新会话中 Bot 的回答体现记忆（增强式注入生效）
6. 群聊运行后，发言 Bot 产生 group 记忆（bot_id/group_id 正确，各 Bot 各自记）
7. 群聊上下文带群记忆驱动行为；私聊记忆不出现在群聊、群记忆不进入私聊
8. 记忆库搜索/编辑/删除即时生效；删除后注入立即停止
9. `memory_extraction` 关闭后不再新增记忆，已有记忆仍注入
10. 记忆提取/检索失败时聊天与群聊功能正常（静默降级）
11. v2 全部功能与自动化测试保持全绿；老库迁移零破坏
