# capture.ps1 - run ulw_probe.exe and classify per-phase screen visibility.
# For every "PHASE <id>" marker in the exe stdout, wait until the phase window
# is up, screenshot the known region (physical px), count pixels of the phase's
# expected color, and report <id> -> hit count.
#
# Phase window: 300x150 at (600,300) physical. Capture region slightly larger:
# (560,260) size 380x240, step 3.
param([int]$MsPerPhase = 2500)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WCap {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
}
'@
[WCap]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null   # PMv2
Add-Type -AssemblyName System.Drawing

$exe = Join-Path $PSScriptRoot "ulw_probe.exe"
$out = Join-Path $PSScriptRoot "run_out.txt"
Remove-Item $out -ErrorAction SilentlyContinue

$expected = @{
    1 = @("yellow", { param($c) $c.R -gt 230 -and $c.G -gt 230 -and $c.B -lt 60 })
    2 = @("red",    { param($c) $c.R -gt 200 -and $c.G -lt 90 -and $c.B -lt 90 })
    3 = @("reddim", { param($c) $c.R -gt 110 -and $c.R -lt 235 -and $c.G -lt 80 -and $c.B -lt 80 })
    4 = @("red",    { param($c) $c.R -gt 200 -and $c.G -lt 90 -and $c.B -lt 90 })
    5 = @("green",  { param($c) $c.G -gt 200 -and $c.R -lt 90 -and $c.B -lt 90 })
    6 = @("blue",   { param($c) $c.B -gt 160 -and $c.R -lt 90 -and $c.G -lt 90 })
    7 = @("orange", { param($c) $c.R -gt 220 -and $c.G -gt 120 -and $c.G -lt 200 -and $c.B -lt 100 })
    8 = @("cyan",   { param($c) $c.B -gt 190 -and $c.G -gt 190 -and $c.R -lt 100 })
}

$p = Start-Process -FilePath $exe -ArgumentList "-ms", $MsPerPhase `
    -RedirectStandardOutput $out -RedirectStandardError "$out.err" -PassThru -NoNewWindow

$results = @{}

function Count-Pixels($id) {
    $rule = $expected[$id]
    if ($null -eq $rule) { return -1 }
    $name = $rule[0]; $pred = $rule[1]
    $X = 560; $Y = 260; $W = 380; $H = 240
    $b = New-Object System.Drawing.Bitmap($W, $H)
    $g2 = [System.Drawing.Graphics]::FromImage($b)
    $g2.CopyFromScreen($X, $Y, 0, 0, $b.Size)
    $hit = 0
    for ($y = 0; $y -lt $b.Height; $y += 3) {
        for ($x = 0; $x -lt $b.Width; $x += 3) {
            $c = $b.GetPixel($x, $y)
            if (& $pred $c) { $hit++ }
        }
    }
    $b.Dispose()
    return $hit
}

"capture start (ms/phase=$MsPerPhase) pid=$($p.Id)"
$done = $false
while (-not $done) {
    $phase = $null
    $seen = @{}
    if (Test-Path $out) {
        try {
            $fs = New-Object System.IO.FileStream($out, [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
            $sr = New-Object System.IO.StreamReader($fs)
            $content = $sr.ReadToEnd()
            $sr.Close()
            foreach ($line in ($content -split "`r?`n")) {
                if ($line -match '^PHASE (\d+) (\S+)') {
                    $id = [int]$Matches[1]
                    if (-not $seen.ContainsKey($id)) {
                        $seen[$id] = $true
                        $phase = $id
                    }
                }
                if ($line -match '=== all phases done ===') { $done = $true }
            }
        } catch { }
    }
    if ($phase -and -not $results.ContainsKey($phase)) {
        Start-Sleep -Milliseconds 1500
        $hit = Count-Pixels $phase
        $results[$phase] = $hit
        "RESULT phase $phase $($expected[$phase][0]) hit=$hit"
        Start-Sleep -Milliseconds $MsPerPhase  # let this phase finish before next
    }
    if ($p.HasExited) { $done = $true }
    Start-Sleep -Milliseconds 200
}

Start-Sleep -Milliseconds 800
"=== final (phase: name hit) ==="
foreach ($k in ($results.Keys | Sort-Object)) {
    "phase $k -> $($expected[$k][0]) visible-px=$($results[$k])"
}
"capture done exit=$($p.ExitCode)"
