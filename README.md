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
| http_get | HTTP 请求 | 仅公网 http(s)，正文 ≤2KB 截断、10s 超时；拒绝 localhost / `.localhost` / 内网段（10. / 172.16-31. / 192.168. / 169.254. / 0.）/ 回环（127.*、`::1`、`::ffff:` 映射）与数字简写主机（十进制 `2130706433`、十六进制 `0x7f000001`、八进制 `0177.0.0.1`）防 SSRF；**不跟随重定向**（3xx 直接回 `status_code` + `location`，跳转目标不会被访问）；v5 起对主机名再解析并逐 IP 复检，封死 DNS Rebinding |
| file_read | 读取文件 | 读取指定本地文本文件，≤8KB 截断（按字节切，可能截断多字节字符，回填时按替换策略序列化），失败返回错误文本；**v5 起限制在 data 目录 / 系统临时目录 / `file_read_allowlist` 白名单内** |
| reminder | 定时提醒 | `due_at` 支持 ISO 8601（含 `Z` / 显式时区偏移，也接受无时区的本地墙钟）与相对分钟（`15分钟后`、裸数字 `30`），统一归一化为 UTC 秒级 ISO 后写入 `reminders` 表；无法解析时返回错误文本且不入库；到期提示由主窗口轮询驱动（见「功能」与验收清单） |

**已接受风险（裁决 #21，v5 已收敛 `file_read`）**：`http_get` 只保证拒绝内网/回环与数字简写主机、不跟随重定向、主机名解析复检，不做出网域名白名单；主窗口对 `file_read` 结果不做二次确认（现已受目录白名单约束）。

另：`backend/src/event_bus.h` 定义事件总线，内置 Webhook 插件可将事件 POST 到外部地址。事件：`chat.completed` / `chat.error` / `session.created` / `session.deleted`。

## 打包与更新指引

一条命令完成「构建后端 + 跑 ctest + 发布 WPF 壳 + 部署 backend.exe + 建桌面快捷方式」：

```cmd
scripts\publish.cmd
```

产物在 `dist\Orua\`：`AiCompanion.exe`（启动器/壳）、`backend\backend.exe`、`wwwroot\`。桌面快捷方式 `Orua.lnk` 指向该 exe，工作目录即 `dist\Orua`。

> **更新前必须备份数据。** `publish.cmd` 会先 `rmdir /s /q dist\Orua` 清掉旧产物，**连同 `dist\Orua\data\` 一起删除**——里面有 `app.db` / `app.db-wal` / `app.db-shm`（Bot、群、聊天记录、技能开关、提醒）、`config.json`（含 DPAPI 加密的 API Key）、`uploads\`（图片头像）、`logs\`。不备份就等于清空用户数据。

更新步骤（同一台机器、同一个 Windows 用户）：

```cmd
rem 1) 先退出 Orua（壳退出时会带走它守护的 backend.exe；确认任务管理器里没有残留 backend.exe）
rem 2) 整目录备份 data（三个 db 文件必须一起拷，WAL 里可能还有未 checkpoint 的数据）
xcopy /e /i /y dist\Orua\data backups\dist-data-2026-11-07-pre-publish
rem 3) 发布
scripts\publish.cmd
rem 4) 想保留原有 Bot / Key / 聊天记录就把 data 拷回去；想要干净起步则跳过这一步
xcopy /e /i /y backups\dist-data-2026-11-07-pre-publish dist\Orua\data
```

`backups\` 已在 `.gitignore` 中（内含 data 副本与 DPAPI 密钥文件，切勿提交）。DPAPI 密文绑定当前 Windows 用户，换机器或换用户后旧 Key 解不开，需要重新填。

发布后核对产物是否为新版（三处版本同源，运行时以 `/health` 为准）：

```cmd
curl http://127.0.0.1:8765/health
powershell -NoProfile -Command "(Get-Item dist\Orua\AiCompanion.exe).VersionInfo | Format-List ProductName,ProductVersion,FileVersion"
```

- `/health` 返回 `{"status":"ok","version":"5.0.0"}`；启动器打开后侧栏脚注显示 `v5.0`（boot 时读 `/health` 覆盖，因此脚注反映的是**实际连上的后端**，不是 HTML 里的字面量）
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

## v4.1 变更

> v4.1 不是新功能版本，而是 **v4 终验后的缺陷收口版本**：把「验证通过但用户看不见」与「失败被静默吞掉」两类问题从根上修掉。设计规格见 `docs/superpowers/specs/2026-11-06-orua-v4.1-design.md`。

1. **失败必须可见**：单聊失败记入 `App.state.chatFailure`，`loadMessages` 重载后按会话重画错误气泡，清疤只在「开始新一次生成」；群聊失败原因与自动暂停原因以 `role=system`、`bot_id=NULL` 落库，重开房间可见，且不占「最近 12 条」上下文、不进记忆提取快照。
2. **`group_messages` 放开 `role='system'`**：`CHECK` 约束不能 `ALTER`，故加一次性原地重建迁移（`PRAGMA foreign_keys=OFF` → 改名 → 新 DDL 建表 → 显式列名整行搬回 → 删旧表），幂等且数据零丢失；DDL 抽成 `kGroupMessagesDdl` 常量供建库与迁移共用。
3. **假闭合清零**：到期提醒改为「一个 tick 只允许一次 `toast()`」，取 `due_at` 升序最早一条呈现，其余条数并成「（另有 N 条待提醒）」后缀，**该条真落地才记账并标完成**；群「进行中」徽标新增 `syncGroupStatus()` 同时回写面板态与列表数据源并重渲染，后端重连时经 `resyncGroups()` 串行整表刷新（串行是必需的——并发时 `renderChatList` 会快照到陈旧数组把旧徽标写回去）。
4. **Bot 编辑器提交前校验**：`max_tokens` 限 1–65536 整数（留空仍 2048），模型名与设置默认模型双空时拦在提交前并指出去哪儿填；`input` 补 `max` 属性。
5. **版本三处同源 `4.1.0`**：exe 文件/产品版本、`/health`、侧栏脚注统一；脚注 boot 时读 `/health` 覆盖，反映实际连上的后端产物，避免 UI 与产物各说各话。
6. **可证伪闸门**：三个 stub 检查脚本（`check-reminders-ui.mjs` 109 条 / `check-bot-skills-ui.mjs` 54 条 / `check-failure-visibility.mjs` 19 条）固化进 `scripts/checks/`，断言必须能被旧代码证伪——修复前副本跑应有 FAIL，修复后全 PASS。

## v5.1 变更

> v5.1 是 **v5.0 实机验收的缺陷修复版**。核心是一个从 v1 潜伏至今、所有自动化测试都没抓到的 bug。

1. **聊天请求漏拼 `API_BASE`（致命）**：`consumeSse()` 从 v1 起用裸相对路径 `fetch(url)`，而页面由 WebView2 以虚拟主机 `https://app.local` 提供，请求因此被解析成 `https://app.local/api/v1/chat/stream` → `net::ERR_FILE_NOT_FOUND`，界面只显示 `Failed to fetch`——**单聊与重新生成全部不可用**。`App.api.req` 一直是 `API_BASE + path`，只有 `consumeSse` 漏拼。
   - 为何长期未被发现：所有既有测试要么用相对路径 stub，要么用 curl 直打后端，**从未在真实 WebView2 里点过「发送」**。
   - 修复：`consumeSse(path, ...)` 内部改为 `fetch(API_BASE + path)`；并把「真实点击发送且不出现 Failed to fetch」固化成 `scripts/smoke_gui.mjs` 的断言。
2. **`build_backend.cmd` 在中文路径下误报 cmake 缺失**：VS 安装路径含中文（`D:\...\VS\本体`）时，`vswhere` 按 UTF-8 输出而 cmd 默认代码页转码成乱码，`if not exist` 判定失败，导致 `publish.cmd` 无法一键构建。修复：脚本开头 `chcp 65001`。
3. **版本号升至 5.1.0**：exe 文件/产品版本、`/health`、侧栏脚注三处同源。
4. **`smoke_gui.mjs` 支持 `CDP_PORT` 环境变量**，不再硬编码 9222，便于与其它调试实例并存。

## v5 变更

1. **`file_read` 目录白名单**：仅允许读取 data 目录、系统临时目录，以及设置表 `file_read_allowlist`（分号分隔的绝对根目录）显式追加的路径；8KB 硬截断不变，越界路径直接拒绝。
2. **本地 API 令牌 + DNS-Rebinding 防护**：后端每次启动生成 32 字节随机令牌写入 `config.json` 与请求校验；WPF 启动注入并在后端重启（含端口回退）时重推 `auth-token` 消息，前端对 `/api/v1/*` 请求回传 `X-Orua-Token`。仅当请求带跨域 `Origin` 且令牌已启用时拦截，本机直连（curl / `test_api.ps1`）不受影响。
3. **`http_get` 解析复检**：hostname 通过 `isBlockedHost` 后，仍解析出全部真实 IP 并逐个复检，封死「先公网、后切回内网」的 rebinding；不跟随重定向。
4. **服务端参数强校验**：`max_tokens`（1–65536）、`temperature`（0–2）、`keep_rounds`（0–100）、`proxy_port`（0–65535）均在后端收窄，绕过前端的越界值不会直接落库。
5. **event_bus Webhook 并发节流**：有界分派（并发上限 8、队列上限 256），突发 `emit` 不再 scale 出无界线程。

## 验收清单

### v2

1. 窗口标题与 UI 显示「Orua」；标题栏拖动、双击最大化/还原、右键系统菜单可用
2. 创建 ≥3 个 Bot（含不同 API 覆盖），新建 1v1 会话可任选 Bot 流式对话
3. 创建含 2-10 个 Bot 的群，点开聊后 Bot 按顺序自动连聊、消息流式展示；暂停 / 继续 / 停止 / 插话 / 换话题全部可用
4. 某 Bot API 不可用时该轮显示失败气泡、群聊不中断；连续 3 次失败自动暂停，失败原因与暂停原因落库，重开房间仍可见
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

### v4

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
10. v1 / v2 / v3 功能与自动化测试全绿；旧库升级零破坏

> 第 8 项备注：`reminder` 技能入库、`/api/v1/reminders?due=1` 到期查询与完成标记已由自动化覆盖（ctest + test_api.ps1）。前端已实现到期轮询与提示（主窗口 15s 轮询 → 每周期至多一条 toast → 该条落地才标完成，未见不标完成，未提示的排队到下一轮），独立聊天窗口按设计不参与提示（见「已知限制」）。实机验收仍建议以「HTTP 拒绝内网」「文件 8KB 截断」两项现场观察为准。

### v4.1

1. 单聊生成失败后错误提示留在对话区（不被整表重载擦掉、切走再切回仍跟随该会话、下一次成功不留旧疤）
2. 群聊失败原因落库，重开房间能看到为什么停了；留痕渲染成群内提示，不伪装成用户 / Bot 消息
3. Bot 编辑器拦下越界 `max_tokens`（1–65536 整数，留空仍 2048）与「模型名 + 设置默认模型双空」，提示可操作
4. 到期提醒每 tick 至多一条 toast 真正可见；正文与待发计数合并进同一条；未见不标完成，其余排队到下一轮
5. 群「进行中」徽标随运行状态实时出现 / 消失；后端重启后陈旧徽标被整表刷新清除；持续在线不重复刷新（无抖动）；detached 窗口不代主窗口刷新
6. 版本三处统一 `4.1.0`（exe 文件/产品版本、`/health`、侧栏脚注），脚注 boot 时读 `/health` 覆盖
7. README 含「打包与更新指引」，明确 `publish.cmd` 会连 `data` 一起清掉、需备份与回拷
8. 三个 stub 闸门全 PASS（修复前副本应 FAIL）：提醒 109 条、技能 54 条、失败可见性 19 条
9. `ctest` 全绿（v4.1 当时基线 82 用例 / 695 断言；v5 追加 `test_v5.cpp` 后为 91 / 777）；`test_api.ps1` PASSED；WPF 构建 0 警告 0 错误；v1–v4 功能与自动化测试全绿；旧库升级零破坏

### v5

1. `file_read` 仅允许 data 目录 / 系统临时目录 / `file_read_allowlist` 白名单，越界路径拒绝（ctest 覆盖）
2. 后端每启动生成随机令牌；前端经 WPF 注入后对 `/api/v1/*` 请求回传 `X-Orua-Token`；带跨域 Origin 且令牌已启用时校验，无 Origin 直连不受影响（test_api.ps1 全绿）
3. `http_get` 主机名解析后逐 IP 复检（DNS-Rebinding），不跟随重定向，SSRF 约束不回归（ctest 覆盖）
4. `max_tokens` / `temperature` / `keep_rounds` / `proxy_port` 服务端收窄（ctest 覆盖）
5. event_bus Webhook 有界分派（并发 ≤8、队列 ≤256），单次 emit 仍 ≤400ms 送达（ctest 覆盖）
6. v1–v4.1 功能与自动化测试全绿；版本号三处 5.0.0

### v5.1

1. 单聊「发送」在真实 WebView2 中可用：请求打到 `http://127.0.0.1:<port>/api/v1/chat/stream`，不再出现 `Failed to fetch`（`smoke_gui.mjs` 真实点击断言覆盖）
2. 「重新生成」可用（与单聊同走 `consumeSse`，同一修复）
3. 全部 5 处 `fetch` 均以 `API_BASE` 开头，无裸相对路径请求
4. `scripts\build_backend.cmd` 在 VS 安装路径含中文时能正确找到 cmake/ctest，`publish.cmd` 一键跑通
5. `dist\Orua\AiCompanion.exe` 文件版本为 `5.1.0.0`、产品名 `Orua`；`/health` 返回 `5.1.0`；侧栏脚注显示 `v5.1`
6. `smoke_gui.mjs` 支持 `CDP_PORT` 环境变量
7. v1–v5.0 功能与自动化测试全绿（ctest 91 用例 / 777 断言、三个 stub 闸门 109/54/19、`test_api.ps1`）
