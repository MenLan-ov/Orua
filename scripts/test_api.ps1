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

# 清理：删除测试会话
Invoke-RestMethod "$base/api/v1/sessions/$sid" -Method Delete | Out-Null
$list = Invoke-RestMethod "$base/api/v1/sessions"
Assert-True (($list.data | Where-Object id -eq $sid) -eq $null) 'session delete'

Write-Host 'CRUD API TESTS PASSED'
