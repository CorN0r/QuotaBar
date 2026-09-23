# Inject WM_LBUTTONDOWN/UP on the Save (IDOK) button of the display settings dialog.
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W5 {
    public delegate bool P(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(P c, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, P c, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder s, int m);
    public struct RECT { public int L; public int T; public int R; public int B; }
    public static IntPtr Dlg = IntPtr.Zero;
    public static IntPtr SaveBtn = IntPtr.Zero;
    public static uint Target;
}
'@
[W5]::Target = [uint32](Get-Process QuotaBar | Select-Object -First 1).Id

$findDlg = [W5+P] {
    param($h, $l)
    $p = 0
    [W5]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
    if ($p -eq [W5]::Target -and [W5]::IsWindowVisible($h)) {
        $cls = New-Object System.Text.StringBuilder 64
        [W5]::GetClassNameW($h, $cls, 64) | Out-Null
        if ($cls.ToString() -eq 'QuotaBarDisplayDlg') { [W5]::Dlg = $h }
    }
    return $true
}
[W5]::EnumWindows($findDlg, [IntPtr]::Zero) | Out-Null
Write-Host ("dlg=" + [W5]::Dlg)
if ([W5]::Dlg -eq [IntPtr]::Zero) { exit 1 }

$findBtn = [W5+P] {
    param($h, $l)
    if ([W5]::GetDlgCtrlID($h) -eq 1) { [W5]::SaveBtn = $h }   # IDOK
    return $true
}
[W5]::EnumChildWindows([W5]::Dlg, $findBtn, [IntPtr]::Zero) | Out-Null
Write-Host ("save btn=" + [W5]::SaveBtn)
if ([W5]::SaveBtn -eq [IntPtr]::Zero) { exit 1 }

$r = New-Object W5+RECT
[W5]::GetWindowRect([W5]::SaveBtn, [ref]$r) | Out-Null
$cx = [int](($r.R - $r.L) / 2); $cy = [int](($r.B - $r.T) / 2)
$lparam = [IntPtr]((($cy -shl 16) -bor ($cx -band 0xFFFF)))
[W5]::PostMessage([W5]::SaveBtn, 0x0201, [IntPtr]1, $lparam) | Out-Null
Start-Sleep -Milliseconds 80
[W5]::PostMessage([W5]::SaveBtn, 0x0202, [IntPtr]0, $lparam) | Out-Null
Write-Host ("posted LDOWN/LUP at client ($cx,$cy)")
