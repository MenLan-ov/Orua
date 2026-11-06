# Orua v3 设计规格：Bot 长期记忆、Bot 化术语与编辑器升级

> 参考 UI：图一「新建 API Bot」弹窗（字段布局）、图二「Bot 记忆库」页面（布局与文案精神）。
> 本规格在 v2（2026-11-05-orua-v2-design.md）之上增量演进，v2 全部功能与测试保持有效。

## 1. 目标

1. **术语统一**：所有用户可见文案由「机器人」改为「Bot」（含默认 Bot）。
2. **Bot 编辑器升级**：新建/编辑 Bot 弹窗对齐图一字段（名字、头像上传、API 类型、API 地址、Key、模型、温度、最大 tokens），创建后可随时增改提示词/人设。
3. **头像上传**：Bot 支持图片头像（保留 emoji 回退），聊天气泡、列表、详情、群聊全部换用。
4. **Bot 长期记忆**：每个 Bot 有独立记忆，记住私聊与群聊内容；私聊/群聊各自带着对应 scope 的记忆驱动行为；提供记忆库 UI（浏览/搜索/编辑/删除）。

## 2. 总体架构（变更点）

- **数据层**：`bots` 表新增 `max_tokens`、`api_type`、`avatar_path` 列；新增 `bot_memories` 表；`settings` 新增 `memory_extraction`；头像文件存 `data/uploads/`。
- **后端**：新增记忆提取管线（异步 LLM）与两段式检索（本地召回 + LLM 精选）；ChatService 与 GroupEngine 的上下文组装改为增强式注入；新增记忆 CRUD/搜索与头像上传路由。
- **前端**：侧栏增「记忆」标签；Bot 编辑器扩展字段与头像上传；新增记忆库页（图二布局）；全量文案 Bot 化。
- **WPF 壳**：无改动（桥协议不变）。

## 3. 数据模型

### 3.1 bots 表新增列（幂等 ALTER，沿用 v2 迁移模式）

| 列 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `max_tokens` | INTEGER NOT NULL | 2048 | 请求最大 tokens，进请求体 |
| `api_type` | TEXT NOT NULL | 'openai' | 仅 'openai'（OpenAI 兼容）；UI 下拉预留扩展 |
| `avatar_path` | TEXT | NULL | 头像图片路径（相对 data 目录，如 `uploads/3.png`）；NULL = 用 emoji |

### 3.2 bot_memories 表（新建）

```sql
CREATE TABLE IF NOT EXISTS bot_memories (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  bot_id INTEGER NOT NULL,
  scope TEXT NOT NULL CHECK(scope IN ('private','group')),
  group_id INTEGER,                      -- scope='group' 时必填；private 为 NULL
  content TEXT NOT NULL,
  tags TEXT NOT NULL DEFAULT '[]',       -- JSON 字符串数组
  source_msg_id INTEGER,                 -- private: messages.id；group: group_messages.id；可空
  created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
  updated_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
);
CREATE INDEX IF NOT EXISTS idx_mem_bot ON bot_memories(bot_id, scope);
CREATE INDEX IF NOT EXISTS idx_mem_group ON bot_memories(bot_id, group_id);
```

- 维度：私聊记忆 `(bot_id, scope='private')` 跨会话共享；群聊记忆 `(bot_id, group_id)`（一个 Bot 在多个群各自记）。
- **不设上限、不自动淘汰**；去重靠提取时的「同主题合并」（见 4.1）。
- 级联清理：删除 Bot → 清其全部记忆；删除群 → 清该群全部 group 记忆。

### 3.3 settings 扩展

- `settings` 表新增键 `memory_extraction`：`'1'` 开启（默认）/ `'0'` 关闭。**开关只控制提取**，已有记忆仍参与注入。

### 3.4 头像文件存储

- 路径：`data/uploads/{bot_id}.png`（db 存 `uploads/{bot_id}.png`）。
- 前端 canvas 缩放到 128×128 后以 base64 上传；后端原样落盘（不再服务端缩放）。大小双层限制：前端限制原图 ≤256KB，后端校验解码后 PNG ≤256KB，超限返回 400。
- `data/uploads/` 加入 .gitignore（与 data/ 同级管理）。

### 3.5 迁移

- `db::open` 内幂等：`ALTER TABLE bots ADD COLUMN ...`（存在即跳过，沿用 v2 模式）；`bot_memories` 用 `CREATE TABLE IF NOT EXISTS`。老库升级零破坏，v2 数据不动。

## 4. 记忆系统（后端）

### 4.1 提取管线（异步 LLM）

- 触发点：
  - 私聊：助手回复流式完成、消息落库后（`/chat/stream` 完成路径），为该会话的 bot 异步提取一次；
  - 群聊：`GroupEngine` 每个 `turn_end` 落库后，为该轮发言 bot 异步提取一次。
- 执行：每 Bot 一个串行异步队列（提取任务不并发、不阻塞主请求/引擎线程）；失败静默、重试一次（退避 2s），仍失败放弃本轮。
- LLM 输入：该 bot 的现有记忆摘要（`id + content` 一行，上限 200 条）+ 最近一轮对话（私聊：最近一轮用户消息与助手回复；群聊：该轮全部消息）。
- LLM 输出（JSON，强制格式）：
  ```json
  {"ops":[{"action":"new","content":"...","tags":["..."]},
          {"action":"update","memory_id":12,"content":"...","tags":["..."]}]}
  ```
  - `new`：插入新记忆；`update`：同主题合并（更新 content/tags/updated_at），实现「同主题去重」。
  - 无值得记的事实 → `{"ops":[]}`，不产生空记忆。
- 配置：复用该 bot 的 API 覆盖配置（空则继承全局）；`memory_extraction='0'` 时整体跳过。
- `LLM_MOCK=1` 时返回固定两条记忆（一条 new、一条 update 占位），保证测试确定性。

### 4.2 两段式检索（注入前选记忆）

- 阶段 1（本地召回，零 API 成本）：用最近一轮对话文本与 `tags`/`content` 做关键词 LIKE 匹配（按空格/标点切词，逐词 OR），按 `updated_at` 排序取 ≤30 条候选；候选为空则跳过阶段 2。
- 阶段 2（LLM 精选）：把候选（`id + content` 一行）与最近对话交给 LLM，输出相关记忆 id 列表（≤5 条），按序注入。
- 失败降级：阶段 2 失败 → 直接注入阶段 1 前 5 条；全部失败 → 无记忆块（退化为 v2 纯窗口行为）。

### 4.3 上下文注入（增强式）

- 私聊 `ChatService::buildRequest`：
  `system_prompt + [长期记忆]块(≤5 条，每条一行 "· content") + 最近 context_rounds 轮原始消息（沿用现有配置，不变）+ 当前用户消息`
- 群聊 `GroupEngine::buildRequest`：
  `system_prompt + 群话题 + [长期记忆]块(该 bot 在该群的 ≤5 条) + 最近 12 条群消息（v2 为 20 条，随记忆块引入而缩小）`
- scope 隔离：私聊只注入 `scope='private'`；群聊只注入 `(bot, 该群)` 的 group 记忆。禁止串用。
- 检索失败或记忆为空时完全等价于 v2 行为。

### 4.4 降级与开关

- 提取、检索任一步 LLM 不可用/失败：静默降级，聊天/群聊功能永不受阻。
- `memory_extraction='0'`：停止新提取，已有记忆照常注入；记忆库 UI 仍可浏览/编辑/删除。

## 5. 后端 API（信封与错误码沿用 v2；无新增错误码）

### 5.1 记忆

- `GET /api/v1/bots/{id}/memories?scope=private|group&group_id=&q=`
  → `data: {memories:[{id,bot_id,scope,group_id,content,tags,source_msg_id,created_at,updated_at}]}`
  （`q` 为关键词，本地 LIKE 匹配 content/tags；`scope`/`group_id` 过滤）
- `PUT /api/v1/bots/{id}/memories/{memId}` body `{content}` → 更新 content/updated_at（tags 保留）
- `DELETE /api/v1/bots/{id}/memories/{memId}` → 删除
- 不存在（bot 或记忆）→ 404；scope 非法 → 400

### 5.2 头像

- `POST /api/v1/bots/{id}/avatar` body `{image_base64}`（PNG，解码后 ≤256KB）
  → `data:{avatar_url:"uploads/3.png"}`；落盘 `data/uploads/{id}.png`
- 解码失败/超限 → 400；bot 不存在 → 404

### 5.3 配置

- `GET/POST /api/v1/config` 扩展键 `memory_extraction`（'1'/'0'），与现有键同信封；未知键忽略。

### 5.4 botToJson 扩展

- 新增返回字段：`max_tokens`、`api_type`、`avatar_url`（NULL 或路径）；`avatar`（emoji）保留。

## 6. 前端 UI

### 6.1 术语统一

- 全部用户可见文案「机器人」→「Bot」：侧栏标签、新建按钮、弹窗标题（新建 Bot/编辑 Bot）、默认 Bot、群成员面板、toast、README、验收清单。
- 「✦ Orua」品牌、主题、布局不变；后端字段名不变。

### 6.2 Bot 编辑器（图一参考）

- 现有编辑弹窗扩展为图一字段序：名字（必填）、头像（emoji 快捷 + 「选择文件」上传）、API 类型下拉（仅「OpenAI 兼容」）、API 地址、API Key、模型、温度、最大 tokens（默认 2048）、开场白、系统提示词/人设。
- 创建后可在 Bot 详情页随时再次编辑（复用弹窗）；详情页展示提示词/人设摘要。

### 6.3 头像

- 上传：选择本地图片 → canvas 缩放 128×128 PNG → base64 → `POST /avatar`；成功后刷新列表/详情。
- 显示：凡头像处（列表、详情、聊天气泡、群聊气泡、成员面板）`avatar_url` 优先（`<img>`，圆角裁切），无则回退 emoji。

### 6.4 记忆库页（图二参考）

- 侧栏 4 个标签：消息 / Bot / 群聊 / 记忆；记忆页标题「Bot 记忆库」。
- 布局：左侧「我的 Bot」列表（含默认 Bot）；右侧选中 Bot 的记忆区：搜索框（占位「搜一搜…」风格）+ 刷新 + 记忆列表。
- 记忆条目：内容、标签、scope 徽标（私聊 / 群聊·群名）、更新时间、来源（private 且 source 存在时点回原会话）、编辑（改 content 即时生效）、删除（立即停止注入）。
- 空态文案体现「记忆在本地，改这里就改变 Bot 的行为」。

## 7. 错误处理

- 无新增错误码；沿用 v2：1001 默认 Bot 不可删、1002 状态冲突、1003 成员数 2-10、1004 停用 Bot、不存在 → 404。
- 头像超限/解码失败、scope 非法 → 400 + message。
- 记忆链路（提取/检索）任何异常：日志记录 + 静默降级，不影响主请求。

## 8. 测试策略

- 单测（backend_tests 扩展）：
  - 记忆 CRUD、同主题 update 合并、级联清理（删 Bot/删群）；
  - 标签召回（阶段 1）候选排序与上限；注入组装（私聊不含群记忆、群不含私聊记忆、K≤5）；
  - 迁移幂等（max_tokens/api_type/avatar_path 默认值，老库升级后 v2 数据完好）；
  - 群聊引擎上下文：12 条窗口 + 记忆块（同步更新 v2 引擎 ctx 测试期望）。
- 集成（test_api.ps1 扩展，LLM_MOCK）：
  - 私聊一轮 → 记忆落库（scope=private）；群聊 turn_end → group 记忆落库且 bot_id/group_id 正确；
  - 记忆搜索/编辑/删除 API；头像上传落盘并返回 avatar_url；`memory_extraction='0'` 后不再新增；
  - v2 全部断言保持通过。
- 前端：`node --check` + dotnet 构建 0 警告；手动验收清单（见 §10）。

## 9. 明确排除项

- 语义向量检索/本地嵌入模型（关键词 + LLM 精选已够用）；
- 记忆容量上限与自动淘汰（用户明确选择不设上限）；
- 手动添加记忆（记忆只由对话提取产生；UI 仅浏览/搜索/编辑/删除）；
- 多 Bot 共享记忆、群级共享画像；
- 云同步 / 平台记忆镜像（图二的平台概念不做；我们本就是本地存储）；
- Anthropic 等其他 API 类型（仅预留下拉位）；
- 记忆导出/导入。

## 10. 验收标准

1. 全 UI（含 README、验收清单）无「机器人」字样，统一显示「Bot」。
2. 新建 Bot 弹窗含图一全部字段（名字/头像/API 类型/地址/Key/模型/温度/最大 tokens）+ 开场白 + 提示词/人设；创建后可在详情页随时修改。
3. 头像上传后，列表/详情/聊天气泡/群聊气泡/成员面板显示图片头像；无图 Bot 回退 emoji。
4. 私聊数轮后，记忆库出现提取的记忆（真实 API 或 LLM_MOCK 下均可验证）。
5. 私聊新会话中 Bot 的回答体现记忆（增强式注入生效）。
6. 群聊运行后，发言 Bot 产生 group 记忆（bot_id/group_id 正确，各 Bot 各自记）。
7. 群聊上下文带群记忆驱动行为；私聊记忆不出现在群聊、群记忆不进入私聊。
8. 记忆库搜索/编辑/删除即时生效；删除后注入立即停止。
9. `memory_extraction` 关闭后不再新增记忆，已有记忆仍注入。
10. 记忆提取/检索失败时聊天与群聊功能正常（静默降级）。
11. v2 全部功能与自动化测试保持全绿；老库迁移零破坏。
