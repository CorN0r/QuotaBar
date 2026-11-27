# ag_probe — Antigravity 额度（本机 hub）真机探针与隔离联调

2026-11-27 接入 Antigravity 时建立。**额度不是查 Google，而是查本机 Antigravity 的
hub（Language Server）**——认证、令牌刷新全由 hub 代劳，QuotaBar 全程不接触 OAuth 令牌。

| 文件 | 用途 |
| --- | --- |
| `probe_quota.py` | 只读探针：找存活 hub → `GetUserStatus`（脱敏摘要）→ `RetrieveUserQuotaSummary`。`--save` 导出样本 |
| `ag_response.json` | 真机响应样本（三种请求变体各一份；文案截短以外的字段/数值/数组序原样），`ParseAntigravityResponse` 契约依据 |
| `winhttp_loopback_proxy_test.ps1` | 对照实验：会话级指向假代理时，回环请求是否仍直达（A/B/C/D + 对照 E） |
| `qb_sandbox.py` | 用隔离的用户目录启动任意 QuotaBar.exe 做联调，**不碰真实 `%LOCALAPPDATA%\QuotaBar`** |
| `fake_hub_drive.py` | 故障路径联调：受控假 hub 依次制造「残留描述文件 / 401 / 成功 / 进程消失」 |

## 契约（Antigravity 44.9.1 / LS 2.35.2 真机锁定）

- **hub 描述文件**：`~/.gemini/<product>/daemon/hub_<httpPort>.json` =
  `{pid, httpsPort, httpPort, lsVersion, csrfToken}`，product ∈ antigravity-cli / antigravity /
  antigravity-ide / antigravity-idea（基目录可被环境变量 `ANTIGRAVITY_GEMINI_DIR` 覆盖）。
  hub 每次启动写一份新文件、**退出不清理**（本机实测残留 6 份）→ 必须按 pid 存活筛选、
  再取 mtime 最新。
- **额度 RPC**：`POST http://127.0.0.1:<httpPort>/exa.language_server_pb.LanguageServerService/RetrieveUserQuotaSummary`，
  头 `x-codeium-csrf-token: <csrfToken>`、`Content-Type: application/json`，体 `{}`。
- **响应**：`{"response":{"groups":[{"displayName","buckets":[{"bucketId","window",
  "remainingFraction","resetTime"?}]}]}}`。实测桶：`gemini-5h` / `gemini-weekly`
  （"Gemini Models"）、`3p-5h` / `3p-weekly`（"Claude and GPT models"）。
- 鉴权失败 401：`{"code":"unauthenticated","message":"unauthenticated: missing|invalid CSRF token"}`。

### 与"常识"不同、只有真机才看得出的点

1. **本机是 Electron 新架构**（`Antigravity.exe` + `language_server.exe`），数据在
   `~/.gemini/`——不是社区文章里 VS Code fork 时代的 `~/.codeium/antigravity/auth_token`。
   磁盘上也没有可直接使用的 OAuth 访问令牌；令牌只在 hub 进程里。
2. **组内 weekly 排在 5h 前面**。解析必须按 `bucketId` 分派，不能按数组序。
3. **没动过的桶（剩余 1.0）没有 `resetTime` 字段**；额度耗尽时 `remainingFraction: 0` 是
   proto3 零值，**整个字段被省略**（以 `disabled` 或未过期的 `resetTime` 佐证为 0%）。
4. `forceRefresh` / `connect-protocol-version` 头对响应信封没有影响（三种变体同形）。
5. **WinHTTP 天生不让回环地址走代理**：`winhttp_loopback_proxy_test.ps1` 把会话级代理指向
   `127.0.0.1:9`（对照 E 证明非回环地址确实连不上，12029），回环 hub 仍然 200——HTTP 层
   无需为 hub 做按请求的代理覆盖（AUTOMATIC_PROXY 会话上 `WINHTTP_OPTION_PROXY` 还不可设，
   12011）。
6. 社区另报道的直连路线（`daily-cloudcode-pa.googleapis.com`，需 OAuth 令牌且 User-Agent
   版本段决定返回哪些桶）未采用：要自己拿令牌、脆弱，且本机磁盘上根本没有。

## 复跑

```bat
python -I experiments\ag_probe\probe_quota.py --save experiments\ag_probe\ag_response.json
powershell -NoProfile -ExecutionPolicy Bypass -File experiments\ag_probe\winhttp_loopback_proxy_test.ps1
```

探针里的 CSRF 令牌只在内存里组请求头，不打印、不落盘；`GetUserStatus` 只输出键名与
套餐档位，邮箱/姓名脱敏。

## 隔离联调（qb_sandbox.py）

单实例互斥量按登录会话计，跑沙箱前要先退出正在运行的 QuotaBar。做法是覆盖
`USERPROFILE`/`LOCALAPPDATA`/`APPDATA` 启动新 exe：`SHGetKnownFolderPath(LOCALAPPDATA)`
 会跟着重定向（实测 config/history/logs 全落在 `<root>\profile\AppData\Local\QuotaBar`，
真实目录的 config/secrets mtime 全程未变），而 `ANTIGRAVITY_GEMINI_DIR` 指回真实
`~/.gemini` 即可连真 hub。

```bat
set S=%TEMP%\qb_sandbox
python -I experiments\ag_probe\qb_sandbox.py setup  %S% real    :: real=真 hub；empty=hub 缺席
python -I experiments\ag_probe\qb_sandbox.py launch %S%                            :: 打印 pid
python -I experiments\ag_probe\qb_sandbox.py launch %S%                            :: 第二次启动=请首实例显示主窗口
python -I experiments\ag_probe\qb_sandbox.py log    %S%
python -I experiments\ag_probe\qb_sandbox.py close  <pid>
```

`fake_hub_drive.py %S%` 要求沙箱以 `empty` 模式启动且配置 `interval_sec=60`（setup 已是），
约 5 分钟跑完四个阶段，结束时打印每条请求的校验结果（路径/令牌头/是否误发 Authorization/
请求体/Content-Type）。

2026-11-27 实跑结论：P1（pid 已死、mtime 最新的残留描述文件被忽略）在真实二进制上两轮
全过；P2 中应用在一个重探周期内发现了新出现的 hub 并发出请求；P2–P4 的后续阶段因用户
回到工位、需要让出单实例位而中止——401 不断机、连接被拒走短重探、成功发布快照这三条
由 `TestSchedulerBasics` 的 `HubHost` 用例覆盖（selftest）。

## 联调手段上的坑（下次少走弯路）

- **关实例别用 `taskkill /IM QuotaBar.exe`**：它并不总能把 WM_CLOSE 送到隐藏消息窗口
  （实测同一会话内一次能退、两次报"已发送终止信号"却仍在运行）。直接向
  `QuotaBarHiddenWnd` 投递 `WM_CLOSE`（`qb_sandbox.py close`）稳定走"退出中"→落盘→退出。
- **锁屏时**：整屏截图是黑的；`PrintWindow(hwnd, hdc, 2)` 仍能拿到窗口内容（数据来自窗口
  自身的 DWM 表面），但窗口刚带淡入动画显示后的头几秒可能给空白帧，等 3–4 秒再抓。
  需要抓瞬态时用 flags=0（走 WM_PRINT 同步重绘）。
- **锁屏时合成点击无效，且与版本无关**：`PostMessage` 的鼠标消息进不了窗口过程
  （前置的 `IsDialogMessageW` 拦下），`SendMessage` 能执行到窗口过程（滚轮消息的无条件
  日志可证）但 `HitAt` 恒不命中。改动前的 v1.2.1 发行包在同样条件下表现完全相同
  （对照实验），不是回归。真实键鼠输入一切正常。
- **别把测试实例留在用户面前**：沙箱实例和真实实例共用单实例位，且用户可能把它当成自己
  的 QuotaBar 去操作（本次就发生了：测试窗口弹出后用户在里面点了添加面板）。联调一结束
  立刻 `close` 沙箱并重新启动真实实例。
- 以为"点击生效了"之前先核对时间线：5 分钟定时刷新很容易和手动点击撞在同一分钟。
