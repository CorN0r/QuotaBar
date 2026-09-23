# 临时:查询 DeepSeek 官方余额,核对圆环低额显示(用后可删)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.Security
$bytes = [IO.File]::ReadAllBytes((Join-Path $env:LOCALAPPDATA 'QuotaBar\secrets.dat'))
$p = 8
$blobs = @()
while ($p + 4 -le $bytes.Length) {
    $len = [BitConverter]::ToInt32($bytes, $p)
    $p += 4
    if ($len -lt 0 -or $p + $len -gt $bytes.Length) { break }
    $blobs += , $bytes[$p..($p + $len - 1)]
    $p += $len
}
$plain = [System.Security.Cryptography.ProtectedData]::Unprotect(
    [byte[]]$blobs[1], $null, [System.Security.Cryptography.DataProtectionScope]::CurrentUser)
$key = [Text.Encoding]::UTF8.GetString($plain)
$r = Invoke-RestMethod -Uri 'https://api.deepseek.com/user/balance' `
    -Headers @{ Authorization = ('Bearer ' + $key) }
$r.balance_infos | ForEach-Object { Write-Host ($_.currency + ' ' + $_.total_balance) }
