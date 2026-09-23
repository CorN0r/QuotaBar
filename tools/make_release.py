# -*- coding: utf-8 -*-
# make_release.py — 发行打包：构建 → 自测门禁 → 组 zip。
# 用法：python tools/make_release.py [版本号]，缺省从 assets/quota_bar.rc 读 APP_VER_S。
import re, subprocess, sys, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main():
    ver = None
    if len(sys.argv) > 1:
        ver = sys.argv[1]
    else:
        rc = (ROOT / 'assets' / 'quota_bar.rc').read_text(encoding='utf-8', errors='strict')
        m = re.search(r'#define APP_VER_S "([0-9.]+)"', rc)
        assert m, 'APP_VER_S not found in quota_bar.rc'
        parts = m.group(1).split('.')
        while len(parts) > 3 and parts[-1] == '0':
            parts.pop()
        ver = '.'.join(parts)

    print(f'[release] version {ver}')
    print('[release] building...')
    # build.bat 输出是 GBK（MSVC 中文诊断），按字节收再宽松解码
    r = subprocess.run(['cmd', '/c', 'build.bat'], cwd=ROOT, capture_output=True)
    out = (r.stdout or b'').decode('gbk', errors='replace') + \
          (r.stderr or b'').decode('gbk', errors='replace')
    assert r.returncode == 0 and 'OK' in out, 'build failed:\n' + out[-2000:]
    for line in out.splitlines():
        if 'warning' in line.lower() or 'error' in line.lower():
            raise SystemExit('[release] warning/error in build:\n' + line)

    print('[release] selftest...')
    st = subprocess.run([str(ROOT / 'build' / 'QuotaBar.exe'), '--selftest'],
                        cwd=ROOT, capture_output=True, timeout=120)
    assert st.returncode == 0, f'selftest failed rc={st.returncode}'
    tail = (ROOT / 'build' / 'selftest.txt').read_text(encoding='utf-8').strip().splitlines()[-1]
    print('[release]', tail)
    assert '失败 0' in tail, 'selftest has failures'

    dist = ROOT / 'dist'
    dist.mkdir(exist_ok=True)
    zpath = dist / f'QuotaBar-v{ver}-x64.zip'
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(ROOT / 'build' / 'QuotaBar.exe', 'QuotaBar.exe')
        z.write(ROOT / 'README.md', 'README.md')
        z.write(ROOT / 'RELEASE-NOTES.md', 'RELEASE-NOTES.md')
        z.write(ROOT / 'LICENSE', 'LICENSE')
    print(f'[release] {zpath} ({zpath.stat().st_size} bytes)')

if __name__ == '__main__':
    main()
