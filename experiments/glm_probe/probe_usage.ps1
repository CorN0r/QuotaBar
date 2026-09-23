# glm_probe: 用与 QuotaBar 相同的方式真实调用 GLM monitor/quota/limit,导出响应 JSON 供契约分析。
# 密钥仅从 secrets.dat(DPAPI CurrentUser)读入内存,只发给 bigmodel.cn 本身;不打印密钥。
# 输出: experiments/glm_probe/glm_response.json(用户自己的额度数据,与 cache.json 同级敏感度)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.Security

$secretsPath = Join-Path $env:LOCALAPPDATA 'QuotaBar\secrets.dat'
$bytes = [IO.File]::ReadAllBytes($secretsPath)
$magic = [Text.Encoding]::ASCII.GetString($bytes[0..7])
if ($magic -ne 'QBARSEC1') { throw "secrets.dat magic 不识别: $magic" }

# 布局: 8 字节 magic + N 段 (4 字节 LE 长度 + DPAPI blob);顺序 kimi/deepseek/glm,取最后一段
$blobs = @()
$p = 8
while ($p + 4 -le $bytes.Length) {
    $len = [BitConverter]::ToInt32($bytes, $p)
    $p += 4
    if ($len -lt 0 -or $p + $len -gt $bytes.Length) { throw "blob 长度越界 (p=$p len=$len)" }
    $blobs += , $bytes[$p..($p + $len - 1)]
    $p += $len
}
if ($blobs.Count -lt 3) { throw "secrets.dat 只有 $($blobs.Count) 段,期望 3 段(新格式)" }

$plain = [System.Security.Cryptography.ProtectedData]::Unprotect(
    [byte[]]$blobs[2], $null, [System.Security.Cryptography.DataProtectionScope]::CurrentUser)
$glmKey = [Text.Encoding]::UTF8.GetString($plain)
if (-not $glmKey) { throw "GLM Key 为空" }

$url = 'https://open.bigmodel.cn/api/monitor/usage/quota/limit'
$headers = @{
    'Authorization'   = $glmKey   # 监控端点约定:裸 Key,无 Bearer 前缀
    'Accept-Language' = 'en-US,en'
    'Content-Type'    = 'application/json'
}

try {
    $resp = Invoke-WebRequest -Uri $url -Headers $headers -Method Get -UseBasicParsing
    $status = [int]$resp.StatusCode
    $body = $resp.Content
} catch {
    # 非 2xx:从异常中取响应体与状态码
    $stream = $_.Exception.Response.GetResponseStream()
    $reader = New-Object IO.StreamReader($stream)
    $body = $reader.ReadToEnd()
    $status = [int]$_.Exception.Response.StatusCode
}

$out = Join-Path $PSScriptRoot 'glm_response.json'
[IO.File]::WriteAllText($out, $body, (New-Object Text.UTF8Encoding($false)))
Write-Host "HTTP $status, $($body.Length) bytes -> $out"
