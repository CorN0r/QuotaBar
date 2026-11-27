# winhttp_proxy_test.ps1 -- does a per-request WINHTTP_OPTION_PROXY override beat the session proxy?
# Session is opened against a dead proxy (127.0.0.1:9); case A has no override (expect failure),
# case B sets NO_PROXY on the request handle (expect 200 if the override is honored).
# The CSRF token is read from the hub descriptor and never printed. Pure ASCII on purpose.
$ErrorActionPreference = 'Stop'
$src = @'
using System;
using System.Runtime.InteropServices;
public static class WH {
  [StructLayout(LayoutKind.Sequential)] public struct PI { public int Access; public IntPtr Proxy; public IntPtr Bypass; }
  [DllImport("winhttp", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr WinHttpOpen(string agent, int access, string proxy, string bypass, int flags);
  [DllImport("winhttp", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr WinHttpConnect(IntPtr s, string host, ushort port, int rsv);
  [DllImport("winhttp", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr WinHttpOpenRequest(IntPtr c, string verb, string path, IntPtr ver, IntPtr referer, IntPtr accept, int flags);
  [DllImport("winhttp", SetLastError=true)] public static extern bool WinHttpSetOption(IntPtr h, int opt, ref PI info, int len);
  [DllImport("winhttp", SetLastError=true)] public static extern bool WinHttpSetTimeouts(IntPtr h, int r, int c, int s, int rcv);
  [DllImport("winhttp", CharSet=CharSet.Unicode, SetLastError=true)] public static extern bool WinHttpAddRequestHeaders(IntPtr h, string headers, int len, int mods);
  [DllImport("winhttp", CharSet=CharSet.Unicode, SetLastError=true)] public static extern bool WinHttpSendRequest(IntPtr h, string headers, int hlen, byte[] body, int blen, int total, IntPtr ctx);
  [DllImport("winhttp", SetLastError=true)] public static extern bool WinHttpReceiveResponse(IntPtr h, IntPtr rsv);
  [DllImport("winhttp", SetLastError=true)] public static extern bool WinHttpQueryHeaders(IntPtr h, int info, IntPtr name, ref int val, ref int len, IntPtr idx);
  [DllImport("winhttp", SetLastError=true)] public static extern bool WinHttpCloseHandle(IntPtr h);
  public static string Ctrl() {
    IntPtr s0 = WinHttpOpen("QuotaBar-probe", 3, "127.0.0.1:9", null, 0);
    WinHttpSetTimeouts(s0, 3000, 3000, 3000, 3000);
    IntPtr c0 = WinHttpConnect(s0, "example.com", 80, 0);
    IntPtr r0 = WinHttpOpenRequest(c0, "GET", "/", IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, 0);
    bool ok = WinHttpSendRequest(r0, null, 0, null, 0, 0, IntPtr.Zero) && WinHttpReceiveResponse(r0, IntPtr.Zero);
    int err = Marshal.GetLastWin32Error();
    WinHttpCloseHandle(r0); WinHttpCloseHandle(c0); WinHttpCloseHandle(s0);
    return ok ? "unexpectedly succeeded" : "failed as expected (win32=" + err + ")";
  }
  public static string Run(int sessionAccess, bool perReqNoProxy, ushort port, string token) {
    IntPtr s = WinHttpOpen("QuotaBar-probe", sessionAccess, "127.0.0.1:9", null, 0);
    if (s == IntPtr.Zero) return "WinHttpOpen failed win32=" + Marshal.GetLastWin32Error();
    WinHttpSetTimeouts(s, 3000, 3000, 3000, 3000);
    IntPtr c = WinHttpConnect(s, "127.0.0.1", port, 0);
    IntPtr r = WinHttpOpenRequest(c, "POST", "/exa.language_server_pb.LanguageServerService/RetrieveUserQuotaSummary", IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, 0);
    int setResult = 0;
    if (perReqNoProxy) {
      PI pi = new PI(); pi.Access = 1; pi.Proxy = IntPtr.Zero; pi.Bypass = IntPtr.Zero;
      setResult = WinHttpSetOption(r, 38, ref pi, Marshal.SizeOf(pi)) ? 1 : -Marshal.GetLastWin32Error();
    }
    string hdr = "Content-Type: application/json\r\nx-codeium-csrf-token: " + token + "\r\n";
    WinHttpAddRequestHeaders(r, hdr, hdr.Length, 0x20000000 | 0x40000000);
    byte[] body = new byte[] { 0x7B, 0x7D };
    bool sent = WinHttpSendRequest(r, null, 0, body, 2, 2, IntPtr.Zero);
    int status = 0;
    if (sent) {
      if (WinHttpReceiveResponse(r, IntPtr.Zero)) {
        int len = 4;
        WinHttpQueryHeaders(r, 19 | 0x20000000, IntPtr.Zero, ref status, ref len, IntPtr.Zero);
      } else { status = -300 - Marshal.GetLastWin32Error(); }
    } else { status = -200 - Marshal.GetLastWin32Error(); }
    WinHttpCloseHandle(r); WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return "http=" + status + " setOption=" + setResult;
  }
}
'@
Add-Type -TypeDefinition $src
$dir = Join-Path $env:USERPROFILE '.gemini\antigravity\daemon'
$live = Get-Process language_server -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id
$cand = Get-ChildItem $dir -Filter 'hub_*.json' | ForEach-Object {
  $j = Get-Content $_.FullName -Raw | ConvertFrom-Json
  if ($live -contains [int]$j.pid) { [pscustomobject]@{ m = $_.LastWriteTimeUtc; p = [int]$j.httpPort; t = [string]$j.csrfToken } }
} | Sort-Object m -Descending | Select-Object -First 1
if (-not $cand) { 'no live hub'; exit 2 }
"hub httpPort=$($cand.p) tokenLen=$($cand.t.Length)"
"E (control) dead-proxy session, non-loopback example.com: " + [WH]::Ctrl()
"A: session=named-dead-proxy, no per-request override  -> status $([WH]::Run(3, $false, [uint16]$cand.p, $cand.t))"
"B: session=named-dead-proxy, per-request NO_PROXY     -> status $([WH]::Run(3, $true, [uint16]$cand.p, $cand.t))"
"C: session=automatic-proxy,   no per-request override  -> status $([WH]::Run(4, $false, [uint16]$cand.p, $cand.t))"
"D: session=automatic-proxy,   per-request NO_PROXY     -> status $([WH]::Run(4, $true, [uint16]$cand.p, $cand.t))"
