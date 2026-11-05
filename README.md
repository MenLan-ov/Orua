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
| Visual Studio 2022/2026 Build Tools | 勾选"使用 C++ 的桌面开发"（MSVC + CMake） |
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
