# 性能采样（发布简报用）：QuotaBar 进程 60s x N 记录 WorkingSet/PrivateMemory/CPU。
# 用法: powershell -File tools\perf_sample.ps1 [样本数, 默认 10]
param([int]$Count = 10)
$result = "D:\Zcode\QuotaBar\build\perf_sample.csv"
"name,pid,working_set_mb,private_mb,cpu_total_s,tick" | Out-File -FilePath $result -Encoding ascii
for ($i = 0; $i -lt $Count; $i++) {
    $p = Get-Process -Name QuotaBar -ErrorAction SilentlyContinue
    if (-not $p) { Write-Host "QuotaBar not running (tick $i)"; Start-Sleep -Seconds 60; continue }
    $ws = [math]::Round($p.WorkingSet64 / 1MB, 1)
    $pm = [math]::Round($p.PrivateMemorySize64 / 1MB, 1)
    $cpu = [math]::Round($p.TotalProcessorTime.TotalSeconds, 3)
    $tick = (Get-Date -Format "HH:mm:ss")
    "$($p.Name),$($p.Id),$ws,$pm,$cpu,$tick" | Add-Content -Path $result -Encoding ascii
    Write-Host "tick $i : ws=$ws MB private=$pm MB cpu=$cpu s"
    if ($i -lt $Count - 1) { Start-Sleep -Seconds 60 }
}
Write-Host "done -> $result"
