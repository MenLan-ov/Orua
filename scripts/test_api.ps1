$ErrorActionPreference = 'Stop'
$base = 'http://127.0.0.1:8765'

function Assert-True($cond, $msg) {
  if (-not $cond) { throw "FAIL: $msg" }
}

# PS 5.1 将字符串 Body 按 ANSI 编码发送，中文会乱码；统一转 UTF-8 字节发送
function Invoke-Json($Method, $Uri, $Obj) {
  $json = $Obj | ConvertTo-Json -Compress
  $bytes = [Text.Encoding]::UTF8.GetBytes($json)
  Invoke-RestMethod $Uri -Method $Method -Body $bytes -ContentType 'application/json; charset=utf-8'
}

# health
$h = Invoke-RestMethod "$base/health"
Assert-True ($h.status -eq 'ok') 'health'

# 人设列表（种子默认人设）
$p = Invoke-RestMethod "$base/api/v1/personas"
Assert-True ($p.code -eq 0) 'personas list code'
Assert-True ($p.data.Count -ge 1) 'personas seeded'

# 会话创建
$s = Invoke-Json 'Post' "$base/api/v1/sessions" @{ title = 'E2E测试会话' }
Assert-True ($s.code -eq 0) 'session create'
$sid = $s.data.id

# 重命名
Invoke-Json 'Patch' "$base/api/v1/sessions/$sid" @{ title = '改名后' } | Out-Null
$list = Invoke-RestMethod "$base/api/v1/sessions"
Assert-True (($list.data | Where-Object id -eq $sid).title -eq '改名后') 'session rename'

# 配置读写（key 留空不变更）
Invoke-Json 'Post' "$base/api/v1/config" @{ base_url='https://api.example.com/v1'; model='demo-model'; keep_rounds=5; theme='sakura'; proxy_enabled=$false; proxy_host=''; proxy_port=0 } | Out-Null
$c = Invoke-RestMethod "$base/api/v1/config"
Assert-True ($c.data.model -eq 'demo-model') 'config model roundtrip'
Assert-True ($c.data.theme -eq 'sakura') 'config theme roundtrip'
Assert-True ($c.data.keep_rounds -eq '5') 'config keep_rounds roundtrip'

# 聊天流（LLM_MOCK 假流；载荷经临时文件传递，避免 PS 5.1 原生参数转义问题）
$pfile = Join-Path $env:TEMP 'ac_payload.json'
[IO.File]::WriteAllText($pfile, '{"session_id":' + $sid + ',"content":"hello mock"}')
$tmp = Join-Path $env:TEMP 'ac_sse.txt'
& curl.exe -s -N -X POST "$base/api/v1/chat/stream" -H "Content-Type: application/json" --data-binary "@$pfile" -o $tmp
$sse = Get-Content $tmp -Raw -Encoding UTF8
Remove-Item $tmp, $pfile
Assert-True ($sse -match 'event: delta') 'sse delta event'
Assert-True ($sse -match 'event: done') 'sse done event'

# 消息已落库（user + assistant 各一条）
$m = Invoke-RestMethod "$base/api/v1/messages?session_id=$sid&limit=10"
Assert-True ($m.data.Count -eq 2) "messages count=$($m.data.Count)"
Assert-True (($m.data | Where-Object role -eq 'assistant').content.Length -gt 0) 'assistant message stored'

# 重新生成（删除最后一条 assistant 并重跑，仍应有 2 条）
$rfile = Join-Path $env:TEMP 'ac_reg.json'
[IO.File]::WriteAllText($rfile, '{"session_id":' + $sid + '}')
$tmp2 = Join-Path $env:TEMP 'ac_sse2.txt'
& curl.exe -s -N -X POST "$base/api/v1/chat/regenerate" -H "Content-Type: application/json" --data-binary "@$rfile" -o $tmp2
$sse2 = Get-Content $tmp2 -Raw -Encoding UTF8
Remove-Item $tmp2, $rfile
Assert-True ($sse2 -match 'event: done') 'regenerate done'
$m2 = Invoke-RestMethod "$base/api/v1/messages?session_id=$sid&limit=10"
Assert-True ($m2.data.Count -eq 2) 'regenerate keeps 2 messages'

# 导出 md 与 json
$tmp3 = Join-Path $env:TEMP 'ac_export.md'
& curl.exe -s -X POST "$base/api/v1/export?session_id=$sid&format=md" -o $tmp3
$md = Get-Content $tmp3 -Raw -Encoding UTF8
Remove-Item $tmp3
Assert-True ($md -match '用户') 'export md contains user label'

$tmp4 = Join-Path $env:TEMP 'ac_export.json'
& curl.exe -s -X POST "$base/api/v1/export?session_id=$sid&format=json" -o $tmp4
$j = Get-Content $tmp4 -Raw -Encoding UTF8 | ConvertFrom-Json
Remove-Item $tmp4
Assert-True ($j.messages.Count -eq 2) 'export json message count'

# 清空会话消息
Invoke-RestMethod "$base/api/v1/messages?session_id=$sid" -Method Delete | Out-Null
$m3 = Invoke-RestMethod "$base/api/v1/messages?session_id=$sid"
Assert-True ($m3.data.Count -eq 0) 'messages cleared'

# 清理：删除测试会话
Invoke-RestMethod "$base/api/v1/sessions/$sid" -Method Delete | Out-Null
$list = Invoke-RestMethod "$base/api/v1/sessions"
Assert-True (($list.data | Where-Object id -eq $sid) -eq $null) 'session delete'

Write-Host 'CRUD API TESTS PASSED'
