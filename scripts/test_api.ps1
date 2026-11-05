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

# 期望报错的请求：返回 @{ Status = <HTTP码>; Body = <信封> }；请求意外成功则 FAIL
function Invoke-ExpectError($Method, $Uri, $Obj) {
  try {
    if ($null -eq $Obj) { Invoke-RestMethod $Uri -Method $Method | Out-Null }
    else { Invoke-Json $Method $Uri $Obj | Out-Null }
  } catch {
    $resp = $_.Exception.Response
    if ($null -ne $resp) {
      $code = [int]$resp.StatusCode.value__
      $body = $null
      if ($_.ErrorDetails.Message) { $body = $_.ErrorDetails.Message | ConvertFrom-Json }
      return @{ Status = $code; Body = $body }
    }
    throw
  }
  throw "FAIL: $Uri 预期报错却成功"
}

# health
$h = Invoke-RestMethod "$base/health"
Assert-True ($h.status -eq 'ok') 'health'

# personas 已移除
$gone = Invoke-ExpectError 'Get' "$base/api/v1/personas" $null
Assert-True ($gone.Status -eq 404) 'personas removed 404'

# 默认机器人（迁移播种）
$bl = Invoke-RestMethod "$base/api/v1/bots"
Assert-True ($bl.code -eq 0) 'bots list code'
Assert-True ($bl.data.bots.Count -ge 1) 'default bot seeded'
Assert-True ($bl.data.default_bot_id -gt 0) 'default_bot_id present'
$defaultBotId = [int64]$bl.data.default_bot_id

# 默认机器人不可删（409 + code 1001）
$del = Invoke-ExpectError 'Delete' "$base/api/v1/bots/$defaultBotId" $null
Assert-True ($del.Status -eq 409) 'default bot delete 409'
Assert-True ($del.Body.code -eq 1001) 'default bot delete code 1001'

# 创建机器人（api_key 明文入、掩码出）
$b = Invoke-Json 'Post' "$base/api/v1/bots" @{
  name = '测试猫娘'; avatar = '🐱'; greeting = '喵~'; system_prompt = '你是猫娘'
  temperature = 0.9; api_base_url = ''; api_key = 'sk-test-1234567890abcd'; api_model = ''
}
Assert-True ($b.code -eq 0) 'bot create'
$botId = [int64]$b.data.id
Assert-True ($b.data.api_key -eq 'sk-tes...abcd') 'bot key masked'

# PATCH：改名 + api_key 空串 = 清除
$b2 = Invoke-Json 'Patch' "$base/api/v1/bots/$botId" @{ name = '猫娘二号'; api_key = '' }
Assert-True ($b2.data.name -eq '猫娘二号') 'bot patch name'
Assert-True ($b2.data.api_key -eq '') 'bot key cleared'

# 停用后不可开新对话、不可入群
Invoke-Json 'Post' "$base/api/v1/bots/$botId/enable" @{ enabled = $false } | Out-Null
$bad = Invoke-ExpectError 'Post' "$base/api/v1/sessions" @{ title = 'x'; bot_id = $botId }
Assert-True ($bad.Status -eq 400) 'session with disabled bot 400'
$g1004 = Invoke-ExpectError 'Post' "$base/api/v1/groups" @{ name = '坏群'; bot_ids = @($botId, $defaultBotId) }
Assert-True ($g1004.Body.code -eq 1004) 'disabled bot in group 1004'
$g1003 = Invoke-ExpectError 'Post' "$base/api/v1/groups" @{ name = '坏群2'; bot_ids = @($defaultBotId) }
Assert-True ($g1003.Body.code -eq 1003) 'member count 1003'

# 重新启用并建群
Invoke-Json 'Post' "$base/api/v1/bots/$botId/enable" @{ enabled = $true } | Out-Null
$g = Invoke-Json 'Post' "$base/api/v1/groups" @{ name = '测试群'; topic = '聊聊天气'; bot_ids = @($botId, $defaultBotId) }
Assert-True ($g.code -eq 0) 'group create'
Assert-True ($g.data.member_count -eq 2) 'group member_count'
$gid = [int64]$g.data.id

# 开聊
$st = Invoke-Json 'Post' "$base/api/v1/groups/$gid/start" @{ topic = '新话题' }
Assert-True ($st.data.state -eq 'running') 'group start running'

# 运行中改成员 → 409 + code 1002
$c = Invoke-ExpectError 'Patch' "$base/api/v1/groups/$gid" @{ bot_ids = @($botId, $defaultBotId) }
Assert-True ($c.Status -eq 409) 'patch members while running 409'
Assert-True ($c.Body.code -eq 1002) 'patch members 1002'

# 群流 SSE：后台 curl 抓 12s，应含 status + ≥2 轮 turn_start/delta/turn_end
$tmp = Join-Path $env:TEMP 'ac_group_sse.txt'
$proc = Start-Process -FilePath 'curl.exe' -ArgumentList @('-s','-N',"$base/api/v1/groups/$gid/stream") -RedirectStandardOutput $tmp -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 12
Stop-Process -Id $proc.Id -Force
Start-Sleep -Milliseconds 300
$sse = Get-Content $tmp -Raw -Encoding UTF8
Remove-Item $tmp
Assert-True ($sse -match 'event: status') 'group sse status'
$starts = ([regex]::Matches($sse, 'event: turn_start')).Count
Assert-True ($starts -ge 2) "turn_start count=$starts"
Assert-True ($sse -match 'event: delta') 'group sse delta'
Assert-True ($sse -match 'event: turn_end') 'group sse turn_end'

# 消息落库（≥2 条 assistant）
$gm = Invoke-RestMethod "$base/api/v1/groups/$gid/messages?limit=200"
$asst = @($gm.data | Where-Object { $_.role -eq 'assistant' }).Count
Assert-True ($asst -ge 2) "group assistant messages=$asst"

# 插话落库（bot_id = 0）
Invoke-Json 'Post' "$base/api/v1/groups/$gid/interject" @{ content = '插话测试' } | Out-Null
Start-Sleep -Seconds 2
$gm2 = Invoke-RestMethod "$base/api/v1/groups/$gid/messages?limit=200"
Assert-True (@($gm2.data | Where-Object { $_.role -eq 'user' -and $_.content -eq '插话测试' }).Count -eq 1) 'interject stored'

# 停止 → idle；详情带成员
$sp = Invoke-Json 'Post' "$base/api/v1/groups/$gid/stop" @{}
Assert-True ($sp.data.state -eq 'idle') 'group stop idle'
$gd = Invoke-RestMethod "$base/api/v1/groups/$gid"
Assert-True ($gd.data.status -eq 'idle') 'group status idle after stop'
Assert-True ($gd.data.members.Count -eq 2) 'group detail members'

# 删除群（级联）
Invoke-RestMethod "$base/api/v1/groups/$gid" -Method Delete | Out-Null
$gl = Invoke-RestMethod "$base/api/v1/groups"
Assert-True (($gl.data | Where-Object id -eq $gid) -eq $null) 'group delete'

# 会话绑定机器人 + 切换
$s = Invoke-Json 'Post' "$base/api/v1/sessions" @{ title = 'E2E测试会话'; bot_id = $botId }
Assert-True ($s.code -eq 0) 'session create'
Assert-True ($s.data.bot_id -eq $botId) 'session bot_id echo'
$sid = [int64]$s.data.id
Invoke-Json 'Patch' "$base/api/v1/sessions/$sid" @{ title = '改名后'; bot_id = $defaultBotId } | Out-Null
$list = Invoke-RestMethod "$base/api/v1/sessions"
Assert-True (($list.data | Where-Object id -eq $sid).title -eq '改名后') 'session rename'
Assert-True (($list.data | Where-Object id -eq $sid).bot_id -eq $defaultBotId) 'session bot switch'

# 配置读写（key 留空不变更）
Invoke-Json 'Post' "$base/api/v1/config" @{ base_url='https://api.example.com/v1'; model='demo-model'; keep_rounds=5; theme='sakura'; proxy_enabled=$false; proxy_host=''; proxy_port=0 } | Out-Null
$cfg = Invoke-RestMethod "$base/api/v1/config"
Assert-True ($cfg.data.model -eq 'demo-model') 'config model roundtrip'
Assert-True ($cfg.data.theme -eq 'sakura') 'config theme roundtrip'
Assert-True ($cfg.data.keep_rounds -eq '5') 'config keep_rounds roundtrip'

# 聊天流（LLM_MOCK 假流；载荷经临时文件传递，避免 PS 5.1 原生参数转义问题）
$pfile = Join-Path $env:TEMP 'ac_payload.json'
[IO.File]::WriteAllText($pfile, '{"session_id":' + $sid + ',"content":"hello mock"}')
$tmp2 = Join-Path $env:TEMP 'ac_sse.txt'
& curl.exe -s -N -X POST "$base/api/v1/chat/stream" -H "Content-Type: application/json" --data-binary "@$pfile" -o $tmp2
$sse2 = Get-Content $tmp2 -Raw -Encoding UTF8
Remove-Item $tmp2, $pfile
Assert-True ($sse2 -match 'event: delta') 'sse delta event'
Assert-True ($sse2 -match 'event: done') 'sse done event'

# 消息已落库（user + assistant 各一条）
$m = Invoke-RestMethod "$base/api/v1/messages?session_id=$sid&limit=10"
Assert-True ($m.data.Count -eq 2) "messages count=$($m.data.Count)"
Assert-True (($m.data | Where-Object role -eq 'assistant').content.Length -gt 0) 'assistant message stored'

# 重新生成（删除最后一条 assistant 并重跑，仍应有 2 条）
$rfile = Join-Path $env:TEMP 'ac_reg.json'
[IO.File]::WriteAllText($rfile, '{"session_id":' + $sid + '}')
$tmp3 = Join-Path $env:TEMP 'ac_sse2.txt'
& curl.exe -s -N -X POST "$base/api/v1/chat/regenerate" -H "Content-Type: application/json" --data-binary "@$rfile" -o $tmp3
$sse3 = Get-Content $tmp3 -Raw -Encoding UTF8
Remove-Item $tmp3, $rfile
Assert-True ($sse3 -match 'event: done') 'regenerate done'
$m2 = Invoke-RestMethod "$base/api/v1/messages?session_id=$sid&limit=10"
Assert-True ($m2.data.Count -eq 2) 'regenerate keeps 2 messages'

# 导出 md 与 json
$tmp4 = Join-Path $env:TEMP 'ac_export.md'
& curl.exe -s -X POST "$base/api/v1/export?session_id=$sid&format=md" -o $tmp4
$md = Get-Content $tmp4 -Raw -Encoding UTF8
Remove-Item $tmp4
Assert-True ($md -match '用户') 'export md contains user label'

$tmp5 = Join-Path $env:TEMP 'ac_export.json'
& curl.exe -s -X POST "$base/api/v1/export?session_id=$sid&format=json" -o $tmp5
$j = Get-Content $tmp5 -Raw -Encoding UTF8 | ConvertFrom-Json
Remove-Item $tmp5
Assert-True ($j.messages.Count -eq 2) 'export json message count'

# 清空会话消息
Invoke-RestMethod "$base/api/v1/messages?session_id=$sid" -Method Delete | Out-Null
$m3 = Invoke-RestMethod "$base/api/v1/messages?session_id=$sid"
Assert-True ($m3.data.Count -eq 0) 'messages cleared'

# 清理：删除测试会话与测试机器人
Invoke-RestMethod "$base/api/v1/sessions/$sid" -Method Delete | Out-Null
$list2 = Invoke-RestMethod "$base/api/v1/sessions"
Assert-True (($list2.data | Where-Object id -eq $sid) -eq $null) 'session delete'
Invoke-RestMethod "$base/api/v1/bots/$botId" -Method Delete | Out-Null
$bl3 = Invoke-RestMethod "$base/api/v1/bots"
Assert-True (($bl3.data.bots | Where-Object id -eq $botId) -eq $null) 'bot delete'
Assert-True ($bl3.data.bots.Count -ge 1) 'default bot remains'

Write-Host 'CRUD API TESTS PASSED'
