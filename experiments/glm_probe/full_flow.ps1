# Click kModeRings radio (id=1205) then Save (id=1) in the display dialog.
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class W7 {
    public delegate bool P(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, P c, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    public struct RECT { public int L; public int T; public int R; public int B; }
    public static IntPtr Radio = IntPtr.Zero;
    public static IntPtr SaveBtn = IntPtr.Zero;
}
'@
$find = [W7+P] {
    param($h, $l)
    $id = [W7]::GetDlgCtrlID($h)
    if ($id -eq 1205) { [W7]::Radio = $h }
    if ($id -eq 1) { [W7]::SaveBtn = $h }
    return $true
}
$dlg = [IntPtr][int]0
# reuse: enumerate from the desktop is overkill; the dialog is the radio's top-level ancestor.
# Instead enumerate children of the known dialog class instance via GetParent chain is complex;
# simplest: find dialog again by class.
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W8 {
    public delegate bool P(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(P c, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder s, int m);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder s, int m);
    public static IntPtr Dlg = IntPtr.Zero;
    public static uint Target;
}
'@
[W8]::Target = [uint32](Get-Process QuotaBar | Select-Object -First 1).Id
$findDlg = [W8+P] {
    param($h, $l)
    $p = 0
    [W8]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
    if ($p -eq [W8]::Target -and [W8]::IsWindowVisible($h)) {
        $s = New-Object System.Text.StringBuilder 256
        [W8]::GetWindowTextW($h, $s, 256) | Out-Null
        $cls = New-Object System.Text.StringBuilder 64
        [W8]::GetClassNameW($h, $cls, 64) | Out-Null
        if ($cls.ToString() -eq 'QuotaBarDisplayDlg') { [W8]::Dlg = $h }
    }
    return $true
}
[W8]::EnumWindows($findDlg, [IntPtr]::Zero) | Out-Null
Write-Host ("dlg=" + [W8]::Dlg)
[W7]::EnumChildWindows([W8]::Dlg, $find, [IntPtr]::Zero) | Out-Null
Write-Host ("radio=" + [W7]::Radio + " save=" + [W7]::SaveBtn)
if ([W7]::Radio -eq [IntPtr]::Zero -or [W7]::SaveBtn -eq [IntPtr]::Zero) { exit 1 }
[W7]::SendMessage([W7]::Radio, 0x00F5, [IntPtr]0, [IntPtr]0) | Out-Null   # BM_CLICK radio
Start-Sleep -Milliseconds 200
$r = New-Object W7+RECT
[W7]::GetWindowRect([W7]::SaveBtn, [ref]$r) | Out-Null
$cx = [int](($r.R - $r.L) / 2); $cy = [int](($r.B - $r.T) / 2)
$lparam = [IntPtr]((($cy -shl 16) -bor ($cx -band 0xFFFF)))
[W7]::PostMessage([W7]::SaveBtn, 0x0201, [IntPtr]1, $lparam) | Out-Null
Start-Sleep -Milliseconds 80
[W7]::PostMessage([W7]::SaveBtn, 0x0202, [IntPtr]0, $lparam) | Out-Null
Write-Host "clicked radio + save"
