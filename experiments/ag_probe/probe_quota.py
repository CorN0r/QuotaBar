# probe_quota.py — Antigravity hub（Language Server）只读探针：真机锁定额度契约。
# 用法：python -I probe_quota.py [--save 样本输出路径]
#
# 路线（2026-11-27 Antigravity 44.9.1 / LS 2.35.2 真机确认）：
#   ~/.gemini/<product>/daemon/hub_<pid>.json = {pid, httpsPort, httpPort, lsVersion, csrfToken}
#   POST http://127.0.0.1:<httpPort>/exa.language_server_pb.LanguageServerService/<Method>
#        头 x-codeium-csrf-token: <csrfToken>；体 {}
#   RetrieveUserQuotaSummary → {response:{groups[]|buckets[]}}（官方实时配额摘要）
#
# CSRF 令牌只在内存里用：不打印、不落盘、不进日志。GetUserStatus 里的邮箱/姓名同样脱敏。
import ctypes, json, os, re, ssl, sys, urllib.error, urllib.request

home = os.environ.get('USERPROFILE') or os.path.expanduser('~')
base = os.environ.get('ANTIGRAVITY_GEMINI_DIR') or os.path.join(home, '.gemini')
ROOTS = ['antigravity-cli', 'antigravity', 'antigravity-ide', 'antigravity-idea']
RPC = '/exa.language_server_pb.LanguageServerService/'
PII_KEY = re.compile(r'token|secret|^email$|^name$', re.I)


def pid_alive(pid):
    k = ctypes.windll.kernel32
    k.OpenProcess.restype = ctypes.c_void_p
    h = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    code = ctypes.c_ulong()
    k.GetExitCodeProcess(ctypes.c_void_p(h), ctypes.byref(code))
    k.CloseHandle(ctypes.c_void_p(h))
    return code.value == 259  # STILL_ACTIVE


def candidates():
    out = []
    for root in ROOTS:
        d = os.path.join(base, root, 'daemon')
        if not os.path.isdir(d):
            continue
        for fn in os.listdir(d):
            if not re.match(r'^hub_.*\.json$', fn, re.I):
                continue
            p = os.path.join(d, fn)
            try:
                with open(p, encoding='utf-8') as f:
                    j = json.load(f)
                tok = str(j.get('csrfToken') or j.get('csrf_token') or '').strip()
                pid = int(j.get('pid') or 0)
            except Exception:
                continue  # 损坏/正在替换的文件
            if not tok or (pid > 0 and not pid_alive(pid)):
                continue
            out.append({
                'file': p, 'mtime': os.path.getmtime(p), 'pid': pid,
                'http': int(j.get('httpPort') or j.get('port') or 0),
                'https': int(j.get('httpsPort') or 0),
                'ver': str(j.get('lsVersion') or ''), 'tok': tok,
            })
    out.sort(key=lambda c: c['mtime'], reverse=True)
    return out


def call(url, tok, method, body, extra=None, timeout=6):
    hdr = {'Content-Type': 'application/json', 'x-codeium-csrf-token': tok}
    hdr.update(extra or {})
    req = urllib.request.Request(url + RPC + method, data=json.dumps(body).encode(),
                                 headers=hdr, method='POST')
    # 回环直连：禁用系统代理；hub 的 https 为自签证书
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({}),
        urllib.request.HTTPSHandler(context=ssl._create_unverified_context()))
    try:
        with opener.open(req, timeout=timeout) as r:
            return r.status, r.read().decode('utf-8', 'replace')
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode('utf-8', 'replace')
    except Exception as e:  # 连接拒绝/超时/TLS
        return 0, type(e).__name__


def redact(v):
    # 只脱敏邮箱/姓名/令牌类键；bucketId/displayName/description 是产品文案，保留
    if isinstance(v, dict):
        return {k: ('<redacted>' if PII_KEY.search(k) and isinstance(x, str) and x
                    else redact(x)) for k, x in v.items()}
    if isinstance(v, list):
        return [redact(x) for x in v]
    if isinstance(v, str) and '@' in v:
        return f'<redacted len={len(v)}>'
    return v


def status_summary(j):
    # GetUserStatus 有 52KB（逐模型配置）：只摘与额度相关的字段
    us = j.get('userStatus') or {}
    plan = ((us.get('planStatus') or {}).get('planInfo') or {})
    cfgs = ((j.get('cascadeModelConfigData') or {}).get('clientModelConfigs') or [])
    models = [{'label': c.get('label'), 'model': (c.get('modelOrAlias') or {}).get('model'),
               'quotaInfo': c.get('quotaInfo')} for c in cfgs]
    return {
        'top_keys': list(j.keys()),
        'userStatus_keys': list(us.keys()),
        'teamsTier': plan.get('teamsTier'),
        'planName': plan.get('planName'),
        'clientModelConfigs_count': len(cfgs),
        'clientModelConfigs[0..5]': models[:6],
        'distinct_quotaInfo': sorted({json.dumps(m['quotaInfo'], sort_keys=True)
                                      for m in models}),
    }


def main():
    save = sys.argv[sys.argv.index('--save') + 1] if '--save' in sys.argv else None
    cands = candidates()
    if not cands:
        print('no live hub_*.json (Antigravity 未运行或 2.33.0+ 未写入 csrfToken)')
        return 2
    for c in cands:
        print(f"hub pid={c['pid']} http={c['http']} https={c['https']} ls={c['ver']!r} "
              f"file={os.path.relpath(c['file'], home)}")
    c = cands[0]
    base_url = None
    for proto, port in (('http', c['http']), ('https', c['https']), ('https', c['http'])):
        if not port:
            continue
        st, body = call(f'{proto}://127.0.0.1:{port}', c['tok'], 'GetUserStatus', {})
        print(f"probe GetUserStatus {proto}://{port} -> {st} ({len(body)}B)")
        if st == 200:
            base_url = f'{proto}://127.0.0.1:{port}'
            try:
                print('  GetUserStatus summary:', json.dumps(redact(status_summary(json.loads(body))),
                                                             ensure_ascii=False, indent=2))
            except Exception:
                print('  (non-JSON body head)', body[:60])
            break
        else:
            print('  body head:', body[:120])
    if not base_url:
        print('no reachable hub endpoint')
        return 3

    results = {}
    for label, body, extra in (
            ('plain', {}, None),
            ('forceRefresh', {'forceRefresh': True}, None),
            ('connect-v1', {}, {'connect-protocol-version': '1'})):
        st, txt = call(base_url, c['tok'], 'RetrieveUserQuotaSummary', body, extra)
        print(f"\n=== RetrieveUserQuotaSummary [{label}] -> {st} ({len(txt)}B)")
        try:
            j = json.loads(txt)
        except Exception:
            print('  non-JSON:', txt[:200])
            continue
        print(json.dumps(redact(j), ensure_ascii=False, indent=2))
        results[label] = j
    # 401 对照：缺令牌/错令牌的服务端行为（调度器停机语义的依据）
    for label, tok in (('no-token', ''), ('bad-token', 'x' * 36)):
        st, txt = call(base_url, tok, 'RetrieveUserQuotaSummary', {})
        print(f"auth probe [{label}] -> {st} {txt[:120]!r}")
    if save:
        with open(save, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(results, f, ensure_ascii=False, indent=2)
            f.write('\n')
        print(f'\nsaved {save}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
