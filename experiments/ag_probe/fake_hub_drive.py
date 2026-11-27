# fake_hub_drive.py <sandbox_root> -- failure-path integration for the Antigravity provider.
#
# Runs against an ALREADY RUNNING sandbox QuotaBar (see qb_sandbox.py; set up with mode
# "empty", i.e. ANTIGRAVITY_GEMINI_DIR = <root>/emptygemini and no hub descriptor yet). A
# controlled fake hub -- plain local HTTP, fake token only, no real credential anywhere --
# is brought up / torn down while the app's own ~60s re-probe loop reacts:
#
#   P1  only a stale descriptor whose pid is dead, with the NEWEST mtime  -> must be ignored
#   P2  live descriptor; fake hub answers 401                              -> AuthError, account NOT stopped
#   P3  fake hub answers 200 with the recorded real sample                -> "acc1 查询成功"
#   P4  the fake hub + the descriptor's pid die                            -> "本机 hub 未运行，转入重探" (once)
#
# Every request the fake hub receives is validated (method/path/headers/body) and appended
# to <root>/fakehub/requests.log, then summarised at the end. Takes ~5 minutes (one re-probe
# cycle per phase). Requires the sandbox config interval_sec = 60 so P4 is seen within a cycle.
import http.server, json, os, pathlib, subprocess, sys, threading, time

root = pathlib.Path(sys.argv[1])
hub_dir = root / 'fakehub'
daemon = root / 'emptygemini' / 'antigravity' / 'daemon'
hub_dir.mkdir(parents=True, exist_ok=True)
daemon.mkdir(parents=True, exist_ok=True)
TOKEN = 'fake-csrf-token-000000000000000000000'
RPC = '/exa.language_server_pb.LanguageServerService/RetrieveUserQuotaSummary'
SAMPLE = json.loads((pathlib.Path(__file__).parent / 'ag_response.json')
                    .read_text(encoding='utf-8'))['plain']
app_log = root / 'profile' / 'AppData' / 'Local' / 'QuotaBar' / 'logs' / 'quota_1.log'
req_log = hub_dir / 'requests.log'
req_log.write_text('', encoding='utf-8')

state = {'mode': '401'}
lock = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length') or 0))
        with lock:
            with req_log.open('a', encoding='utf-8') as f:
                f.write(json.dumps({
                    't': time.strftime('%H:%M:%S'), 'path_ok': self.path == RPC,
                    'token_ok': self.headers.get('x-codeium-csrf-token') == TOKEN,
                    'authorization_sent': 'Authorization' in self.headers,
                    'body': body.decode('utf-8', 'replace'),
                    'ctype': self.headers.get('Content-Type'), 'ua': self.headers.get('User-Agent'),
                    'mode': state['mode']}) + '\n')
            mode = state['mode']
        status = int(mode) if mode.isdigit() else 200
        payload = (json.dumps(SAMPLE) if status == 200 else
                   json.dumps({'code': 'unauthenticated',
                               'message': 'unauthenticated: invalid CSRF token'})).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def say(msg):
    print(f'[{time.strftime("%H:%M:%S")}] {msg}', flush=True)


def app_lines():
    return app_log.read_bytes().decode('utf-8', 'replace').splitlines() if app_log.exists() else []


def wait_for(pred, what, timeout=95):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pred():
            say(f'OBSERVED  {what}  (after {int(time.time() - t0)}s)')
            return True
        time.sleep(2)
    say(f'TIMEOUT   {what}  ({timeout}s)')
    return False


def request_count():
    return len(req_log.read_text(encoding='utf-8').splitlines())


results = {}
base = len(app_lines())
server = None
holder = None
try:
    # ---- P1: stale descriptor with a dead pid and the newest mtime must be ignored ----
    dead = subprocess.Popen(['cmd', '/c', 'exit', '0'])
    dead.wait()
    stale = daemon / 'hub_1.json'
    stale.write_text(json.dumps({'pid': dead.pid, 'httpsPort': 0, 'httpPort': 1,
                                 'lsVersion': '0.0.0', 'csrfToken': 'stale'}), encoding='utf-8')
    os.utime(stale, None)
    say(f'P1 planted stale descriptor hub_1.json (dead pid {dead.pid}, newest mtime); waiting one re-probe cycle')
    time.sleep(75)
    p1_new = [l for l in app_lines()[base:] if '[fetch]' in l or 'hub' in l]
    results['P1'] = request_count() == 0 and not p1_new
    say(f'P1 {"PASS" if results["P1"] else "FAIL"}: requests={request_count()}, fetch/hub log lines={p1_new}')

    # ---- live fake hub ----
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    holder = subprocess.Popen(['cmd', '/c', 'ping', '-n', '900', '127.0.0.1', '>', 'nul'])
    live = daemon / f'hub_{port}.json'
    live.write_text(json.dumps({'pid': holder.pid, 'httpsPort': 0, 'httpPort': port,
                                'lsVersion': '9.9.9-fake', 'csrfToken': TOKEN}), encoding='utf-8')
    say(f'P2 fake hub listening on 127.0.0.1:{port} (descriptor pid = holder {holder.pid}); mode 401')

    # ---- P2: 401 -> AuthError ----
    wait_for(lambda: request_count() >= 1, 'first request reaching the fake hub')
    time.sleep(4)
    say('P2 app log: ' + ' | '.join(l.split('] ', 1)[-1] for l in app_lines()[base:]
                                    if '[slot]' not in l)[-300:])

    # ---- P3: 200 -> success (and proof that the 401 did not stop the account) ----
    state['mode'] = '200'
    say('P3 switched fake hub to 200; waiting for the next re-probe')
    got2 = wait_for(lambda: request_count() >= 2, 'second request after 401')
    ok = wait_for(lambda: any('acc1 查询成功' in l for l in app_lines()[base:]), '"acc1 查询成功"', 20)
    results['P2/P3'] = got2 and ok
    say(f'P2/P3 {"PASS" if results["P2/P3"] else "FAIL"}: a 401 did not stop the account; recovered to a successful snapshot')

    # ---- P4: hub process dies (descriptor pid dead) -> "未运行" ----
    server.shutdown()
    server.server_close()
    holder.kill()
    holder.wait()
    say('P4 killed fake hub + pid holder; waiting for the app to notice')
    noticed = wait_for(lambda: any('本机 hub 未运行，转入重探' in l for l in app_lines()[base:]),
                       '"本机 hub 未运行，转入重探"')
    time.sleep(70)
    hits = [l for l in app_lines()[base:] if '本机 hub 未运行' in l]
    results['P4'] = noticed and len(hits) == 1
    say(f'P4 {"PASS" if results["P4"] else "FAIL"}: noticed={noticed}, log lines for it={len(hits)} (must be exactly 1: no spam)')
finally:
    if server:
        try:
            server.shutdown()
            server.server_close()
        except Exception:
            pass
    if holder and holder.poll() is None:
        holder.kill()
    for f in daemon.glob('hub_*.json'):
        f.unlink()

say('---- fake hub request log ----')
for l in req_log.read_text(encoding='utf-8').splitlines():
    r = json.loads(l)
    say(f"  mode={r['mode']:>4} path_ok={r['path_ok']} token_ok={r['token_ok']} "
        f"authorization_sent={r['authorization_sent']} body={r['body']!r} ctype={r['ctype']} ua={r['ua']}")
say('---- app log (new part, slot noise removed) ----')
for l in app_lines()[base:]:
    if '[slot]' in l:
        continue
    say('  ' + l)
say('---- RESULT: ' + ('ALL PASS' if all(results.values()) else 'FAIL ' + str(results)) + ' ----')
