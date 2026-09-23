Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class KB9 {
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static void TapWin() {
        keybd_event(0x5B, 0, 0, UIntPtr.Zero);
        keybd_event(0x5B, 0, 2, UIntPtr.Zero);
    }
}
"@
[Kb9]::TapWin()
Write-Host "win tapped"
