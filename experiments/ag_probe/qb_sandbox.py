# qb_sandbox.py — run a freshly built QuotaBar.exe against an isolated profile, so the real
# %LOCALAPPDATA%\QuotaBar is never touched during verification.
#
#   python -I qb_sandbox.py setup  <root> <real|empty|fake>   create profile + a sandbox config.json
#   python -I qb_sandbox.py launch <root>                    launch with redirected env; prints pid
#     (env QB_EXE=<path> overrides the exe; launching a 2nd time just asks the 1st to show its window)
#   python -I qb_sandbox.py log    <root> [n]               tail the sandbox log (token-safe)
#   python -I qb_sandbox.py close  <pid>                    WM_CLOSE to the hidden window -> clean exit
#
# Redirection: USERPROFILE / LOCALAPPDATA / APPDATA / HOMEDRIVE+HOMEPATH -> <root>\profile.
# SHGetKnownFolderPath(FOLDERID_LocalAppData) follows the overridden USERPROFILE (verified
# 2026-11-27: config/history/logs all landed under <root>\profile\AppData\Local\QuotaBar).
# ANTIGRAVITY_GEMINI_DIR points at the real ~/.gemini (real), an empty dir (hub absent) or a
# fake-hub dir (fake; see fake_hub_drive.py). The config is hand-written test data, NOT the user's.
#
# Note: the single-instance mutex is per logon session, so the user's own QuotaBar must be
# closed first; and plain `taskkill /IM QuotaBar.exe` does not reliably reach the hidden
# window -- use `close`, which posts WM_CLOSE to it directly.
import json, os, pathlib, re, subprocess, sys

EXE = os.environ.get('QB_EXE',
                     str(pathlib.Path(__file__).resolve().parents[2] / 'build' / 'QuotaBar.exe'))


def cmd_setup(root, mode):
    root = pathlib.Path(root)
    profile = root / 'profile'
    for d in ('AppData/Local', 'AppData/Roaming'):
        (profile / d).mkdir(parents=True, exist_ok=True)
    data = profile / 'AppData' / 'Local' / 'QuotaBar'
    data.mkdir(parents=True, exist_ok=True)
    cfg = {
        'config_version': 2, 'display': {'mode': 3, 'two_lines': True},
        'query': {'interval_sec': 60}, 'notify': {'enabled': False},
        'first_run_done': True, 'ui_theme': 0, 'account_seq': 2,
        'accounts': [
            # type 4 = antigravity (no key; reads the local hub)
            {'id': 'acc1', 'type': 4, 'alias': 'Antigravity', 'enabled': True,
             'card_visible': True, 'cred_rev': 1, 'alert_pct': 0},
            # an unconfigured API-key card, to see the two card kinds side by side
            {'id': 'acc2', 'type': 0, 'alias': 'Kimi Code', 'enabled': True,
             'card_visible': True, 'cred_rev': 0, 'alert_pct': 0},
        ],
    }
    (data / 'config.json').write_text(json.dumps(cfg, ensure_ascii=False), encoding='utf-8')
    real = os.environ['USERPROFILE'] + '\\.gemini'
    gemini = {'real': real, 'empty': str(root / 'emptygemini'),
              'fake': str(root / 'fakegemini')}[mode]
    pathlib.Path(gemini).mkdir(parents=True, exist_ok=True)
    (root / 'gemini_dir.txt').write_text(gemini, encoding='utf-8')
    print('sandbox data dir :', data)
    print('gemini dir       :', gemini)


def cmd_launch(root):
    root = pathlib.Path(root)
    profile = root / 'profile'
    env = dict(os.environ)
    env['USERPROFILE'] = str(profile)
    env['LOCALAPPDATA'] = str(profile / 'AppData' / 'Local')
    env['APPDATA'] = str(profile / 'AppData' / 'Roaming')
    env['HOMEDRIVE'] = profile.drive
    env['HOMEPATH'] = str(profile)[len(profile.drive):]
    env['ANTIGRAVITY_GEMINI_DIR'] = (root / 'gemini_dir.txt').read_text(encoding='utf-8')
    p = subprocess.Popen([EXE], env=env, cwd=str(root), close_fds=True,
                         creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)
    print(p.pid)


def cmd_log(root, n=40):
    log = pathlib.Path(root) / 'profile' / 'AppData' / 'Local' / 'QuotaBar' / 'logs' / 'quota_1.log'
    if not log.exists():
        print('(no sandbox log yet)')
        return
    lines = log.read_bytes().decode('utf-8', 'replace').splitlines()
    for l in lines[-int(n):]:
        print(re.sub(r'[A-Za-z0-9_\-]{36,}', '<redacted>', l))


def cmd_close(pid):
    # WM_CLOSE (0x0010) to QuotaBarHiddenWnd: the app saves cache/history/config and exits.
    ps = ("Add-Type @'\nusing System; using System.Text; using System.Runtime.InteropServices;\n"
          "public static class CL {\n"
          "  public delegate bool P(IntPtr h, IntPtr l);\n"
          "  [DllImport(\"user32.dll\")] public static extern bool EnumWindows(P cb, IntPtr l);\n"
          "  [DllImport(\"user32.dll\", CharSet=CharSet.Unicode)] public static extern int "
          "GetClassName(IntPtr h, StringBuilder s, int n);\n"
          "  [DllImport(\"user32.dll\")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);\n"
          "  [DllImport(\"user32.dll\")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);\n"
          "  public static int Close(uint pid) { int n = 0; EnumWindows((h, l) => { uint p; "
          "GetWindowThreadProcessId(h, out p); if (p == pid) { var c = new StringBuilder(128); "
          "GetClassName(h, c, 128); if (c.ToString() == \"QuotaBarHiddenWnd\") { "
          "PostMessage(h, 0x0010, IntPtr.Zero, IntPtr.Zero); n++; } } return true; }, IntPtr.Zero); return n; }\n}\n'@\n"
          "[CL]::Close([uint32]%d)\n" % int(pid))
    out = subprocess.run(['powershell', '-NoProfile', '-Command', ps], capture_output=True)
    print('posted WM_CLOSE to', out.stdout.decode('ascii', 'replace').strip(), 'hidden window(s)')


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'close':
        cmd_close(int(sys.argv[2]))
    else:
        {'setup': cmd_setup, 'launch': cmd_launch, 'log': cmd_log}[cmd](*sys.argv[2:])
