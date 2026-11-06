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

## 数据存储

`data/app.db`（SQLite，WAL）：

| 表 | 说明 |
|---|---|
| bots | Bot（含 API 覆盖项 api_base_url / api_key_enc / api_model，key 为 DPAPI 密文） |
| sessions | 会话（bot_id 绑定 Bot，0 = 默认） |
| messages | 单聊消息 |
| groups / group_members / group_messages | 群 / 成员（position 即发言顺序）/ 群消息（bot_id 为 NULL = 用户插话） |
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
| http_get | HTTP 请求 | 仅公网 http(s)，正文 ≤2KB 截断、10s 超时；拒绝 localhost / `.localhost` / 内网段（10. / 172.16-31. / 192.168. / 169.254. / 0.）/ 回环（127.*、`::1`、`::ffff:` 映射）与数字简写主机（十进制 `2130706433`、十六进制 `0x7f000001`、八进制 `0177.0.0.1`）防 SSRF；**不跟随重定向**（3xx 直接回 `status_code` + `location`，跳转目标不会被访问） |
| file_read | 读取文件 | 读取指定本地文本文件，≤8KB 截断（按字节切，可能截断多字节字符，回填时按替换策略序列化），失败返回错误文本；**未做目录沙箱**（见下方已接受风险） |
| reminder | 定时提醒 | `due_at` 支持 ISO 8601（含 `Z` / 显式时区偏移，也接受无时区的本地墙钟）与相对分钟（`15分钟后`、裸数字 `30`），统一归一化为 UTC 秒级 ISO 后写入 `reminders` 表；无法解析时返回错误文本且不入库；到期提示由主窗口轮询驱动（见「功能」与验收清单） |

**已接受风险（裁决 #21）**：`file_read` 可读取 Bot 被要求读取的**任意本地路径**（仅 ≤8KB 截断，未做工作目录白名单/目录沙箱）。本轮终验决定不加沙箱，是否收敛为「仅工作目录」由用户后续决定；主窗口对 `file_read` 结果不做二次确认。同理 `http_get` 只保证拒绝内网/回环与数字简写主机、不跟随重定向，不做出网白名单。

另：`backend/src/event_bus.h` 定义事件总线，内置 Webhook 插件可将事件 POST 到外部地址。事件：`chat.completed` / `chat.error` / `session.created` / `session.deleted`。

## 打包

```cmd
dotnet publish src\AiCompanion.App -c Release -r win-x64
```

将 `bin\backend\backend.exe` 放在发布目录的 `backend\` 子目录下（`run_dev.cmd` 的部署步骤即此布局）。

## 已知限制

- **独立聊天窗口不弹提醒 toast**：到期提醒轮询只在主窗口启动，detached 窗口不发轮询、不提示、也不标完成；提醒仍会正常入库，并在主窗口下一次轮询时提示。这是有意限制——避免多个窗口重复提示、重复 `POST /reminders/{id}/done` 竞争。
- **提醒只在应用运行期间提示**：无系统通知、无开机自启调度；应用关闭期间到期的提醒会在下次启动后按每 15s 一条逐轮提示（每轮至多一条，其余排队）。
- **群聊不启用技能**：Function calling 工具循环（≤3 轮）只在单聊生效，群聊发言不调用技能。
- **`file_read` 无目录沙箱**：见「插件」小节的已接受风险说明（裁决 #21，本轮不收敛）。

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

### v4

1. 侧栏页签为「聊天 / Bot / 记忆 / 插件」；聊天页单聊与群聊同一列表，QQ 好友式布局（置顶区 + 按最近活跃排序）
2. 每个 Bot 一个聊天项，点击进入其最近会话（无会话时自动创建）；同一 Bot 的多会话入口在右键菜单
   - 终验修复波后已覆盖：历史会话子菜单每个会话两行——标题行点击即打开（当前会话带「当前」标记），动作行为 默认 / 重命名 / 清空 / 导出 / 删除，`清空`/`导出` 已支持作用于非当前会话
3. 右键菜单按项类型（Bot / 群）显示正确项集；删除有二次确认；默认 Bot 删除返回 1001 提示
4. 置顶与未读红点生效，重启后保留（`chat_pinned` / `chat_reads`）；群聊项在群 `status = running` 时显示「进行中」徽标
   - 第二轮修复后已覆盖：群状态变更（SSE `status` 事件、开始/暂停/继续/停止）统一经 `syncGroupStatus()` 同时回写 `App.state.groups[].status` 并重渲染聊天列表，徽标随运行状态实时出现/消失（stub 断言 J 组）；本版本群只能从房间面板启动，故其实时范围为「本次会话打开过房间的群」，其它群按整表刷新（启动 / 保存群 / 删群）同步
5. 独立聊天窗口：打开 / 发送 / 同步 / 主窗口关闭联动可用；同一目标重复打开聚焦已有窗口
   - 终验修复波后已覆盖：窗口标题取 Bot 名 / 群名（壳侧剔除控制字符并截断 40 字符，名称缺失回退「聊天」/「群聊」，初始化失败标题优先）；重复打开时若旧窗口被最小化，先恢复再聚焦
6. 插件页展示 6 个技能卡片；Bot 技能开关与 Bot 编辑器勾选同步生效
7. 单聊中 Bot 调用技能并给出正确结果（真实 API 或 `LLM_MOCK=1` 均可验证，计算器返回「计算结果：2」）
   - 终验修复波后已覆盖：技能抛异常或结果含非法 UTF-8 时按替换策略序列化并回填 `{"error": "技能执行失败: …"}`，每个 tool_call 都有对应 tool 消息，对话继续生成且助手消息照常落库；流式回调退出（含异常）即清理该会话 stop 标记
8. 提醒到期前端弹出提示；HTTP 技能拒绝内网地址；文件技能超过 8KB 截断
   - 第二轮修复后已覆盖：`reminder` 写入带 Bot 归属（`bot_id` 取自会话），`due_at` 归一化为 UTC 秒级 ISO 8601；主窗口 15s 轮询到期提醒，每个周期只发一次 toast——呈现最早那条的正文，本轮仍有待发时把条数并成「（另有 N 条待提醒）」后缀，只有这条真落到 toast 元素上才记账并标完成，其余排队到下一轮（不再出现「正文被计数条覆盖」的静默丢失）
   - `http_get` 另拒绝数字简写/八进制/前导零与 IPv6 字面量（含 `[::ffff:…]`、`::1` 映射），并禁用自动重定向
9. v1 / v2 / v3 功能与自动化测试全绿；旧库升级零破坏

> 第 8 项备注：`reminder` 技能入库、`/api/v1/reminders?due=1` 到期查询与完成标记已由自动化覆盖（ctest + test_api.ps1）。前端已实现到期轮询与提示（主窗口 15s 轮询 → 每周期至多一条 toast → 该条落地才标完成，未见不标完成，未提示的排队到下一轮），独立聊天窗口按设计不参与提示（见「已知限制」）。实机验收仍建议以「HTTP 拒绝内网」「文件 8KB 截断」两项现场观察为准。
