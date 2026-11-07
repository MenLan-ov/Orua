# Orua

Windows 桌面 AI 陪伴应用：多 Bot + 群聊围观。WPF 壳 + WebView2 前端 + C++ 后端（cpp-httplib / SQLite / WinHTTP），接入任意 OpenAI 兼容 API（如 DeepSeek、通义、Moonshot），无需本地模型。

## 功能

- **多 Bot**：创建任意数量的 Bot，各自独立的人设（名称 / 头像 / System Prompt / Temperature / 开场白）
- **独立 API 配置**：每个 Bot 可覆盖 API Base URL / Key / 模型，留空即继承全局默认配置
- **1v1 对话**：新会话可选 Bot，随时切换；流式打字机、Markdown + 代码高亮、复制 / 重新生成 / 删除 / 清空、自动标题
- **群聊围观**：拉 2-10 个 Bot 进群，按顺序自动轮流连聊；实时围观、插话、暂停 / 继续 / 停止、换话题、轮数显示；单轮失败自动跳过并提示，同一 Bot 连续 3 次失败自动暂停；失败原因与「为什么停了」落库留痕，重开房间仍能看到（留痕不进模型上下文）
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
- **QQ 式聊天列表**：侧栏「消息」页签改名「聊天」，单聊与群聊合并为同一列表（置顶区 + 按最近活跃排序）；每个 Bot 一个聊天项，点击进入其最近会话（无会话自动创建），未读红点与置顶重启保留；群聊项在群处于「进行中」时显示小徽标
- **QQ 式右键菜单**：按项类型（Bot / 群）适配——置顶 / 标记未读 / 历史会话子菜单（打开 · 设为默认 · 重命名 · 清空 · 导出 · 删除）/ 编辑 Bot / 记忆库 / 群设置 / 打开独立聊天窗口 / 从列表移除（删除二次确认，默认 Bot 禁删 → 1001）
- **独立聊天窗口**：任一会话或群聊可在独立窗口中与主窗口并行打开（同一前端 detached 模式），窗口标题取 Bot 名 / 群名；发送与同步照常；同一目标重复打开聚焦已有窗口（最小化时先恢复），主窗口关闭时联动关闭
- **到期提醒提示**：主窗口每 15s 轮询 `GET /api/v1/reminders?due=1`，每个轮询周期至多提示一条——按 `due_at` 最早的一条呈现为「〈Bot〉提醒你：〈内容〉」，本轮还有其它待发时在同一条文案后附「（另有 N 条待提醒）」；该条提示落地后自动 `POST /api/v1/reminders/{id}/done`，未提示的留到下一轮逐条消化
- **Bot 技能系统**：6 个内置技能（计算器 / 时间日期 / 系统信息 / HTTP 请求 / 读取文件 / 定时提醒），Bot 启用后经 Function calling 在单聊中「调用 → 执行 → 回填 → 续聊」（最多 3 轮）；技能失败回填错误文本、不中断对话；群聊首版不启用
- **插件页**：侧栏新增「插件」页签，左侧技能库 6 卡片、右侧按 Bot 的技能开关，与 Bot 编辑器的技能多选同一数据

## 架构

```
WPF (net8.0-windows, WebView2, app.local)
  ├─ wwwroot/ (index.html + js/app.js + css/theme.css, 虚拟主机) — 聊天 / Bot / 记忆 / 插件
  ├─ ChatWindow (独立聊天窗口，同一前端 detached 模式 + 独立 WebView2 用户数据目录)
  └─ HTTP → C++ 后端 (127.0.0.1:8765, 回退 8766-8775)
      ├─ SQLite (data/app.db, WAL) — 会话/Bot/群/消息/记忆/技能/提醒/设置
      ├─ 群聊引擎 (每群一个线程的状态机, SSE 广播)
      ├─ 技能执行器 (6 个内置技能注册表, 单聊工具循环 ≤3 轮)
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

输出 `CRUD API TESTS PASSED` 即通过（覆盖 bots/groups CRUD、群 SSE ≥2 轮、插话、会话 bot 绑定、单聊流/重新生成/导出/清空、私聊/群聊记忆提取、记忆 CRUD/搜索、头像上传、memory_extraction 配置、skills/bot_skills 回环、技能全链路（tool_call → 执行 → 回填 → 「计算结果：2」）、reminders 到期查询与完成、chat_pinned/chat_reads 配置等）。测试用独立数据目录 `data_e2e`，不影响 `data/` 用户数据。

### 前端回归 stub 检查

直接评估 `src/AiCompanion.App/wwwroot/js/app.js`，不启动真实后端、不读写 `data/`，全部数据来自脚本内置的假后端 stub：

```cmd
node scripts\checks\check-reminders-ui.mjs
node scripts\checks\check-bot-skills-ui.mjs
node scripts\checks\check-failure-visibility.mjs
```

- `check-reminders-ui.mjs`（109 条断言）：到期提醒的呈现与记账（一轮至多提示一条、被标 done 的那条必须就是 toast 上可见的文案、mark-done 失败不刷屏、呈现失败不记账）、detached 窗口守卫、重叠 tick 单飞、群状态回写与「进行中」徽标、后端重启后重连整表刷新。
- `check-bot-skills-ui.mjs`（54 条断言）：技能未确认加载时禁用勾选并拒绝提交、PUT 单飞且按 Bot 分槽、PUT 失败回滚乐观缓存、Bot 编辑器与插件页共用同一个提交 helper。
- `check-failure-visibility.mjs`（19 条断言）：单聊生成失败后错误提示不被 `refresh-after-chat` 的整表重载擦掉、切走再切回仍跟随该会话且不串会话、下一次成功不留旧疤；Bot 编辑器拦下越界 `max_tokens`（1–65536）与「Bot 模型 + 设置默认模型双空」；群房间重建时把后端落库的 `role=system` 失败留痕渲染成群内提示而非伪装成发言。假流按服务端真实帧序构造（`error` 之后仍会发 `done`），否则测不出「onDone 把刚记下的失败清掉」。

三个脚本都接受 `--app <path>`，把被测文件指向某个历史副本即可验证「修复前的代码必须 FAIL」——断言只有能被旧代码证伪，才算证据。

## 数据存储

`data/app.db`（SQLite，WAL）：

| 表 | 说明 |
|---|---|
| bots | Bot（含 API 覆盖项 api_base_url / api_key_enc / api_model，key 为 DPAPI 密文） |
| sessions | 会话（bot_id 绑定 Bot，0 = 默认） |
| messages | 单聊消息 |
| groups / group_members / group_messages | 群 / 成员（position 即发言顺序）/ 群消息（bot_id 为 NULL = 用户插话或 `role=system` 失败留痕；留痕只给人看，不进模型上下文与记忆提取） |
| settings | 全局默认 API 配置、主题、代理、default_bot_id、memory_extraction（记忆提取开关）、chat_pinned（聊天置顶列表）、chat_reads（未读水位） |
| bot_memories | Bot 记忆（bot_id / scope=private/group / group_id / content / tags / source_msg_id） |
| bot_skills | Bot 技能开关（bot_id / skill_id / enabled / params，UNIQUE(bot_id, skill_id)，随 Bot 级联删除） |
| reminders | 定时提醒（bot_id / content / due_at / done；`reminder` 技能写入，due_at 统一归一化为 UTC 秒级 ISO 8601（`YYYY-MM-DDTHH:MM:SSZ`），到期查询按字符串比较；随 Bot 级联删除） |

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
| GET | /skills | 内置技能库（6 个：id / name / description / params_schema） |
| GET/PUT | /bots/{id}/skills | 该 Bot 技能开关 / 全量覆盖 {skills:[{skill_id, enabled}]}（未知 skill_id 或 skills 非数组 → 400，Bot 不存在 → 404） |
| GET/PUT | /bots/{id}/skills/{sid}/params | 该 Bot 下某技能的默认参数（PUT 只收对象，上限 8KB；技能未启用 → 404）（v0.5.3） |
| GET/POST | /personas | 人设列表 / 创建 {name, role_text, admin_rules, group_display_name, participation, system_override}（v0.6.1） |
| GET/PATCH/DELETE | /personas/{id} | 人设详情 / 编辑（PATCH 只改传入字段）/ 删除（内置 → 400；删除时自动解绑引用它的 Bot）（v0.6.1） |
| PUT | /bots/{id}/persona | 绑定/解绑人设 {persona_id}（0 = 解绑；目标不存在 → 404）（v0.6.1） |
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
| GET | /reminders?due=1 | 提醒列表（due=1 只返回到期未完成；缺省全部） |
| POST | /reminders/{id}/done | 标记提醒完成（不存在 → 404） |
| GET/POST | /config | 全局默认 API 配置 / 主题 / 代理 / 记忆提取开关 / chat_pinned、chat_reads（JSON 字符串直通） |

错误码：1001=默认 Bot 不可删除，1002=群状态冲突，1003=成员数越界（2-10），1004=含停用 Bot。

## 插件

侧栏「插件」页签管理 6 个内置技能（左侧技能库、右侧按 Bot 的开关，与 Bot 编辑器技能多选同一数据）；Bot 启用后在单聊中经 Function calling 调用，执行器与安全约束见 `backend/src/skills.h/.cpp`：

| 技能 | 说明 | 实现与安全约束 |
|---|---|---|
| calculator | 四则运算表达式 | 自制解析器，仅支持 `+ - * / ( )`、数字与空格；非法输入返回错误文本，**无 eval** |
| datetime | 时间日期 | 当前时间 / 日期 / 星期，取本地系统时间，零依赖 |
| sysinfo | 系统信息 | 内存与磁盘占用（GlobalMemoryStatusEx / GetDiskFreeSpaceExW，只读） |
| http_get | HTTP 请求 | 仅公网 http(s)，正文 ≤2KB 截断、10s 超时；拒绝 localhost / `.localhost` / 内网段（10. / 172.16-31. / 192.168. / 169.254. / 0.）/ 回环（127.*、`::1`、`::ffff:` 映射）与数字简写主机（十进制 `2130706433`、十六进制 `0x7f000001`、八进制 `0177.0.0.1`）防 SSRF；**不跟随重定向**（3xx 直接回 `status_code` + `location`，跳转目标不会被访问）；v0.5.0 起对主机名再解析并逐 IP 复检，封死 DNS Rebinding |
| file_read | 读取文件 | 读取指定本地文本文件，≤8KB 截断（按字节切，可能截断多字节字符，回填时按替换策略序列化），失败返回错误文本；**v0.5.0 起限制在 data 目录 / 系统临时目录 / `file_read_allowlist` 白名单内** |
| reminder | 定时提醒 | `due_at` 支持 ISO 8601（含 `Z` / 显式时区偏移，也接受无时区的本地墙钟）与相对分钟（`15分钟后`、裸数字 `30`），统一归一化为 UTC 秒级 ISO 后写入 `reminders` 表；无法解析时返回错误文本且不入库；到期提示由主窗口轮询驱动（见「功能」与验收清单） |

**已接受风险（裁决 #21，v0.5.0 已收敛 `file_read`）**：`http_get` 只保证拒绝内网/回环与数字简写主机、不跟随重定向、主机名解析复检，不做出网域名白名单；主窗口对 `file_read` 结果不做二次确认（现已受目录白名单约束）。

另：`backend/src/event_bus.h` 定义事件总线，内置 Webhook 插件可将事件 POST 到外部地址。事件：`chat.completed` / `chat.error` / `session.created` / `session.deleted`。

### 自制插件（v0.5.2 起可在界面添加）

侧栏「插件」页 →「自制插件」区块 →「+ 添加插件」，填名称、Webhook 地址、勾选订阅事件即可，**保存后立即生效**（后端热重载 EventBus，无需重启）。

| 接口 | 说明 |
|---|---|
| `GET /api/v1/plugins` | 列出全部插件与可选事件 |
| `POST /api/v1/plugins` | 新增；**同名视为更新**（「编辑」复用此接口） |
| `POST /api/v1/plugins/test` | 连通性自检：同步 POST 一条 `plugin.test`，返回 `{ok,status,detail,elapsed_ms}`（v0.5.3） |
| `POST /api/v1/plugins/reorder` | 按 `order` 数组重排投递顺序（v0.5.3） |
| `DELETE /api/v1/plugins/{name}` | 按名称删除 |

校验规则：名称非空且 ≤64 字符；URL 必须以 `http://` 或 `https://` 开头且 ≤2048 字符；事件仅接受上面四个（未知事件直接拒绝，避免配了永不触发的订阅）；最多 32 个插件。配置写入 `%LOCALAPPDATA%\Orua\config.json` 的 `plugins` 数组。

> 此前这些配置只能手改 `config.json` 且必须重启后端才生效，UI 上没有任何入口——这就是「无法增加插件」的根因。

## 打包与更新指引

一条命令完成「构建后端 + 跑 ctest + 发布 WPF 壳 + 部署 backend.exe + 建桌面快捷方式」：

```cmd
scripts\publish.cmd
```

产物在 `dist\Orua\`：`AiCompanion.exe`（启动器/壳）、`backend\backend.exe`、`wwwroot\`。桌面快捷方式 `Orua.lnk` 指向该 exe，工作目录即 `dist\Orua`。

> **用户数据不在产物目录（v0.5.2 起）。** 数据固定存放在 `%LOCALAPPDATA%\Orua\`（通常是 `C:\Users\<你>\AppData\Local\Orua`）：
> `app.db` / `app.db-wal` / `app.db-shm`（Bot、群、聊天记录、技能开关、提醒）、`config.json`（含 DPAPI 加密的 API Key 与插件配置）、`uploads\`（图片头像）、`logs\`。
>
> 因此 `publish.cmd` 的 `rmdir /s /q dist\Orua` **不再影响用户数据**，覆盖安装、换版本都不会丢头像或聊天记录。
>
> 若本机仍是旧布局（`dist\Orua\data` 存在），`publish.cmd` 会先把它整体搬到 `%LOCALAPPDATA%\Orua` 再删产物；WPF 壳首次启动也会做同样的一次性迁移（目标已存在则跳过）。迁移失败时发布会中止而不是删数据。

更新步骤（同一台机器、同一个 Windows 用户）：

```cmd
rem 1) 先退出 Orua（壳退出时会带走它守护的 backend.exe；确认任务管理器里没有残留 backend.exe）
rem 2) 发布（不再需要手工备份/回拷 data）
scripts\publish.cmd
```

如需手动备份（换机器、重装系统前）：

```cmd
xcopy /e /i /y "%LOCALAPPDATA%\Orua" backups\orua-data-2026-11-07
```

`backups\` 已在 `.gitignore` 中（内含 data 副本与 DPAPI 密钥文件，切勿提交）。DPAPI 密文绑定当前 Windows 用户，换机器或换用户后旧 Key 解不开，需要重新填。

发布后核对产物是否为新版（三处版本同源，运行时以 `/health` 为准）：

```cmd
curl http://127.0.0.1:8765/health
powershell -NoProfile -Command "(Get-Item dist\Orua\AiCompanion.exe).VersionInfo | Format-List ProductName,ProductVersion,FileVersion"
```

- `/health` 返回 `{"status":"ok","version":"0.6.1"}`；启动器打开后侧栏脚注显示 `v0.6.1`（boot 时读 `/health` 覆盖，因此脚注反映的是**实际连上的后端**，不是 HTML 里的字面量）
- exe 的 `ProductVersion` / `FileVersion` 来自 `AiCompanion.App.csproj` 的 `<Version>`
- 旧库首次启动会自动迁移：`group_messages` 的 `role` CHECK 放开 `system`（失败留痕），原地重建并保留全部历史行，幂等；日志里会出现 `db: group_messages 重建以放开 role='system'`

改版本号时三处一起改：`AiCompanion.App.csproj` 的 `<Version>`、`backend/src/server.cpp` 的 `/health`、`wwwroot/index.html` 的 `#foot-version` 兜底字面量。

手动发布（不走脚本、不清数据）时的布局要求：

```cmd
dotnet publish src\AiCompanion.App -c Release -r win-x64
```

将 `bin\backend\backend.exe` 放在发布目录的 `backend\` 子目录下（`run_dev.cmd` 的部署步骤即此布局）。

## 已知限制

- **独立聊天窗口不弹提醒 toast**：到期提醒轮询只在主窗口启动，detached 窗口不发轮询、不提示、也不标完成；提醒仍会正常入库，并在主窗口下一次轮询时提示。这是有意限制——避免多个窗口重复提示、重复 `POST /reminders/{id}/done` 竞争。
- **提醒只在应用运行期间提示**：无系统通知、无开机自启调度；应用关闭期间到期的提醒会在下次启动后按每 15s 一条逐轮提示（每轮至多一条，其余排队）。toast 是单元素提示位且 2.2s 自动隐藏，因此 N 条到期需 N×15s 才提示完，每条只有约 2.2s 可见窗，错过的内容没有回看入口（提醒仍会落库，`GET /api/v1/reminders` 可查）。
- **群聊不启用技能**：Function calling 工具循环（≤3 轮）只在单聊生效，群聊发言不调用技能。

## v0.6.1 变更

> 新增「人设」系统：把「这个 Bot 是谁」从 System Prompt 里拆出来，做成可复用、可切换的独立实体。顺带修掉窄窗口下内容区被侧栏挤成 0 的布局缺陷。

1. **人设是独立实体**（`personas_v2` 表）：一条人设可被多个 Bot 共用，也能同时作用于私聊和群聊——这正是参考稿里「为所有的群聊/私聊使用同一个人设」的用法。Bot 通过 `persona_id` 引用（`0` = 未绑定）。
2. **人设真正影响回复**：`db::applyPersonaToPrompt` 把「角色设定 + 管理员附加规则 + Bot 自己的 System Prompt」拼成最终系统提示。拼装顺序是刻意的——角色设定在最前（定义「我是谁」，权重最高），Bot 的提示词在最后（紧贴对话，模型对末尾更敏感）。
3. **群聊也跟人设走**：此前 `group_engine.cpp` 直接用 `bot.system_prompt`，绕过人设，会出现「私聊变了、群里还是老样子」。现在私聊与群聊走同一段拼接逻辑。
4. **人设与记忆叠加而非互斥**：记忆块照旧追加在系统提示之后，顺序为 `人设+提示词 → [长期记忆] → 历史 → 本轮输入`。有单测钉住这个顺序，避免以后改动让记忆盖过人设。
5. **高级「系统提示词覆盖」**：填写后整体替换系统提示（角色设定与附加规则不再参与，避免出现「以为覆盖了、其实混进了别的规则」）。表单里明确写了这会连默认安全规则与工具协议一起失效。
6. **内置人设**：首次建库种入「温暖陪伴 / 理性顾问 / 活跃群友」三条，幂等——只在表为空时插入，用户删掉后不会下次启动又冒出来。内置人设不可删（`deletePersona` 返回 false）。
7. **删除人设自动解绑**：`deletePersona` 先把引用它的 Bot 的 `persona_id` 置 0，不留悬空引用。
8. **人设页**（侧栏新增「人设」页签）：卡片列出人设名、参与度、角色设定摘要，以及「使用中：某某 Bot」——删除前能看到影响面。
9. **Bot 详情新增人设块**：位置在 System Prompt 之上（人设是「他是谁」，提示词是「怎么说话」），显示当前人设摘要与「切换」入口。
10. **修窄窗口布局塌陷**：`#sidebar` 原为 `width:260px; flex:none`，窗口变窄时侧栏不肯收缩，把内容区挤成 **0 宽**——输入框只剩 22px、发送按钮跑出视口。改为 `flex:0 1 260px; min-width:0`，并补 720px / 520px 宽度与 520px 高度三档断点。实测 214×1152 下内容区从 0 → 90px，输入框 22 → 66px。
11. **修 Bot 页右侧整片空白**：有 Bot 但未选中时，`#bots-empty` 与 `#bot-detail` 同时隐藏，右半边什么都没有。现在自动落到第一个 Bot；搜索无结果时文案区分「没有匹配」与「还没有 Bot」。
12. **修记忆页溢出**：`grid-template-columns: 240px 1fr` 在窄容器下把右列挤出视口，「刷新」按钮被裁掉；`#view-memory` 此前完全没有容器样式，标题贴着窗口边缘。改用 `minmax(0,1fr)` 并补内边距与三档断点。
13. **人设弹窗**：字段对齐参考稿（人设名 / 群内展示名 / 参与度 / 角色设定 / 管理员附加规则 / 系统提示词覆盖）。字段较多，改为内容区滚动 + 底部按钮吸附，任何窗口高度下都能直接保存。

### v0.6.1

1. 侧栏出现「人设」页签，首次启动有 3 条内置人设，带「内置」标记且不可删除。
2. 「＋ 新建人设」打开弹窗，六个字段齐全；人设名为空或「角色设定与覆盖都为空」时拦在提交前并给出原因。
3. 新建的人设立即出现在列表，可编辑、可删除；删除前提示哪些 Bot 正在使用它。
4. Bot 详情页显示「当前人设」，未绑定时说明「未绑定人设 — 只使用下面的 System Prompt」；点「切换」可绑定/解绑。
5. 绑定后该 Bot 的私聊与群聊都按人设回复；解绑后行为回到绑定前。
6. 「系统提示词覆盖」填写后整体替换系统提示；留空则走拼接，Bot 原提示词不被丢弃。
7. 删除人设后，原引用它的 Bot `persona_id` 归 0，不残留悬空引用。
8. 窗口拖到很窄（如 900×620）时，侧栏与内容区都可用，输入框与发送按钮完整可见，无横向溢出。
9. 后端 `backend_tests` 109 用例 / 883 断言全通过；三个 stub 闸门 109 / 54 / 19 全通过；人设端到端 `e2e_persona_v061.mjs` 16 条全通过。
10. 版本号三处同源 `0.6.1`：exe 文件版本 `0.6.1.0`、`/health` 返回 `0.6.1`、侧栏脚注显示 `v0.6.1`。

## v0.5.3 变更

> 插件页按参考稿重做：卡片化布局、齿轮入口、连接自检。核心是把「插件/技能」从一张侧栏小列表升级成可管理的一页。

1. **插件页重做**（`#view-plugins`）：标题带英文副标、右上「+ 添加插件」、搜索框（按名称/简介过滤）、`N / M 生效` 计数徽章、「刷新」。内容分两组——「内置技能」与「自制插件」，各自带条数，卡片按参考稿排布：名称 + 状态标签 + 操作按钮、能力 id 药丸、简介。
2. **齿轮入口**：每张卡片右上角有齿轮按钮。技能卡打开「技能参数」弹窗，可改该 Bot 下这个技能的默认参数；插件卡打开「插件设置」弹窗，可改订阅事件、限定生效 Bot（多选）、临时停用。
3. **插件可限定生效 Bot**：`config.json` 里插件新增 `bot_ids` 数组，空数组表示「全部 Bot」（保持 v0.5.2 语义，升级不需要迁移）。`EventBus::emit` 按事件里的 `bot_id` 过滤。
4. **插件可临时停用**：新增 `enabled` 字段。停用后不再投递，但配置保留，卡片以 `已停用` 标签和降低的不透明度区分。
5. **「测试连接」按钮**：`POST /api/v1/plugins/test` 同步 POST 一条 `plugin.test` 事件，2 秒超时，把 HTTP 状态码和响应片段回显在弹窗里。超时与正式投递一致，避免「测着通、发不出去」。
6. **插件可排序**：`POST /api/v1/plugins/reorder`，卡片上的 ↑ 按钮调整投递顺序。
7. **修 `parseUrl` 的 https 缺陷**：此前只剥 `http://`，`https://example.com/x` 会把 host 解析成 `https:`、path 变成 `//example.com/x`，投递必然失败。现在正确剥离并按 TLS 走 `WINHTTP_FLAG_SECURE`。
8. **Webhook 投递改走 WinHTTP**：本项目 httplib 编译时未启用 OpenSSL（没有 `SSLClient`），https 地址根本无法投递。改用 WinHTTP 后同时获得 https 支持与系统代理复用，与 LLM 请求行为一致。顺带把「只有 200 算成功」放宽为 2xx。
9. **修技能参数被清空的缺陷**：`setBotSkills` 每次保存都把 `params` 重置成 `{}`，导致重新勾选技能就丢掉已配参数。改为保存前先读回既有参数并保留。
10. **技能参数可按 Bot 配置**：新增 `GET`/`PUT /api/v1/bots/{id}/skills/{sid}/params`。执行技能时把已存参数作为默认值，模型显式传的同名参数优先。
11. **加载失败不给出可提交状态**：技能状态未从服务端确认时，勾选框与齿轮置灰，强行触发也会拒绝保存——否则会拿空集合覆盖服务端已有技能。保存失败同样撤销信任，需点「刷新」恢复。

### v0.5.3

1. 插件页标题、搜索、`N / M 生效` 徽章、「刷新」、「+ 添加插件」齐全，两组（内置技能 / 自制插件）各带条数。
2. 搜索框按名称/简介过滤，清空后恢复全部。
3. 每张卡片有齿轮按钮；技能卡打开参数弹窗，插件卡打开设置弹窗（订阅事件 + 生效 Bot 多选 + 启用开关）。
4. 「生效 Bot」不选 = 全部 Bot；选了则卡片显示具体 Bot 名。
5. 「测试连接」对可达地址返回 `ok:true` 与耗时；对不可达地址返回失败原因，不阻塞界面。
6. ↑ 调整插件顺序并持久化；首项按钮禁用。
7. 插件停用后卡片显示「已停用」，且不再投递事件。
8. 重新勾选技能后，此前保存的参数仍在。
9. https 地址可被正确解析并投递。
10. 后端 `backend_tests` 102 用例 / 827 断言全通过；三个 stub 闸门 109 / 54 / 19 全通过；`smoke_plugins_v053.mjs` 21 条、`smoke_v053_features.mjs` 13 条全通过。
11. 版本号三处同源 `0.5.3`：exe 文件版本 `0.5.3.0`、`/health` 返回 `0.5.3`、侧栏脚注显示 `v0.5.3`。

## v0.5.2 变更

> **版本号规则变更**：自 v0.5.2 起全部版本改用 `0.x.y` 语义（此前为 `1`–`5.1`）。脚注改为完整三段显示（`v0.5.2`），不再截断。

1. **头像与数据持久化（修复「更新后头像消失」）**：数据目录从 `dist\Orua\data` 迁到 `%LOCALAPPDATA%\Orua`。此前 `publish.cmd` 会 `rmdir /s /q dist\Orua` 重建产物，把 `app.db`、`uploads\*.png`（头像）、`config.json` 一并删掉——用户每次更新都丢头像。现在数据与产物分离，覆盖安装不再触碰用户数据；首次启动自动从旧位置一次性迁移（含 WAL/SHM 重建）。
2. **插件 UI 重做**：原布局固定 240px 左栏，把「计算器」「定时提醒」压成断字，且 checkbox 被全局 `input{width:100%}` 撑满整行、技能名挤成「计...」。现改为「主区自适应 + 280px 侧栏」两列，技能卡片紧凑网格（描述限 2 行），标题栏按钮右对齐。
3. **修复无法添加插件**：`config.json` 里一直有 `plugins` 数组，但**没有任何接口能读写它**——用户想加自制插件只能手改 JSON 并重启。新增 `GET/POST/DELETE /api/v1/plugins`：
   - 同名 POST 视为更新（「编辑」复用同一接口）
   - 校验：名称非空且 ≤64 字符、URL 必须 `http(s)://`、事件限定在 `chat.completed` / `chat.error` / `session.created` / `session.deleted`、上限 32 个插件
   - 每次写操作回写 `config.json` 并**热重载 EventBus**，无需重启后端
   - 前端新增「+ 添加插件」按钮与弹窗（名称 / Webhook 地址 / 事件多选），列表支持编辑与删除
4. **头像缺失不再破图**：`<img>` 加 `onerror` 回退 emoji 占位，并附 `?v=avatar_path` 缓存破坏参数（换头像后立即生效）。消除此前控制台的 `ERR_BLOCKED_BY_ORB` 噪音。
5. **`publish.cmd` 数据安全**：迁移逻辑抽为独立子程序（cmd 的 `rem` 遇全角括号会解析失败），robocopy 失败时中止发布而非删数据。
6. **新增 `scripts\smoke_plugins.mjs`**：真实 GUI 里点「添加插件」→ 填表 → 保存 → 校验拦截 → 删除的完整链路断言，并把布局（标题不断行、区块可见）纳入检查。

## v0.5.1 变更

> v0.5.1 是 **v0.5.0 实机验收的缺陷修复版**。核心是一个从 v0.1.0 潜伏至今、所有自动化测试都没抓到的 bug。

1. **聊天请求漏拼 `API_BASE`（致命）**：`consumeSse()` 从 v0.1.0 起用裸相对路径 `fetch(url)`，而页面由 WebView2 以虚拟主机 `https://app.local` 提供，请求因此被解析成 `https://app.local/api/v1/chat/stream` → `net::ERR_FILE_NOT_FOUND`，界面只显示 `Failed to fetch`——**单聊与重新生成全部不可用**。`App.api.req` 一直是 `API_BASE + path`，只有 `consumeSse` 漏拼。
   - 为何长期未被发现：所有既有测试要么用相对路径 stub，要么用 curl 直打后端，**从未在真实 WebView2 里点过「发送」**。
   - 修复：`consumeSse(path, ...)` 内部改为 `fetch(API_BASE + path)`；并把「真实点击发送且不出现 Failed to fetch」固化成 `scripts/smoke_gui.mjs` 的断言。
2. **`build_backend.cmd` 在中文路径下误报 cmake 缺失**：VS 安装路径含中文（`D:\...\VS\本体`）时，`vswhere` 按 UTF-8 输出而 cmd 默认代码页转码成乱码，`if not exist` 判定失败，导致 `publish.cmd` 无法一键构建。修复：脚本开头 `chcp 65001`。
3. **版本号升至 0.5.1.0**：exe 文件/产品版本、`/health`、侧栏脚注三处同源。
4. **`smoke_gui.mjs` 支持 `CDP_PORT` 环境变量**，不再硬编码 9222，便于与其它调试实例并存。

## v0.5.0 变更

1. **`file_read` 目录白名单**：仅允许读取 data 目录、系统临时目录，以及设置表 `file_read_allowlist`（分号分隔的绝对根目录）显式追加的路径；8KB 硬截断不变，越界路径直接拒绝。
2. **本地 API 令牌 + DNS-Rebinding 防护**：后端每次启动生成 32 字节随机令牌写入 `config.json` 与请求校验；WPF 启动注入并在后端重启（含端口回退）时重推 `auth-token` 消息，前端对 `/api/v1/*` 请求回传 `X-Orua-Token`。仅当请求带跨域 `Origin` 且令牌已启用时拦截，本机直连（curl / `test_api.ps1`）不受影响。
3. **`http_get` 解析复检**：hostname 通过 `isBlockedHost` 后，仍解析出全部真实 IP 并逐个复检，封死「先公网、后切回内网」的 rebinding；不跟随重定向。
4. **服务端参数强校验**：`max_tokens`（1–65536）、`temperature`（0–2）、`keep_rounds`（0–100）、`proxy_port`（0–65535）均在后端收窄，绕过前端的越界值不会直接落库。
5. **event_bus Webhook 并发节流**：有界分派（并发上限 8、队列上限 256），突发 `emit` 不再 scale 出无界线程。

## v0.4.1 变更

> v0.4.1 不是新功能版本，而是 **v0.4.0 终验后的缺陷收口版本**：把「验证通过但用户看不见」与「失败被静默吞掉」两类问题从根上修掉。设计规格见 `docs/superpowers/specs/2026-11-06-orua-v0.4.1-design.md`。

1. **失败必须可见**：单聊失败记入 `App.state.chatFailure`，`loadMessages` 重载后按会话重画错误气泡，清疤只在「开始新一次生成」；群聊失败原因与自动暂停原因以 `role=system`、`bot_id=NULL` 落库，重开房间可见，且不占「最近 12 条」上下文、不进记忆提取快照。
2. **`group_messages` 放开 `role='system'`**：`CHECK` 约束不能 `ALTER`，故加一次性原地重建迁移（`PRAGMA foreign_keys=OFF` → 改名 → 新 DDL 建表 → 显式列名整行搬回 → 删旧表），幂等且数据零丢失；DDL 抽成 `kGroupMessagesDdl` 常量供建库与迁移共用。
3. **假闭合清零**：到期提醒改为「一个 tick 只允许一次 `toast()`」，取 `due_at` 升序最早一条呈现，其余条数并成「（另有 N 条待提醒）」后缀，**该条真落地才记账并标完成**；群「进行中」徽标新增 `syncGroupStatus()` 同时回写面板态与列表数据源并重渲染，后端重连时经 `resyncGroups()` 串行整表刷新（串行是必需的——并发时 `renderChatList` 会快照到陈旧数组把旧徽标写回去）。
4. **Bot 编辑器提交前校验**：`max_tokens` 限 1–65536 整数（留空仍 2048），模型名与设置默认模型双空时拦在提交前并指出去哪儿填；`input` 补 `max` 属性。
5. **版本三处同源 `0.4.1.0`**：exe 文件/产品版本、`/health`、侧栏脚注统一；脚注 boot 时读 `/health` 覆盖，反映实际连上的后端产物，避免 UI 与产物各说各话。
6. **可证伪闸门**：三个 stub 检查脚本（`check-reminders-ui.mjs` 109 条 / `check-bot-skills-ui.mjs` 54 条 / `check-failure-visibility.mjs` 19 条）固化进 `scripts/checks/`，断言必须能被旧代码证伪——修复前副本跑应有 FAIL，修复后全 PASS。

## 验收清单

### v0.6.1

1. 侧栏出现「人设」页签，首次启动有 3 条内置人设，带「内置」标记且不可删除。
2. 「＋ 新建人设」打开弹窗，六个字段齐全；人设名为空或「角色设定与覆盖都为空」时拦在提交前并给出原因。
3. 新建的人设立即出现在列表，可编辑、可删除；删除前提示哪些 Bot 正在使用它。
4. Bot 详情页显示「当前人设」，未绑定时说明「未绑定人设 — 只使用下面的 System Prompt」；点「切换」可绑定 / 解绑。
5. 绑定后该 Bot 的私聊与群聊都按人设回复；解绑后行为回到绑定前。
6. 「系统提示词覆盖」填写后整体替换系统提示；留空则走拼接，Bot 原提示词不被丢弃。
7. 删除人设后，原引用它的 Bot `persona_id` 归 0，不残留悬空引用。
8. 窗口拖到很窄（如 900×620）时，侧栏与内容区都可用，输入框与发送按钮完整可见，无横向溢出。
9. 后端 `backend_tests` 109 用例 / 883 断言全通过；三个 stub 闸门 109 / 54 / 19 全通过；`e2e_persona_v061.mjs` 16 条全通过。
10. 版本号三处同源 `0.6.1`：exe 文件版本 `0.6.1.0`、`/health` 返回 `0.6.1`、侧栏脚注显示 `v0.6.1`。

### v0.1.0

1. 窗口能启动，显示「Orua」标题；标题栏拖动、双击最大化 / 还原、右键系统菜单可用。
2. 能配置 API（地址 / Key / 模型）并保存；Key 以掩码返回（`sk-***abc`）。
3. 能创建 Bot 并与它进行 1v1 流式对话，回复逐字出现。
4. 重启程序后配置与历史消息仍在。
5. `/health` 返回 `{"status":"ok","version":"0.1.0.0"}`；侧栏脚注显示 `v0.1.0`。

### v0.2.0

1. 窗口标题与 UI 显示「Orua」；标题栏拖动、双击最大化/还原、右键系统菜单可用
2. 创建 ≥3 个 Bot（含不同 API 覆盖），新建 1v1 会话可任选 Bot 流式对话
3. 创建含 2-10 个 Bot 的群，点开聊后 Bot 按顺序自动连聊、消息流式展示；暂停 / 继续 / 停止 / 插话 / 换话题全部可用
4. 某 Bot API 不可用时该轮显示失败气泡、群聊不中断；连续 3 次失败自动暂停，失败原因与暂停原因落库，重开房间仍可见
5. 重启程序后：Bot / 群 / 历史消息完整；群状态复位 idle 可重新开聊
6. v0.1.0 全部既有功能回归通过；ctest 全绿 + test_api.ps1 全绿

### v0.3.0

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
11. v0.2.0 全部功能与自动化测试保持全绿；老库迁移零破坏

### v0.4.0

1. 侧栏页签为「聊天 / Bot / 记忆 / 插件」；聊天页单聊与群聊同一列表，QQ 好友式布局（置顶区 + 按最近活跃排序）
2. 每个 Bot 一个聊天项，点击进入其最近会话（无会话时自动创建）；同一 Bot 的多会话入口在右键菜单
   - 终验修复波后已覆盖：历史会话子菜单每个会话两行——标题行点击即打开（当前会话带「当前」标记），动作行为 默认 / 重命名 / 清空 / 导出 / 删除，`清空`/`导出` 已支持作用于非当前会话
3. 右键菜单按项类型（Bot / 群）显示正确项集；删除有二次确认；默认 Bot 删除返回 1001 提示
4. 置顶与未读红点生效，重启后保留（`chat_pinned` / `chat_reads`）；群聊项在群 `status = running` 时显示「进行中」徽标
   - 第二轮修复后已覆盖：群状态变更（SSE `status` 事件、开始/暂停/继续/停止）统一经 `syncGroupStatus()` 同时回写 `App.state.groups[].status` 并重渲染聊天列表，徽标随运行状态出现/消失（stub 断言 J 组）；后端重启会把群复位 `idle`，前端在重连时经 `resyncGroups()` 整表刷新纠陈旧徽标。限定：本版本群只能从房间面板启动，未开过房间的群按整表刷新（启动 / 保存群 / 删群 / 重连）同步；离开房间后该群的 `status` 事件停止投递，若它期间在服务端自行停止，徽标会滞留到最后已知值，重新开房或后端重连即纠正
5. 独立聊天窗口：打开 / 发送 / 同步 / 主窗口关闭联动可用；同一目标重复打开聚焦已有窗口
   - 终验修复波后已覆盖：窗口标题取 Bot 名 / 群名（壳侧剔除控制字符并截断 40 字符，名称缺失回退「聊天」/「群聊」，初始化失败标题优先）；重复打开时若旧窗口被最小化，先恢复再聚焦
6. 插件页展示 6 个技能卡片；Bot 技能开关与 Bot 编辑器勾选同步生效
7. 单聊中 Bot 调用技能并给出正确结果（真实 API 或 `LLM_MOCK=1` 均可验证，计算器返回「计算结果：2」）
   - 终验修复波后已覆盖：技能抛异常或结果含非法 UTF-8 时按替换策略序列化并回填 `{"error": "技能执行失败: …"}`，每个 tool_call 都有对应 tool 消息，对话继续生成且助手消息照常落库；流式回调退出（含异常）即清理该会话 stop 标记
8. 提醒到期前端弹出提示；HTTP 技能拒绝内网地址；文件技能超过 8KB 截断
   - 第二轮修复后已覆盖：`reminder` 写入带 Bot 归属（`bot_id` 取自会话），`due_at` 归一化为 UTC 秒级 ISO 8601；主窗口 15s 轮询到期提醒，每个周期只发一次 toast——呈现最早那条的正文，本轮仍有待发时把条数并成「（另有 N 条待提醒）」后缀，只有这条真落到 toast 元素上才记账并标完成，其余排队到下一轮（不再出现「正文被计数条覆盖」的静默丢失）
   - `http_get` 另拒绝数字简写/八进制/前导零与 IPv6 字面量（含 `[::ffff:…]`、`::1` 映射），并禁用自动重定向
9. 失败必须可见：单聊生成失败后错误提示留在对话区（不被整表重载擦掉、切走再切回仍跟随该会话、下一次成功不留旧疤）；群聊失败原因落库，重开房间能看到为什么停了；Bot 编辑器拦下越界 `max_tokens` 与「模型名 + 设置默认模型双空」
   - 实机验收修复波（F12）后已覆盖：单聊失败记在 `App.state.chatFailure`，`loadMessages` 重载后按会话重画错误气泡；清疤只发生在「开始新一次生成」——后端失败时仍会发 `event: done`，在 `onDone` 清疤会把刚记下的失败抹掉（真实浏览器实测抓到，stub 假流已按该帧序构造）。群聊失败以 `role=system`、`bot_id=NULL` 落库（旧库 CHECK 约束经一次性重建放开），前端重建房间时渲染成群内提示，且留痕不占「最近 12 条」上下文窗口、不进记忆提取快照。`max_tokens` 限 1–65536 整数（留空仍 2048），越界与双空模型均拦在提交前并给出可操作提示
10. v0.1.0 / v0.2.0 / v0.3.0 功能与自动化测试全绿；旧库升级零破坏

> 第 8 项备注：`reminder` 技能入库、`/api/v1/reminders?due=1` 到期查询与完成标记已由自动化覆盖（ctest + test_api.ps1）。前端已实现到期轮询与提示（主窗口 15s 轮询 → 每周期至多一条 toast → 该条落地才标完成，未见不标完成，未提示的排队到下一轮），独立聊天窗口按设计不参与提示（见「已知限制」）。实机验收仍建议以「HTTP 拒绝内网」「文件 8KB 截断」两项现场观察为准。

### v0.4.1

1. 单聊生成失败后错误提示留在对话区（不被整表重载擦掉、切走再切回仍跟随该会话、下一次成功不留旧疤）
2. 群聊失败原因落库，重开房间能看到为什么停了；留痕渲染成群内提示，不伪装成用户 / Bot 消息
3. Bot 编辑器拦下越界 `max_tokens`（1–65536 整数，留空仍 2048）与「模型名 + 设置默认模型双空」，提示可操作
4. 到期提醒每 tick 至多一条 toast 真正可见；正文与待发计数合并进同一条；未见不标完成，其余排队到下一轮
5. 群「进行中」徽标随运行状态实时出现 / 消失；后端重启后陈旧徽标被整表刷新清除；持续在线不重复刷新（无抖动）；detached 窗口不代主窗口刷新
6. 版本三处统一 `0.4.1.0`（exe 文件/产品版本、`/health`、侧栏脚注），脚注 boot 时读 `/health` 覆盖
7. README 含「打包与更新指引」，明确 `publish.cmd` 会连 `data` 一起清掉、需备份与回拷
8. 三个 stub 闸门全 PASS（修复前副本应 FAIL）：提醒 109 条、技能 54 条、失败可见性 19 条
9. `ctest` 全绿（v0.4.1 当时基线 82 用例 / 695 断言；v0.5.0 追加 `test_v5.cpp` 后为 91 / 777）；`test_api.ps1` PASSED；WPF 构建 0 警告 0 错误；v0.1.0–v0.4.0 功能与自动化测试全绿；旧库升级零破坏

### v0.5.0

1. `file_read` 仅允许 data 目录 / 系统临时目录 / `file_read_allowlist` 白名单，越界路径拒绝（ctest 覆盖）
2. 后端每启动生成随机令牌；前端经 WPF 注入后对 `/api/v1/*` 请求回传 `X-Orua-Token`；带跨域 Origin 且令牌已启用时校验，无 Origin 直连不受影响（test_api.ps1 全绿）
3. `http_get` 主机名解析后逐 IP 复检（DNS-Rebinding），不跟随重定向，SSRF 约束不回归（ctest 覆盖）
4. `max_tokens` / `temperature` / `keep_rounds` / `proxy_port` 服务端收窄（ctest 覆盖）
5. event_bus Webhook 有界分派（并发 ≤8、队列 ≤256），单次 emit 仍 ≤400ms 送达（ctest 覆盖）
6. v0.1.0–v0.4.1 功能与自动化测试全绿；版本号三处 0.5.0.0

### v0.5.1

1. 单聊「发送」在真实 WebView2 中可用：请求打到 `http://127.0.0.1:<port>/api/v1/chat/stream`，不再出现 `Failed to fetch`（`smoke_gui.mjs` 真实点击断言覆盖）
2. 「重新生成」可用（与单聊同走 `consumeSse`，同一修复）
3. 全部 5 处 `fetch` 均以 `API_BASE` 开头，无裸相对路径请求
4. `scripts\build_backend.cmd` 在 VS 安装路径含中文时能正确找到 cmake/ctest，`publish.cmd` 一键跑通
5. `dist\Orua\AiCompanion.exe` 文件版本为 `0.5.1.0`、产品名 `Orua`；`/health` 返回 `0.5.1.0`；侧栏脚注显示 `v0.5.1`
6. `smoke_gui.mjs` 支持 `CDP_PORT` 环境变量
7. v0.1.0–v0.5.0 功能与自动化测试全绿（ctest 91 用例 / 777 断言、三个 stub 闸门 109/54/19、`test_api.ps1`）

### v0.5.2

1. 数据目录为 `%LOCALAPPDATA%\Orua`，`dist\Orua` 内不含用户数据；旧布局首次启动自动一次性迁移（WAL/SHM 重建），`publish.cmd` 覆盖安装不再删除 `app.db` / `uploads`（头像）/ `config.json`
2. 头像持久化：上传后重启应用仍显示；`<img>` 缺失时回退 emoji 占位，控制台无 `ERR_BLOCKED_BY_ORB` 破图噪音；换头像因 `?v=avatar_path` 立即生效
3. 插件页布局：标题与「+ 添加插件」按钮同行右对齐；技能卡片两列网格、标题单行不折断；「自制插件」区块在首屏可见（`smoke_plugins.mjs` 断言）
4. 可添加自制插件：UI 填表 → 保存 → 列表出现 → 重启后仍在（`smoke_plugins.mjs` 真实点击 + 后端持久化测试覆盖）
5. 插件校验：空名称 / 非 http(s) URL / 未知事件 / 超过 32 个 均被拒绝并给出中文提示；同名 POST 走更新而非重复新增
6. 插件热重载：新增/删除后日志出现新的 `plugins loaded: N`，无需重启后端
7. 删除插件后列表与 `config.json` 同步移除
8. 版本号三处同源 `0.5.2`：exe 文件版本 `0.5.2.0`、`/health` 返回 `0.5.2`、侧栏脚注完整显示 `v0.5.2`（不再截断成 `v0.5`）
9. v0.1.0–v0.5.1 功能与自动化测试全绿（ctest 95 用例 / 793 断言、三个 stub 闸门 109/54/19、`test_api.ps1`、`smoke_gui.mjs`、`smoke_plugins.mjs`）
### v0.5.3

1. 插件页标题、搜索、`N / M 生效` 徽章、「刷新」、「+ 添加插件」齐全，两组（内置技能 / 自制插件）各带条数。
2. 搜索框按名称 / 简介过滤，清空后恢复全部。
3. 每张卡片有齿轮按钮；技能卡打开参数弹窗，插件卡打开设置弹窗（订阅事件 + 生效 Bot 多选 + 启用开关）。
4. 「生效 Bot」不选 = 全部 Bot；选了则卡片显示具体 Bot 名。
5. 「测试连接」对可达地址返回 `ok:true` 与耗时；对不可达地址返回失败原因，不阻塞界面。
6. ↑ 调整插件顺序并持久化；首项按钮禁用。
7. 插件停用后卡片显示「已停用」，且不再投递事件。
8. 重新勾选技能后，此前保存的参数仍在。
9. https 地址可被正确解析并投递。
10. 后端 `backend_tests` 102 用例 / 827 断言全通过；三个 stub 闸门 109 / 54 / 19 全通过；`smoke_plugins_v053.mjs` 21 条、`smoke_v053_features.mjs` 13 条全通过。
11. 版本号三处同源 `0.5.3`：exe 文件版本 `0.5.3.0`、`/health` 返回 `0.5.3`、侧栏脚注显示 `v0.5.3`。

