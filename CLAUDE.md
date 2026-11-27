# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

QuotaBar:Windows 11 x64 单进程额度常驻显示小工具（多账户卡片模型：Kimi Code 周期额度 / DeepSeek API 余额 / Codex 订阅额度 / GLM Coding Plan 额度 / Antigravity 订阅额度，每个 coding plan 账户一张卡片，Kimi/GLM/DeepSeek 可加多张，Codex（免密直连 auth.json）与 Antigravity（本机 hub 直连）各限一张）。C++20 + 原生 Win32,透明文字小窗贴靠任务栏 + 托盘图标。设计依据在仓库外的 `outputs/Windows额度插件-开发设计文档.md`(ru-g 目录,由 CMakeLists 注释引用);资源预算、验收清单、公开契约结论都在该文档与 `README.md` 中,改动功能前先读。

## 构建与测试

```bat
build.bat                                    :: 主构建(需 VS2022 BuildTools 的 MSVC,无 CMake 也可)
D:\Zcode\QuotaBar\build\QuotaBar.exe --selftest   :: 合成自测(339 项,不访问网络、不读写数据目录)
:: 结果:退出码 0=全过;结果追加写 build\selftest.txt(仅真实控制台才打印到屏幕)——文件是 APPEND 语义
cmake -B build-cmake -G "Visual Studio 17 2022" -A x64   :: 备选构建
```

发布打包:先杀掉运行中的 QuotaBar.exe → `python tools/make_release.py`(版本号读自
assets/quota_bar.rc 的 APP_VER_S):构建 → 自测门禁 → 产 dist\QuotaBar-vX.Y.Z-x64.zip
(exe+README+RELEASE-NOTES+LICENSE);发布结论回写 README/RELEASE-NOTES。性能采样用后台脚本 60s×N 记录
WorkingSet/PrivateMemory/CPU,结论回写 RELEASE-NOTES。

- 源文件全部 UTF-8 无 BOM,编译带 `/utf-8`;**`build.bat` 必须是 GBK + CRLF 编码**(cmd.exe 解析中文注释),不要重新存为 UTF-8,否则整批命令乱码报废。**禁止用 sed/任何文本工具改 build.bat**(2026-09-18 实测事故:MSYS2 sed -i 即使零命中也会把 GBK 中文替换成 `?`、CRLF 吞成 LF,cmd 逐行吞行首字符整批报废)——改它只用 Python 二进制读写或 GBK 感知编辑器。
- **重新构建前先杀掉运行中的 `QuotaBar.exe`**(单实例持有 exe,直接 LNK1104)。
- 代码目标是 **/W4 零警告**;新代码同样要零警告(依赖 `Log::WriteF` 模板定义在 `log.hpp`,改 log.cpp 时不要移回)。

## 架构

单进程、单 UI 消息循环;所有 GUI 操作在 UI 线程,网络回调只做有界工作后 PostMessage。数据流:

```
app(消息中枢/AppState) ──┐
scheduler(定时器/退避/频率限制) ──→ http_client(WinHTTP 异步) ──→ providers(解析) ──→ state(快照/显示模型) ──→ overlay(透明文字) + tray_ui(托盘/详情/设置)
```

模块约定(按设计文档 §5 分表;新代码并入对应模块,不新建运行时依赖):

| 模块 | 职责要点 |
| --- | --- |
| `app` | 实现 `ISchedulerHost`;隐藏窗口**必须**是顶层窗口(接收 Shell 广播;HWND_MESSAGE 不接收);单实例用命名互斥 + **EnumWindows 按类名查找**隐藏窗(`FindWindow` 对隐藏窗不可靠);系统事件(锁屏/休眠/全屏 WinEvent/网络 NetlistMgr/Explorer 重启/主题/DPI)在此订阅 |
| `scheduler` | 所有请求唯一入口；**按账户调度**（`std::vector<ProviderSched>` 经 `SyncAccounts` 与 `cfg.accounts` 惰性同步；请求/响应按 `account_id` 路由、按 `provider_id`（平台类型）选解析器）；合并单定时器;退避 5/10/20/30min + 10% 抖动;429 遵守 Retry-After;401/403/404 停机等待用户;停用(`enabled=false`)账户不调度;暂停/锁屏不查询;恢复后 5–15s 随机延迟补查。**本机 hub 类（antigravity,`ProviderOps::local_hub`）永不停机**:缺源（hub 未运行）/401（hub 重启换了 CSRF 令牌）/连接被拒/坏响应一律 `kHubReprobeSec=60s` 短周期重探,不走通用退避（hub 随 Antigravity 启停,5→30 分钟退避会让刚启动它的用户等半小时）;缺源时快照转 Unconfigured（UI 文案「未运行」,暂停态保留「已暂停」）且**只在状态切换时记一条日志**（每次重探都记就是刷屏）;hub 随时可能出现,所以这类账户恒视为已配置、始终进调度;手动刷新在缺源时返回 `ManualKick::SourceOffline`（回显「Antigravity 未运行」,不谎报已开始刷新） |
| `http_client` | 单请求总期限 10s(定时器强制);响应 ≤64 KiB;禁自动重定向;证书校验开启;取消后必须等待 HandleClosing 清理。**WinHTTP 异步读必须用标准模式**:Query 只做推动、读只在 DATA_AVAILABLE 事件里做一次、结束只在 avail==0 单点判定——READ_COMPLETE 内"Query+立即 Read"双路驱动会交错致大响应截断(实测 codex cl=1226 只收 1016-1026B)。请求头带 `Accept-Encoding: identity`(暂不支持自动解压)。2026-11-27 起支持 **POST+JSON 体**（`ProviderRequestSpec::method/body`:非空体随请求发送并补 `Content-Type: application/json`,体副本存进 Ctx 以活过异步发送期）与**认证头名可配置**（`auth_header`,默认 Authorization;antigravity 用 `x-codeium-csrf-token`;`auth_header` 值为空则整个认证头不发）。**回环地址 WinHTTP 天生不经代理**——会话级指向假代理的对照实验证实（`experiments/ag_probe/winhttp_loopback_proxy_test.ps1`）,hub 请求无需按请求覆盖代理（AUTOMATIC_PROXY 会话上 `WINHTTP_OPTION_PROXY` 本也不可设,12011） |
| `providers` | 五个静态适配器(Kimi/DeepSeek/**Codex**/**GLM**/**Antigravity**);**`ProviderOps` 分派表(2026-09-18)是"类型→平台行为"唯一真值**(id/契约/徽章/控制台/默认别名/用Key/build/parse 八元组一行一平台;`ProviderOpsOf`按类型、`ProviderOpsById`按id 查表;scheduler 组请求/选解析器/写契约与 providers 五个辅助函数全部查表,**新增 provider=表加一行+各层一处 case**,勿再恢复 if-chain);`kKimiContract = kimi-usages@86f1364`(锁定 kimi-cli 提交),字段漂移时升级契约版本并重新验证。**Kimi 的 limit/used/remaining 是 JSON 字符串数字,必须走 CountField 兼容**(IsNumber 会静默丢窗);`kCodexContract = codex-wham@2026-09-05`(实测锁定):wham/usage 仅百分比+重置时间(无绝对数),窗口标签"5h余/7d余"(用户拍板:7d 与 5h 同构);`kGlmContract = glm-quota-limit@2026-09-12`(探针真机锁定,样本 `experiments/glm_probe/glm_response.json`):智谱 `open.bigmodel.cn/api/monitor/usage/quota/limit`,**Authorization 头不带 Bearer 前缀**(`ProviderRequestSpec::auth_bearer=false` 调度器据此组头),信封 `{code,msg,data,success}`,limits[] 按 `type` 分派——**CREDIT_LIMIT(现网形态)**=绝对点数 usage/currentValue/remaining + `unit`/`number` 窗口时长(实测 (3,5)=5h、(6,1)=周,重置时刻互证;未知组合→"未知周期"仅进详情)+毫秒 `nextResetTime`(ExtractResetTime 自动降为秒);TOKENS_LIMIT/TIME_LIMIT(旧社区形态)保留兼容;percentage 字段是**已用**%但不单独采用,百分比一律走 ComputePercent 计数口径。`kAntigravityContract = antigravity-quota-summary@2026-11-27`(真机锁定,样本 `experiments/ag_probe/ag_response.json`):**查的是本机 Antigravity hub 的 Connect RPC**——`POST http://127.0.0.1:<httpPort>/exa.language_server_pb.LanguageServerService/RetrieveUserQuotaSummary`,头 `x-codeium-csrf-token`,体 `{}`;**不是 Google 端点**,OAuth 由 hub 代劳,QuotaBar 全程不接触令牌。基址端口随 hub 重启变化:经 `build_request(local_base)` 的入参现场传入（宿主 `ISchedulerHost::GetEndpointBase`;其余 provider 忽略该参数）。响应 `{response:{groups[{buckets[{bucketId,window,remainingFraction,resetTime?}]}]}}` 按 bucketId 分派——gemini-5h→`ag_5h`、gemini-weekly→`ag_week`（Gemini 池:常驻条/圆环/卡片主窗）、3p-5h→`ag_3p_5h`、3p-weekly→`ag_3p_week`（Claude&GPT 池:仅详情注解）,其余桶→`ag_other_N`（按 window 归周期,不猜语义,注解里列出）。三点与直觉相反:**组内 weekly 排在 5h 之前,不可按数组序**;没动过的桶（剩余 1.0）**没有 resetTime**;额度耗尽时 remainingFraction 是 proto3 零值被**整个字段省略**（以 disabled 或未过期 resetTime 佐证为 0%,二者皆无=无数据跳过）。信封兼容顶层 groups / 顶层 buckets。另有两个判定函数也在此层:`ProviderSingleCard`（无 Key 类凭证源全机唯一,只能一张卡）与 `ProviderAlertCapable`（比例窗口类 Kimi/GLM/Antigravity 参与阈值预警;codex 历史上不参与,勿顺手"补全"）——卡片阈值行、条上低额变色、阈值通知、添加面板共用 |
| `state` | 快照 + 新鲜度 + 显示模型;**账户列表输入**（2026-09-14 起）:`AccountView{cfg,snap}` 向量进 `BuildDisplayModel/BuildEmbedItems/BuildRingItems/BuildDetailCards`,每账户一行/一环/一卡,**首段恒为「别名 」前缀段**（嵌入条按此拆名称行/值行）;调用方决定集合——常驻条/圆环/托盘提示传 `enabled` 账户,详情页传 `card_visible` 账户;别名:显示层按 `kimi_weekly` 精确 key 和 `kimi_rolling_...s` 前缀匹配滚动窗口;详情各窗口行尾用 `reset_part` 显示"到期：还有 X(绝对时间)"倒计时,粒度按窗口时长(≤24h→分钟,否则小时;`FormatRemainingZh`);**圆环模式**(Rings,2026-09-12):`BuildRingItems` 产环条目(环弧=5h 剩余比例/环心=数值/徽章=别名首字符+类型品牌色),配色自包含固定亮色(overlay 垫半透明深色圆盘,不随壁纸/主题反转),定位沿用吸附锚定(见 overlay 2026-09-21);环心文字必须 MeasureString 后按点绘制——矩形排布在宽度临界时丢末字符("¥22"→"¥2" 实测);**横幅直绘屏幕 DC 会闪烁**:30fps 下"填背景→画粒子→画标题"分步执行,DWM 中途合成采样到半成品帧,表现为标题区持续闪烁——必须双缓冲(整帧绘入内存位图,一次 BitBlt 上屏,详情窗 PaintDetail 实测修复);**被遮挡窗口显示后必须补一次 InvalidateRect**(遮挡期间首帧会被 DWM 丢弃,实测空窗);`BuildDetailText` 已随「复制全部」删除（2026-09-14）,文本断言基线改用 `BuildDetailCards` |
| `taskbar_slot` | 嵌入槽(display.mode=Embed):顶层 WS_POPUP 创建后 SetParent;D2D+DComposition 合成交换链;嵌入布局默认两行(名称行大字/值行 0.8×,列宽=max(名,值),列距 kColGapDip;`embed_show_names=false` 时单行只渲染值行,窗口高减半);锚定链 ①贴天气估算右缘(kWeatherWidthDip=115,用户实机 25℃ 组件实测 98DIP + 变长缓冲)②簇右缘 ③托盘;**软界自适应**:内容超常规 30DIP 留白时放宽到 4DIP(kMinPadDip);`slot_offset_x_dip` 用户水平偏移(±200 钳制,显示设置页滑杆实时微调)。**锚定/偏移/软界的次序(2026-09-12 踩坑)**:可用宽必须按"偏移后的实际 x0"到两道界的距离判定——先应用 slot_offset 再判常规界/软界;若按旧写法先判宽再算 `avail=right_lim(常规界)-x0`,用户一旦设过偏移(实测 32DIP)软界放宽就被整行吞掉,可用宽虚减 26DIP,四平台内容超界 → 列挤压 → `TruncateSlotSegs` 从尾部丢段(表现:"周"额度消失)。锚定链封装为两遍执行:字号收缩后内容变窄,第二遍复评常能进软界。仍放不下才列挤压+尾段截断兜底 |
| `theme` | 现代深色主题(2026-09-12 重构为设计 token + 绘制原语):`ApplyWindowChrome`=Win11 圆角+深色标题栏/边框(DWM 属性,旧系统静默忽略);`ModernizeDialog`=对话框统一深色(EDIT/复选/单选走 DarkMode_Explorer 尽力而为)+**按钮手动子类化自绘**(SetProp 存原过程,刻意不用 SetWindowSubclass 以免依赖 comctl32 v6 清单);**子类化后按下/抬起不得转发原过程**(实测:转发 WM_LBUTTONDOWN 后原生按钮的捕获逻辑立刻发 WM_CAPTURECHANGED 重置自绘 pressed 标志,抬起被判"未按下",点击静默丢失——设置保存点不动;修法=按下/抬起 return 0 完全自管,其余消息照常转发);`HandleDialogMessage`=对话框 WM_ERASEBKGND/CTLCOLOR* 统一深色,各对话框过程入口调用;公共像素原语 `DrawTextPx/MeasureTextPx/FillRoundRectPx/StrokeRoundRectPx/FillEllipsePx/FillGradientPx/DrawRingPx/DrawBadgePx`(详情卡片/按钮/圆环共用,token 调色板在 theme.hpp 单一来源;**品牌色 K/D/C/G 已迁 providers.hpp ProviderOps.brand_color**——2026-09-18 起 theme 只留随主题变的 token);**按钮文字必须用像素字号(UnitPixel)**——UnitPoint 依赖 Graphics DPI,且 `GetDpiForWindow` 在个别路径返回 0 → 字号归零文字静默消失(实测);`AnimationsEnabled()`(SPI_GETCLIENTAREAANIMATION)为假时跳过一切 AnimateWindow;`EnableDarkSystemChrome()`(uxtheme 序数 135/136)启动调用一次让弹出菜单跟随深色;详情窗/对话框 AnimateWindow 淡入淡出 |
| `ui_layout` | tray_ui 的纯布局/命中测试核心(2026-09-18 抽出,God Module 拆分第一步):`SliderValueFrom`(滑杆取值,轨道内缩 7DIP×dpi)/`ComputeDragTarget`(拖拽落位,其余卡片中线判插入槽,同行水平中线分左右,x=中线判**后**)/`HitTestAt`(命中核心:先注册者胜;与内容区有交集的命中须同时在视口或豁免区,滚出视口不可点;chrome 恒可点)。tray_ui 侧同名薄包装喂 g_layout_dpi/g_qlayout/g_hits,调用点零改动;**改判定规则先在 tests.cpp TestUiLayout 补用例**(历史滑杆落盘/kWLink0 判界/滚动误触事故全部源于这三个判定) |
| `storage` | `%LOCALAPPDATA%\QuotaBar\`:config.json(无密钥;v2=`accounts[]` 数组,v1 平铺字段读入时自动迁移、落盘才写 v2)/ secrets.dat(DPAPI;**v2=按 accounts 序逐段** [4 字节长度+blob],v1 三段读取时映射到同类型账户;codex 占位空段)/ cache.json(v2=按 account_id 的快照数组,cred_rev 校验,旧格式废弃重查)/ history.json(用量历史滚动采样,v1=百分比样本 [t,pct] + 余额样本 [t,"raw"],无凭证;codex 以身份锚入盘:可选字段 ik=ChatGPT Account Id 的 FNV-1a 哈希(非原始 Id),换账户整段重记);写入=临时文件+原子替换;自启动=HKCU Run |
| `notify` | Toast 提醒通道(2026-09-15 B 迭代):C++/WinRT(`Windows.UI.Notifications`),**免打包应用必须有带 AUMID 属性的开始菜单捷径**——`EnsureToastShortcut` 幂等创建(`%APPDATA%\...\Programs\QuotaBar.lnk`,System.AppUserModel.ID 手写 PROPERTYKEY 避免 propsys.lib);`ShowToast` 任何失败(通知关闭/Focus Assist/COM 错误)静默返回 false,由 `App::Notify_` 回落托盘气球;winrt 头文件只在 notify.cpp 单 TU 展开(#pragma warning 包裹),链接 windowsapp.lib |

### 关键不变量(改动时不得破坏)

- **账户模型(2026-09-14)**:`cfg.accounts[]` 是唯一账户真值来源的镜像——运行时真值在 `App::accounts_`(`AccountState{cfg,key,snap}`),改账户后必须 `SyncCfgAccounts_()` 回写 `cfg.accounts` 再落盘;`CommitConfig(cfg)` 会用 `accounts_` 重同步,显示页草稿里的 accounts 副本永不生效。
- **secrets 位置式存储 ↔ 账户顺序强耦合(2026-09-15 拖拽排序实测踩坑)**:secrets.dat v2 按 accounts **序**逐段存取,任何改变账户顺序的操作必须**同一次提交内**按 id 保住 key 关联并 `SaveSecretsNow()` 重写(`App::MoveAccountOrder` 的做法:先快照 id→key,重排后按 id 回填再重写),否则重启后密钥按新序错位映射(实测 GLM 拿到 Kimi 的 Key、Kimi 拿到空段显示"未配置")。改配置顺序的脚本/功能务必核对这一点。
- **凭证版本隔离**:每次换 Key/删 Key 递增该账户 `cred_rev`;旧版本的 HTTP 响应和磁盘缓存一律丢弃(跨账户隔离)。**历史序列同口径**(2026-09-15):UpdateAccount 换 Key 与 RemoveAccount 都会把该账户的 `AccountHistory` 整段删除(换 Key 后旧曲线不得混入新速率/今日消耗)。`GetKey` 为空 → 快照 `credential_revision=0` → 显示"待配置"。codex 账户 `cred_rev` 恒 1(令牌只读 `~/.codex/auth.json`,无换 Key 语义;其历史隔离改由身份锚 `identity_key` 负责,见用量历史条目);账户删除后的迟到响应按 account_id 找不到调度表/快照直接丢弃。
- **时钟**:调度与数据年龄用**单调钟**(NowMono);持久化/服务端时间用 UTC;**不**把"重置时间到达"当成本地可恢复 100%,只能触发重新查询。
- **金额**只走 `Decimal` 定点;禁止二进制浮点加减;`FormatMoney` 处理 `<0.01` 展示;余量 0–1% 用 `tiny_positive`(`<1%`),不许显示成 0%。(例外:历史 spark/速率拟合用 double,仅绘图不参与金额业务)
- **DPI**:进程为 PerMonitorV2 感知,系统不自动缩放 —— `tray_ui` 的对话框用 `g_layout_dpi`(96 DIP 设计值 × LayoutScale),`overlay` 用自己的 `Scaled()`;对话框/详情窗创建后必须先 `SetLayoutDpi` 再创建子控件,否则高缩放下控件挤压重叠(实测过的崩溃点)。
- **日志脱敏**:不写 Key/Token/Authorization/完整响应;默认只记状态转移与错误。
- **本机凭证类（无 API Key:codex / antigravity,`ProviderOps::uses_api_key=false`）**:凭证每次请求**现场读本机凭证源**（`~/.codex/auth.json` / `~/.gemini/<product>/daemon/hub_*.json`）,不缓存;`cred_rev` 恒 1;不进 secrets.dat（占位空段保持对齐）;**不进磁盘 cache**（本机凭证源可能已换账户,旧快照不得冒充）。`GetKey` 为空:codex = 未登录,antigravity = hub 未运行——后者仍保持调度（`local_hub`）,由 `StartFetch` 的缺源分支短周期重探。UI 里所有"是不是 codex"的判断 2026-11-27 起一律改查 `ProviderUsesApiKey`/`ProviderSingleCard`/`ProviderAlertCapable`,**不要再写 `type == AccountType::Codex` 特判**（现存的那几处是 Codex 专属语义:auth.json 令牌与 ChatGPT 账户身份锚）。

## 配置映射与界面行为备忘(跨文件才知道)

- **config.json 键名 ≠ C++ 字段名**:位置类 JSON 键无 `_dip` 后缀(`free_pos_x`→`free_pos_x_dip`、`bar_snap_x`→`bar_snap_x_dip`,单位 DIP;旧 `dock_offset_x/y` 读取时迁移到 `bar_snap_x/y` 后不再落盘);读写只经 `storage.cpp` 的 `ConfigIo::Dump/Load`,加设置字段必须两处同步。**config v2 账户键**:`accounts[]`(id/type/alias/enabled/card_visible/cred_rev)+`account_seq`(id 自增,保证不复用;codex 固定 id "codex");v1 的 `show_*`→`card_visible`、`cred_rev_*`→各账户(读入时迁移,落盘才写 v2)。
- **secrets.dat v2**:magic `QBARSEC2` + 按 `accounts` 序逐段 [4 字节长度+DPAPI blob];codex 占位空段保持对齐;账户增删/换 Key 整体重写(`App::SaveSecretsNow`);v1(`QBARSEC1` 固定 kimi/deepseek/glm 三段)读取时映射到同类型首个账户。缓存 cache.json 同理按 `account_id` 存。
- **额度页动态 id 空间**:账户卡片控件 id = `kWAccBase(2000) + 可见序*kAccStride(20) + 子id`(启用 0/显隐 1/设置 2/保存 3/删除 4/Key eye·copy·clear 5-7/帮助 8/codex 重读 9/ⓘ 10;隐藏行「显示」用子 id 12+隐藏序,与卡片子 id 错开防碰撞);`g_links` 链接 id=kWLink0+i 判界必须带上界,页面固定控件 id 须落在 g_links 数量之外(2000 远偏移即为此)。
- **额度页滚动**:内容(添加面板/隐藏区/卡片墙)整体随 `g_q.scroll` 平移,页头按钮固定;`ComputeQuota` 统一钳制并应用偏移,`WM_MOUSEWHEEL` 调整后必须 `Relayout()`(原生 EDIT 随滚动重摆,否则输入框与自绘底错位)。**滚动视口裁剪(2026-09-19 修"下滚卡片盖住标题栏/最小化按钮")**:PaintQuota 的滚动段整体包 `SaveDC+IntersectClipRect`,视口=`{content.left, content.top+LS(kQuotaHeadDip), content.right, content.bottom}`(52 DIP=页头高,与 quota_layout 的 after_head 同源);命中注册经 `g_hit_clip` 裁到可见部分(`ui::ClipHitRect`,完全滚出不注册)——不留"看不见却可点"的幽灵命中带(否则标题栏拖拽区被滚上去的卡片命中吞掉);PaintDetail 额度页 `g_hit_vp` 同矩形、`g_hit_extra`=页头带(隐藏 chip/刷新/添加在视口上方靠它保持可点);Relayout 的 qvp 同矩形隐藏滚出视口的 EDIT;绘制/命中/EDIT 三处矩形必须同步。拖拽插入线 y 钳到视口顶(非 content.top)保持可见。**「刷新」按钮(2026-09-16 从标题栏移来)**=页头「已隐藏」chip 与「添加卡片」之间的图标按钮,`kWRefresh` 命中只由 `PaintQuota` 注册(显示设置页自然没有);标题栏只剩最小化/最大化/关闭。显示设置页滚动用独立的 `g_dscroll`,滚轮处理按 `self->page()` 分流。
- **GDI+ DrawString 不裁文本**:GenericTypographic 布局框宽度超限直接画出去(band 元信息与右邻链接重叠,实测)——长文本入框前自行逐字裁剪加 "…";同样地,`DrawBtn` 文本超按钮宽自动省略号,按钮宽度须按最大文案预留(设置按钮 64 DIP 在 200% 缩放截成"设...",实机改 84)。
- **低额度提醒带滞回**:只在跨阈值边沿提醒一次(恢复再跌破才再次触发);滞回状态**逐账户**存 `App::notify_below_`。百分比窗口(Kimi/GLM 型)共用 `kimi_threshold_pct` 一个阈值,金额账户(DeepSeek 型)用 `deepseek_threshold_cny`。
- **JSON 裸整数映射**(勿凭 C++ 枚举猜 config):`display.mode` 0=Docked(**已并入 Free:2026-09-21 起读取时迁移为 Free+`bar_snap=true`,UI 不再产生**) 1=Free(浮条) 2=TrayOnly 3=Embed(任务栏内嵌) 4=Rings(圆环);`bar_snap` 浮条吸附任务栏上缘右端(拖近自动吸附,吸附偏移 `bar_snap_x/y`;Rings 恒吸附共用 `bar_snap_x`);`text_color_mode` 0=自动(按 `AppsUseLightTheme`;深色主题→黑字,黑壁纸上近乎不可见,排查显示问题先切 1=白)1=白 2=黑;`low_quota_color` 低额变色开关(默认 true;关=值段恒基色,「需更新密钥」等状态告警色不受影响);`brand_color_names` 账户名品牌色(默认 false);`embed_show_names`/`embed_show_windows` 嵌入条名称行/窗口标签(默认 true);`slot_offset_x_dip` 嵌入条水平偏移(±200,正=右);`accounts[].type` 0=Kimi 1=Codex 2=Glm 3=Deepseek 4=Antigravity（数值是落盘契约,**只在末尾追加**;`ConfigIo::Load` 的上界跟 `kAccountTypeCount` 走——新增类型忘了放宽会被当未知条目静默丢弃）;**`accounts[].alert_pct` 0=继承全局 1=关闭 5..95=自定义阈值%**(2026-09-15 B 迭代);`notify.reset_reminder` 重置恢复提醒开关(默认 true)。
- **清除入口**:显示设置页「清除数据」chip 清 cache/secrets/config/history(账户列表复位为 4 张默认卡);普通退出保留配置。首次运行(无 `first_run_done`)自动打开主窗口并展开「添加卡片」面板。

## 已知状态(避免重复踩坑)

- **浮条位置(2026-09-21 重做)**:吸附态位置由 `overlay` 的 `TaskbarRect()`(兼容模块,识别失败自动降级)锚定;浮条 **Ctrl+悬停可直接拖动**(轮询计时器临时取消穿透),松手按 `ResolveBarDrop` 落位——近任务栏上缘自动吸附、否则存自由位置,**即拖即存**(旧的「进入调整/双击保存/ESC 取消」仪式流已删,EnterAdjust/ExitAdjust/CmdEnterAdjust 均已移除)。落位回调里 `SyncDraftDisplay` 同步显示页草稿,防「保存设置」用旧快照拽回位置。
- Explore 重启:监听 `TaskbarCreated` 广播重建托盘图标并 `Overlay::OnTaskbarCreated()`;**不重置查询退避**。
- **月池仍无来源(已核查 2026-09-06)**:真实 `/usages` 响应含顶层 `totalQuota` 键但本账户返回**空对象**;第三方工具(OpenTokenUsage/kimi-code-usage/pi-usage-panel)均只用同一端点;CodexBar 文档提示会员月池在 www.kimi.com 网页(cookie 认证,与 API Key 体系不同)。**维持**"会员月额度:未获取",不得编造接口或估算。
- **Codex(ChatGPT 订阅)provider 事实**:令牌**只读** `%USERPROFILE%\.codex\auth.json`(仅 auth_mode=chatgpt;QuotaBar 不复制不落盘,过期由 Codex CLI 刷新);附加头 `ChatGPT-Account-Id` 经 `GetAccountId` 注入;凭证版本固定 1(无换 Key 语义);**不进磁盘 cache**(历史已以身份锚入盘,见用量历史条目);401/403 → "Codex 需更新令牌"。
- - **PowerShell 自动化脚本编码坑(2026-09-21 实测)**:带中文注释的 `.ps1` 以无 BOM UTF-8 存盘时,PowerShell 5.1 按 GBK 逐字节误读——多字节序列把行尾 LF 吞进伪字符,**下一行代码被并进注释行静默失效**(症状:Add-Type 行"消失"、类型找不到)。仓库内 UI 自动化脚本一律纯 ASCII 或带 BOM。
**rc.exe 编码坑**:`assets/quota_bar.rc` 的版本资源 `FileDescription` 含中文会乱码(rc 按 ANSI 解释源文件;设置"启动应用"列表读此字段)。该字段保持 **ASCII**;如需中文,须把 .rc 存为 GBK。
- **嵌入条微调**:在**主窗口显示设置页的「位置微调」卡**(±4 DIP 实时应用且点击即落盘 CommitConfig,非模态无取消;Alt+←/→ 微移仅 Embed 模式生效)。浮条用 X/Y 滑杆(`CommitFreePos`)与吸附开关/吸附偏移(`CommitSnap`);吸附偏移 `bar_snap_x_dip` 为浮条吸附态与 Rings 共用(±200 钳制)。
- **性能门槛(§12)部分实测(2026-09-06,20 分钟采样)**:working set 稳定 68.4 MB/私有 66.9 MB,无增长;CPU 20 分钟累计 1.19s(≈0.1%);exe 587 KiB。**8 小时长稳仍未测**。
- **P0 真实账户联调已完成(2026-09-05/06)**:Kimi 字符串数字、DeepSeek is_available 拆账、Codex wham 百分比口径均已实机核对,结论回写 README;契约版本 `kKimiContract/kDeepSeekContract/kCodexContract` 已锁定。
- **GLM provider(2026-09-12 接入并真机联调)**:端点 `open.bigmodel.cn/api/monitor/usage/quota/limit`。三点与"常识"不同须记住:①Authorization 头是裸 Key 无 Bearer 前缀(`auth_bearer=false`);②**首版按社区样本(TOKENS_LIMIT/TIME_LIMIT)实现,真机返回的却是 CREDIT_LIMIT**——社区工具的样本是旧 API 形态,字段漂移后解析出 0 窗口报"结构不兼容",靠协议错误日志的 8 字节头 hex + `experiments/glm_probe/probe_usage.ps1`(DPAPI 读 Key 发同款请求导出响应)拿到真机样本修正;③窗口时长看 `unit`+`number` 字段而非数组序(实测 (3,5)=5h、(6,1)=周,`nextResetTime` 毫秒互证)。国际版 Z.AI(api.z.ai)同路径未接入,当前仅国内 bigmodel.cn。**联调协议错误排查路径**:日志 hdr hex → glm_probe 导出真实响应 → 比对解析器。
- **Antigravity provider(2026-11-27 接入并真机联调)**:额度查的是**本机 Antigravity hub**,不是 Google（契约见 providers 行;探针与隔离联调工具在 `experiments/ag_probe/`）。几条踩过或差点踩的坑:①**本机是 Electron 新架构**（`Antigravity.exe` + `language_server.exe --override_ide_version 2.35.2`,数据在 `~/.gemini/`）,社区文章里 VS Code fork 时代的 `~/.codeium/antigravity/auth_token` 根本不存在,磁盘上也没有可直接用的 OAuth 令牌——契约必须以本机为准（`~/.gemini/config/plugins/usage-monitor/` 里的官方插件源码 `ls-discovery.mjs`/`ls-rpc.mjs`/`quota-pools.mjs` 是现成的参照实现）;②hub 描述文件**退出不清理**,本机残留多份,`FindAntigravityHub` 按 pid 存活筛选再取 mtime 最新（pid 缺失时放行,错文件至多换来一次连接被拒）;③hub 的 401 不是"凭证失效"而是"刚重启换了令牌",若按通用语义停机等用户,Antigravity 每次重启都得手动救——所以 local_hub 类永不停机;④它**不在默认账户集**（需本机装着并运行 Antigravity,对不用的用户只是一张「未运行」噪声卡）,经「添加卡片」按需创建;⑤Antigravity 退出后,嵌入条上的数值要到**下一次定时刷新**才变「未运行」（轮询语义,与其它数据源一致）,不是实时的;⑥社区另报道的直连路线（`daily-cloudcode-pa.googleapis.com`,要自己拿 OAuth 令牌且 User-Agent 版本段决定返回哪些桶）未采用。**联调不要碰真实数据目录**:用 `experiments/ag_probe/qb_sandbox.py` 起隔离 profile（单实例互斥量按会话计,得先退出真实实例,联调完**立刻把真实实例拉回来**——它跑的就是 `build\QuotaBar.exe`）;其它联调手段的坑（taskkill 软关闭不可靠、锁屏下合成点击无效且与版本无关等）见该目录 README。
- **ULW 顶层渲染在本机验证可用(2026-09-05 复测,产物 `experiments/ulw_probe/`)**:早先「ULW 本机整体失效」结论**不可复现**。实测矩阵(顶层窗口,PMv2 @200%,GDI+/ULW/色键/普通 GDI 四路径对照 + 用户肉眼 + 截屏三方一致):① 顶层 `UpdateLayeredWindow` 逐像素 alpha **完全正常**——位置/尺寸/颜色/alpha 均正确,含与 overlay 同款管线(GDI+ PARGB 包裹 DIB + DrawString 文字)与「先 Paint 后 Show」次序;② 普通 GDI 与色键顶层窗口同样正常,**但隐藏窗口 DC 的预绘制在首次 ShowWindow 时会被丢弃**(实测内容丢失、合成为空窗透出壁纸;显示后重绘即恢复)——DWM 时代绘制须走 WM_PAINT/显示后重绘,ULW 自带 surface 不受此限;③ **实机 docked 验收已过**(同日,test_mode 合成数据):overlay 在任务栏右端上方正常显示两行文字(用户肉眼确认),位置/尺寸随内容右对齐正确。勿再引用旧「ULW 整体失效」说。
- **深色主题下 overlay 黑字黑底注意**:`text_color_mode=0`(自动)按 `AppsUseLightTheme` 判定字色;本机深色主题 → 黑字,黑壁纸/任务栏上方几乎不可见(曾误判为"没渲染")。验收/排查时切 `text_color_mode=1`(白字)可排除此干扰。
- **父化到 Shell_TrayWnd 的渲染本机不稳定(2026-09-05 复测)**:父化 ULW 子窗口不可见(与早先一致),但**父化色键子窗口复测当日也不合成**(截屏无内容,与早先「色键通过」记录矛盾;当日 crop.png 同样无内容,旧结论疑为目检误判或环境瞬态)。任务栏内嵌方向不变:**勿依赖父化分层窗口,生产渲染仍走 D2D+DComposition**(TrafficMonitor 同款,未验证),落地前须在实机肉眼验收。
- **任务栏内嵌(SetParent 到 Shell_TrayWnd)原型实验(2026-09-05,产物 `experiments/taskbar_probe/`)**:① 宿主是真实可寄宿的 `Shell_TrayWnd`,`TrayNotifyWnd` 可作锚点(实时轮询,托盘宽度变化自动微调);② 点击穿透正常;③ **宿主销毁会连带销毁子窗口**(Windows 父子规则),Explorer 重启恢复必须"整窗重建"(探针已实现并验证 detach→rebuild→attach);④ 坐标坑:父化窗口的 `SetWindowPos`/ULW 按**父客户区**坐标、`GetWindowRect` 按屏幕,混用会每 tick 漂移一个宿主原点(实测 +1984px/tick);⑤ 已知限制仍成立:Win11 左对齐强制靠右、副屏无 TrayNotifyWnd 需固定宽度估算。渲染结论见上条(不稳定,勿依赖)。
- **嵌入模式幽灵浮条(已修 2026-09-16,用户实测"设完嵌入后过一会靠近任务栏的额度条同时出来")**:`Overlay::ApplyPosition` 旧结尾无条件 `SWP_SHOWWINDOW`,Embed 模式下三条事件路径仍会路过它——`TaskbarCreated`→`OnTaskbarCreated`、`WM_DISPLAYCHANGE`→`OnDisplayChange`、overlay 自身 `WM_DPICHANGED`,全部汇入 `Relayout()`,而 `Relayout` 旧守卫只排除 TrayOnly。更致命的是这次 `SetWindowPos` 绕过状态机把窗口点亮而 `visible_` 仍为 `false`,`Hide()` 的旧守卫 `if (hwnd_ && visible_)` 从此永不成立——**每次 RefreshDisplay 的 Hide 全部空转,幽灵条永久无法隐藏**(实机抓到 QuotaBarOverlayWnd 可见于任务栏正上方,与嵌入槽并存)。修复三件套:①overlay 新增 `suppressed_`(`SetSuppressed`,app.cpp `RefreshDisplay` 按分支置位),`Show/Relayout/OnFullscreenCheck/Update/UpdateRings/EnterAdjust/TickRingAnimation` 全部守卫;②`ApplyPosition` 显示标志按 `visible_` 二选一(SHOW/HIDE),隐藏态重定位不得点亮窗口;③`Hide()` 改以 `IsWindowVisible` 实际窗口态为准(标志失配自愈)。验证:实机注入 WM_DISPLAYCHANGE / 真投递 TaskbarCreated(EnumWindows 取隐藏窗句柄,**FindWindow 对该隐藏窗返回 0**,见单实例条目)后 overlay 保持隐藏、槽位正常重建。教训:**任何绕过 Show/Hide 状态机直接动窗口可见性的代码,都会让布尔标志失配并使 Hide 永久失效**。
- **截屏验证的坑**:验证颜色须用 COLORREF(0x00BBGGRR)语义,勿手写 0xRRGGBB 再走 GetRValue/GetGValue(逐字节解析会 R/B 互换,探针曾自误);本机截图与肉眼一致(用户实机可看可截),但注意 PS CopyFromScreen 坐标按进程 DPI 感知解释(PMv2 物理像素 / SystemAware 逻辑像素),两套混用会拍偏。**PS DllImport 字符串参数默认 ANSI marshalling**——`FindWindowW("Shell_TrayWnd")` 不带 `CharSet=Unicode` 时按 ANSI 传 UTF-16 参数必失败返回 0,消息"投递成功"实则发到 hwnd=0 线程队列(静默无效),验证窗口消息链路时先核对句柄非 0。
- **开始菜单打开 → 嵌入条跳右侧(已修 2026-09-06,探针产物 `experiments/slot_probe/`)**:Win11 在 Start 菜单等 flyout 打开期间把左簇(含 `Start` HWND)从 `Shell_TrayWnd` 子窗口树**摘走换挂**,`EnumChildWindows` 枚举不到 → `ClusterLeft` 现场失败 → 锚定链回退 `ClusterRight`(条跳到任务栏右侧,关闭菜单后自动回位,全程无降级日志)。打开态下子窗口链 churn:`EnumChildWindows` 与 `FindWindowExW` 视图不一致(前者漏列仍存活的子窗,**勿用其结果断言窗口已销毁**);`TrayNotifyWnd` 靠 `tray_hwnd_` 缓存 + `IsWindow` 天然粘住。修复 = `ClusterLeft` 粘性锚(`start_hwnd_`+`start_cached_` 两级缓存:句柄活用实时矩形保鲜、句柄亡退缓存矩形,均按宿主几何+垂直重叠校验防陈旧;`TryAttachHost` 换宿主与 `OnTaskbarCreated` 重建时清缓存)。实机复测:打开态(缩减树签名)下嵌入条矩形保持 121,1000 不动,关闭态同值。
- **主窗口切页残影(已修 2026-09-13)**:额度/设置主窗口**不得加 WS_CLIPCHILDREN**——父窗口整帧重绘被裁掉子控件矩形,而 GROUPBOX/STATIC 不画自身背景,切页后这些区域直接露出上一页旧帧(实测「额度详情→调整位置」卡片残影);配合 `RebuildPage` 末尾 `InvalidateRect(detail_, nullptr, TRUE)` 强制带背景擦除。整帧走内存位图双缓冲,去掉裁剪不引入闪烁。
- **导航/链接交互范式(2026-09-13)**:左侧导航与页脚链接为「**down 高亮 + SetCapture、up 命中才提交、拖离取消拖回恢复**」(Apple tap 范式);**up 的激活只按命中测试、不依赖 down 状态**——shot_pages.ps1 等自动化仅 Post WM_LBUTTONUP 仍可切页,勿改成 must-have-pressed。链接悬停提亮色 RGB(150,176,255) 与按钮焦点环同源。**导航 hover 必须注册 TME_LEAVE**(全局 `g_detail_tracked` 门控),否则鼠标快速移出后高亮粘在末项;按压/悬停索引在 `RebuildPage`/`OnDetailClosed`/WM_CAPTURECHANGED 三处复位(矩形随页面重建,旧索引会高亮错位)。
- **主窗口关闭淡出(2026-09-13)**:`CloseDetail` = `AnimateWindow(AW_BLEND|AW_HIDE, 140)` 后 `DestroyWindow`,与 ShowDetail 的 140ms 淡入同径(出入场对称);**标题栏 X 走 WM_SYSCOMMAND SC_CLOSE 分发到 CloseDetail,外部 `WM_CLOSE`(任务栏右键"关闭窗口")2026-09-16 起也转发 CloseDetail 同径**;Esc(IsDialogMessage→IDCANCEL)同路径;`theme::AnimationsEnabled()` 为假直接销毁。直接 Post WM_CLOSE(自动化脚本)已同径,有 "close detail" 日志。
- **托盘左键 toggle 语义(2026-09-16 重写,修"第一次点托盘打不开"实测 bug;2026-09-16 深夜补最小化还原洞)**:旧实现 `detail_ && IsWindowVisible(detail_) → CloseDetail` 有致命歧义——窗口被遮挡或最小化时 `IsWindowVisible` 仍为 TRUE,用户第一次点托盘实际执行的是**销毁窗口**(看不见任何反应),第二次点击才真正打开。新语义:无窗口/不可见/最小化 → `ShowDetail`;可见但 `GetForegroundWindow()!=detail_` → 带到前台;仅当窗口确实在前台才 `CloseDetail`。托盘点击链路有 `tray lbuttonup -> toggle` / `show detail: created=? was_vis=? animated=? iconic=?` / `close detail` 日志可断言。CUA 真机测托盘:图标多在溢出区(chevron 后),用 UIA 找 `TopLevelWindowForOverflowXamlIsland` 里 Name="QuotaBar" 的 BoundingRectangle 再发物理 click;**含中文的 .ps1 必须 UTF-8 BOM 或纯 ASCII(Git Bash heredoc 产出的无 BOM 文件按 GBK 解析会炸语法)**。**两个补充坑(用户实测"窗口缩到最小状态,托盘点不回来")**:①`IsIconic` 窗口的 `IsWindowVisible` 仍为 TRUE → ToggleDetail 正确路由进 `ShowDetail`,但 ShowDetail 尾部只会 `ShowWindow(SW_SHOW)`——**SW_SHOW 不还原最小化窗口**,表现为窗口卡在任务栏、连点托盘无响应(日志实锤:23:09 连续 4 次 `show detail created=0 was_vis=1` 均未还原);修复 = ShowDetail 内 `IsIconic → ShowWindow(SW_RESTORE)`(SW_RESTORE 按此前状态还原,普通/最大化都对);②可见但非前台的分支**不要用 SW_RESTORE 带前台**——最大化的窗口会被缩回普通尺寸,感知即"窗口自己变小了",只 `SW_SHOW+SetForegroundWindow`。
- **圆环弧刷新过渡(2026-09-13)**:`Overlay::SetupRingAnimation/TickRingAnimation`——`UpdateRings` 检出 arc_percent 变化后,从**当前显示值**(`ring_disp_`,非上帧目标值)ease-out cubic 400ms 滑向新值(16ms WM_TIMER 驱动,期间每次 Paint;静态期仍零重绘);无比例语义项(arc_percent<0,金额满环)直接落位不参与;环数量变化/`AnimationsEnabled()`为假/模式切走(非 Rings)/内容清空,四处都直接对齐并停表。PaintRings 一律读 `ring_disp_`(无过渡时与 arc_percent 相等)。
- **文字模式柔和投影(2026-09-13)**:overlay 文字模式每段先画 alpha≈70 黑投影(下移 max(1,Scaled(1)) px)再画正文——文字浮在任意壁纸无衬底,曾发生深色主题黑字黑壁纸"近乎不可见"误判(见下条);圆环模式有垫盘不加。这会给所有字色加轻微重量,属 vibrancy 取向,勿调大。
- **主窗口 stitch 设计 1:1 还原(2026-09-13 首版;2026-09-17 按 `design/reference/stitch_minimalist_app_interface` 重构)**:主窗口全自绘重写(自定义标题栏 WM_NCCALCSIZE+WM_NCHITTEST 拖拽缩放 / 侧栏导航 / 页面;同日「调整位置」页并入「显示设置」,导航为两页:额度详情/显示设置)。2026-09-17 重构要点:①标题栏降为 44 DIP(kTitleDip),左上 logo 改为**手绘环形渐变 logo**(`DrawLogo`:青→靛→紫渐变圆环,不再用应用 .ico/旧方块 logo,`DrawAppIcon` 已删);②侧栏底部新增版本号(读 exe 版本资源 `AppVersionText`)+在线/已暂停状态页脚;③额度页页头新增「刷新间隔 chip」与右上「总计可用额度」两行统计;④卡片改为圆角 16+细描边+hover 提亮(`g_cursor_pt` 判卡级 hover),数值区内嵌深色面板(kSInset)+圆环窗口标签(5h/7d mono)+大号白字%,页脚「打开控制台→/设置▾」改分隔线式;⑤趋势图重画(面板内左缘刻度+虚线网格+渐变面积+端点环)。设计 token 仍在 theme.hpp `kS*`(2026-09-17 换为 stitch_minimalist Tailwind 色板:app.bg #0B0F17/card #141B2B/accent blue-600 等,新增 `kSCardBorder/kSBorderSoft/kSInset/kSGridLine` 与 `TypeSize::Mini`(10px));等宽字体 `kFontMono`(Cascadia Mono)只用于**纯 ASCII**文本(IsAsciiText 守卫)。「此前的"主窗口不得加 WS_CLIPCHILDREN"约束已失效」仍成立。修改主窗口 UI 前先读 `stitch_/stitch_minimalist_app_interface/screen.png` 对照。仅文本输入保留原生 EDIT(无边框,位置由 `Relayout()` 按**与 paint 同一个 content 矩形**摆放,两处坐标系不一致 = 输入框与自绘底错位,实测过;content 矩形=NavW+28 / kTitleDip+24 / 右-28 / 底-24,PaintDetail 与 Relayout 两处必须同步改)。
- **账户卡片墙(2026-09-14 重构)**:主窗口改两页(额度详情/显示设置),「账户设置」页删除并入额度卡片——每 coding plan 账户一张卡:徽章+**别名**标题+类型副题、状态 chip(停用优先显示"已停用")、**启用开关**(即开即落盘:停查询+常驻条隐藏+卡片置灰)、**显隐开关**(eye;隐藏卡从页头「已隐藏 (N)」区找回)、额度仪表、ⓘ 注解、控制台链接、「设置 ▾」 accordion 展开区(别名/Key 输入 + DPAPI 说明 + 删除/保存;codex 卡显示 auth.json 状态+重新读取)。页头「添加卡片」内联面板:类型 chips(Codex 已存在则禁用)→别名+Key→创建即查询。布局走 `ComputeQuota` 单一来源(含滚动钳制/偏移),`PaintQuota`/`Relayout` 消费同一布局;`g_qlayout/g_cards` 是 paint 产物,HandleAction 按同坐标消费(vis_idx/hidden_idx 映射账户)。添加面板与展开区的 EDIT 在 `CreatePageControls` 按 id(kEditAddAlias/kEditAccAlias0+vi 等)创建、`Relayout` 按 ComputeQuota 矩形摆放(内缩:别名 10/键 38 左、102 右)。
- **GDI+ DrawString 整行跳过(LineLimit)**:GenericTypographic 格式下布局框高 < 字体行高(约 1.33×字号)时**整行静默不绘制**(非裁剪)。实测 44px 字体在 56px 框消失、60px 框正常。所有自绘文本框高必须 ≥ 字号×4/3,排查"文字莫名缺失"先查框高。小节标签(12px)实践值:框高 ≥`LS(16)`;14 逻辑高的框在 192 DPI 下(28px<32px 行高)整批静默消失。
- **RC(x,y,w,h) 语义**:tray_ui 内矩形统一用 `RC(x,y,w,h)`(x/y+w/h);传绝对右/底缘当宽高会把控件摆成巨型窗口(实测 EDIT 盖住整卡,2026-09-13 排版重构时 tweak_panel 的 hug 高度忘减 panel_top 又踩一次;2026-09-16 趋势图刻度标签第三次踩——`RC(left, y, plotLeft-4, y+18)` 把绝对坐标当宽高,文字被画到图表右外侧"消失";2026-09-17 stitch_minimalist 重构第四次踩——ComputeQuota 的 stats/trend/ex_* 全部把绝对右缘当宽传入,卡片内容整体炸版,截图一眼可辨)。**写含计算的矩形一律直接 brace-init `RECT r{L,T,R,B}`**,只有纯 DIP 常数才用 `RC(...)`。grep `RECT [a-z]*{` 与 `RC(` 审计。
- **流式布局 RowDown(int&,...) 无返回值**:原地 `y += LS(h)+LS(gap)` 推进;若写成 `y = RowDown(y,..)` 会把(已删的)返回旧值赋回 y 抵消推进,整卡行全部叠在同一点(实测)。布局一律 Compute* 单一来源产矩形、Paint* 只消费,禁止两处各算一遍。
- **标题栏图标与头像(2026-09-13)**:标题栏左上角用 `DrawAppIcon` = **应用图标本体**(`LoadImageW` 按 DPI 取 IDI_QUOTABAR,失败退回手绘 `DrawLogo`)——手绘 logo 与 ico 是两套设计,曾造成"左上角和 APP 图标不一样";右上角纯装饰头像已删除(无功能,删前无命中注册)。新增窗口级图标资源引用时注意 `DestroyIcon` 与 DPI 变化重建。
- **小窗紧凑变体(2026-09-13)**:最小窗口 **960×760**(内容总高决定最小尺寸下限,改前先算各页内容高)。原账户设置页 `ComputeAcc` compact 分支已随页面删除;显示设置按 leftover 阶梯降级(快捷键行/沙盒——沙盒已删,降级只剩快捷键行);「自启动/清除数据」合并行用勾选 chip。
- **主窗口排版密度规范(2026-09-13 流式重构)**:页内纵向布局走 `RowDown` 流式游标(行高 kRowH=40/行距 kRowGap=8/分区 kSecGap=16,DIP);卡片高度随内容收缩、页脚行并入内容流,禁止"内容在上+页脚钉卡底"式布局(网格拉伸时中间留大面积死区);辅助文字最低 12px(废除 11px);不适用/未开启的设置**条件隐藏或渐进展开**(阈值行随预警开关显隐,Relayout 同步 ShowWindow/EnableWindow),不得整块压暗占位。
- **等宽字体(Measure/Draw 不一致)**:Cascadia Mono 无 CJK 字形,MeasureString 与 DrawString 的 fallback 宽度不同 → chip 截断;且字体缺字形 ≠ 报错。**含 CJK 的文本一律默认 Segoe UI**,mono 仅用于纯 ASCII(时间戳/数值/URL)。
- **按值返回 wstring 的 c_str() 悬垂**:`std::wstring f(){static s;...}` 按值返回的临时在语句结束析构,`f().c_str()` 存进数组即悬垂(实测"宿主环境适配"乱码)。先存局部变量再取 c_str()。
- **up 激活不依赖 down**:主窗口自绘控件的 WM_LBUTTONUP 按 `HitAt(pt)` 命中直接触发(不要求等于 down 的控件),shot_pages.ps1 等自动化仅 Post WM_LBUTTONUP 即可切换页面/触发按钮;按压视觉(down 高亮、拖离取消)与激活判定解耦。shot_pages.ps1 的导航坐标按 1000×780 客户区比例(x=112/1000, y=(110+40i)/780)。
- **主窗口点击无响应两坑(2026-09-13 实测修复)**:①`HandleAction` 的链接分支 `if (id >= kWLink0)` **必须带上界**(`id - kWLink0 < (int)g_links.size()`)——kWLink0 之后的 id 全是页面控件,无上界时被当链接吞掉,表现为"所有页面按钮点了没反应"(导航 id 5..8、复制全部 id 9 在 kWLink0=11 之前所以幸存,极具迷惑性);②`WM_NCHITTEST` 的 pt 是**屏幕坐标**,`HitAt` 用客户区坐标,判 HTCAPTION 前必须 `ScreenToClient` 转换,否则整个标题栏(含刷新/最小化/关闭)变成拖拽区。排查"点击无响应"先用日志在 WM_LBUTTONDOWN/UP 打 pt+id:down/up 都有 id 且 up 后无动作=分发吞了;没有 down=命中测试判了非客户区。
- **CUA 真机点击回归**:自动化截图(直接 Post 消息)验证不了点击链路,NCHITTEST/分发 bug 只有真机点击才暴露;用 computer-use 的 coordinate 点击(实测 raster→物理像素映射精确 1:1),配合日志 down/up id 与 config.json/剪贴板等旁证断言。
- **位置微调快捷键(显示设置页)**:Alt+←/→ 微移 1px、Shift+←/→ 步进 4px(设计稿「高频快捷键协同」的真实实现),在 `PreTranslateMessage` 里先于 IsDialogMessage 处理(页面判断用 `kPageDisplay`)。**Alt 是系统键:按住 Alt 的方向键产生 `WM_SYSKEYDOWN` 而非 `WM_KEYDOWN`,只判后者 Alt 组合永远收不到(2026-09-16 用户实测"Alt+左右不微调"的根因,须两个消息都接)**;焦点在 EDIT 输入框时不抢 Shift+方向键(文本选择优先),Alt 组合恒拦截;命中后 return true 吞掉,不进 IsDialogMessage/DefWindowProc(防触发系统菜单模式)。调整实时预览走 `SetSlotOffsetDip`,抬手/点击即 `CommitConfig` 落盘。
- **额度卡片开关与 ⓘ 注解(2026-09-13,2026-09-14 被账户卡片墙取代)**:原固定 kWShow0..3 常驻条开关已删,改为账户卡片的启用/显隐双开关(动态 id,见「额度页动态 id 空间」);显示设置页「显示平台」chips 与任务栏模拟预览已随沙盒删除;「常驻条已隐藏」提示章已随账户页删除。卡片 ⓘ(口径说明/月池状态)在**头部副标题之后**(kWNote0..3),正文悬停显示;气泡必须在 `PaintQuotaCard` **内就地绘制**(`DrawNoteBubble`,水平钳在卡片内)——曾实现为「卡片循环末尾统一绘制」,其中 `int ty = by + LS(5)` 的 `by` 意外命中外层 `PaintQuota` 的页头按钮 y 变量(局部重命名漏改),气泡文字全画到内容区左上角(用户实测"注解没显示在悬停处");就地绘制使命中与绘制同帧同坐标,此类跨作用域错位不可能再发生。
- **显示设置合并页(2026-09-13 并入;2026-09-14 删任务栏模拟沙盒与测试模式;2026-09-16 二次重构为 macOS 系统设置式分组卡片,旧双卡/单列描述作废)**:导航两页(`kWNav0..1`)。**设计规范(用户拍板"参考苹果/谷歌设置页")**:设置列限宽 640 居中;分区=小节标题(Body 粗灰,卡上方)+圆角卡片(kSSurf,r12);卡内行高统一 48(模式行 54),行间细分隔线(左缩进 20,模式行 48);卡左右内边距 20/16;**右缘控件统一内边距 16 与统一尺寸**——开关 44×24、三段选择 240×28(文本居中,选中 kSPrimCont 胶囊)、步进 28×28、按钮 30 高;标签一律 Body 居左、说明 Cap;偏移数值用 **Num 弃 Hero**(巨字与置零按钮失衡,用户批"字体混乱")。分区:显示模式(整行单选行,kModes 嵌入置顶;**2026-09-21 起为 4 项**:嵌入/浮条/仅托盘/圆环,贴靠并入浮条吸附)/外观(主题+排版+字号[仅非嵌入]+字色+低额变色+品牌色;嵌入另加名称行/窗口标签两开关)/位置微调(嵌入=偏移+滑杆+快捷键;浮条=吸附开关+X/Y 滑杆+拖动提示;圆环=吸附偏移滑杆;仅托盘整区隐藏)/刷新与提醒/启动与数据。滚动 `g_dscroll` 滚轮驱动、ComputeDisp 钳制统一 shift;**列表绘制 SaveDC+IntersectClipRect 裁剪在视口内(vp=content.top+52..content.bottom-46)**。**原生 EDIT 不受 DC 裁剪:Relayout 的 place_edit 对滚出视口(部分越界即算)的输入框 ShowWindow 隐藏,否则"900"框悬浮在页脚条上(实测)**。**命中区域规则(g_hit_content/g_hit_vp/g_hit_extra,HitAt 判定)**:滚动列表项只在列表视口内可点,chrome 区(侧栏/标题栏)与页脚恒可点——无此规则点页头误触滚背行、侧栏导航被误杀。**显示模式 UI 顺序表 `kModes[4]`**:嵌入任务栏置顶,绘制与 HandleAction 共用防漂移。**即调即存**:模式/排版/字号/字色/主题点击即 `App::ApplyDisplayLive`/`SetUiThemeMode`,不等「保存设置」;刷新间隔/预警/自启动仍走草稿+页脚保存。
- **滑杆抬起落盘与保存竞态(2026-09-13 实测修复)**:`WM_LBUTTONUP` 里 `ReleaseCapture()` 会**同步派发 `WM_CAPTURECHANGED`**,原代码先 Release 再判 `g_slide_drag` 已被清零,位置滑杆抬起永不落盘——实时预览(`SetSlotOffsetDip` 只改内存 cfg_)生效但配置未写,点「保存设置」又把进页快照的旧 `slot_offset_x_dip` 整体提交,表现为"保存后条跳回旧位置左边"。修法三件套:①抬起时**先取 drag id 再 ReleaseCapture**;②`kWSaveCfg` 以 `g_draft.slot` 覆盖 `cfg.display.slot_offset_x_dip`(草稿里的 slot 是进页快照);③`CommitSlot` 回写 `g_draft.cfg` 保持一致。端到端验证:posted 滑杆 down/up → config.json 立即变,保存后不变,恢复点击精确回到目标值。
- **分屏贴靠几何(2026-09-13)**:`WM_GETMINMAXINFO` 的最小宽/高**必须钳制到目标显示器工作区一半/全高以内**(`MonitorFromWindow`+工作区实测,注意 LONG/int 混算的 std::min 歧义)——否则 200% 缩放下 1920px 最小宽压着半屏贴靠(1013px)撑过分屏线,用户实测"右半屏多出一块"。窄窗配套:客户宽 < 设计宽(1000 DIP×缩放)时侧栏收 56 DIP 图标栏(`RailNav/NavW` 单一来源),content 矩形在 PaintDetail/Relayout/显示页页脚条三处都用 `NavW()`;额度卡 2×2 网格与卡片内部全是相对矩形,自然重排。新窗口创建时 `GetDpiForWindow` **未显示窗口可能返回 0**(实测),`ShowDetail` 以 `GetDpiForMonitor(MDT_EFFECTIVE_DPI)` 兜底;`WM_SIZE` 里再校一次窗口实际 DPI 纠偏 g_layout_dpi(跨屏拖动/贴靠后 DPICHANGED 丢失的路径自愈)。
- **改 config.json 用 Python 无 BOM 写**:PowerShell 5 的 `Set-Content -Encoding UTF8` 会带 BOM(`EF BB BF`),应用 JSON 解析失败→静默回退默认配置(mode=Docked/slot=0/cred_rev=0)并落盘,表现为"改了配置没生效";核对 config.json 是否被意外重置先查 BOM。

### A/B/F 迭代(2026-09-15)新增事实与坑

- **用量历史(A)**:`history.json` 每账户滚动采样——百分比窗口按 `stable_key` 分序列(同值 <10 分钟去重,每序列封顶 2000,保留 30 天);**codex 以身份锚入盘(2026-09-22)**:采样前 `Fnv1a64Hex(GetAccountId)`(`RecordHistoryNewData`)写入该段 `identity_key`,键变化即 `ApplyIdentityKey` 整段清空重记;启动时 `LoadStartupState` 再校验一次(进程关闭期间切换过 ChatGPT 账户→丢弃旧段);锚不上(非 chatgpt 模式/缺 account_id)不采样;cache 仍不含 codex(快照不落盘口径不变);换 Key/删账户整段删除该账户历史(cred_rev 隔离延伸)。速率=近 2h 样本 ≥3 个且跨度 ≥20 分钟才最小二乘拟合,斜率 >-0.5%/h 视为平稳不估;ETA 仅 ≤24h 且剩余 >0.5% 显示。今日消耗=今日 0 点(本地)前最后样本与最新样本之差,净增加(充值)不显示。落盘 ≥5 分钟合并(`SaveHistoryNow`),退出 force。卡片趋势带占位判定 `AccountHasTrend` 是 ComputeQuota(布局)与 BuildDetailCards(绘制)的**共同单一来源**,两处不一致=输入框与自绘底错位。**趋势图表(2026-09-16 从裸 sparkline 升级)**:`DrawTrendChart` 画纵轴刻度+网格线+轴线+面积填充,`DetailCard` 新增 `trend_is_percent`(百分比序列纵轴固定 0..100 诚实口径)与 `trend_span`("近 X小时",样本首尾时间差);金额刻度小数位按纵轴跨度选(span<0.5→%.2f),避免小跨度时三个刻度文案重复。
- **提醒体系(B)**:`App::OnNotifyThreshold` 每次成功查询都被调度器调用,**跟踪(last_pct/last_reset/below_fired)恒续更新**、与通知开关无关——保证关闭再开启后不误报陈旧状态;重置恢复=上期 ≤25% 且主窗口 reset_at 推进 + 新 pct 更高。每账户 `alert_pct`(0=继承全局 1=关闭 5..95)只对 Kimi/GLM 生效,codex 不参与阈值提醒,金额账户仍用全局 ¥ 阈值。Toast 走 notify.cpp(AUMID 捷径首用自动创建),失败回落 `tray_->ShowBubble`;设置里「低额度预警」总开关关闭则一切提醒静默。
- **双主题(F1)**:theme.hpp 全部颜色 token 是 **inline 运行时变量**(`constexpr` 已废)——`theme::SetUiTheme(dark)` 填充整套并递增 `UiThemeGen()`;任何**按主题缓存的 GDI 资源**(输入框底刷等 static HBRUSH)必须按 `UiThemeGen()` 判定重建(实测 theme.cpp `ThemedSolidBrush`/tray_ui `ThemeInputBrush` 两处),否则切换主题后残留旧色。系统切换经 WM_THEMECHANGE/ImmersiveColorSet → `ApplySystemTheme_()`。**2026-09-16 起支持手动切换**:`cfg_.ui_theme`(0=跟随系统 1=深色 2=浅色,config.json 根级键,storage Dump/Load 同步),显示设置页「主题」三段行(常驻显示分区内、模式行之下,任何模式都可见)点击即 `App::SetUiThemeMode`(即选即生效+落盘);`ApplySystemTheme_` 按 ui_theme 决定深浅,手动覆盖时系统主题变化不再倒戈。**EDIT 子控件只在自身失效时才重发 WM_CTLCOLOR* 查背景刷——主题切换后必须 `EnumChildWindows` 逐个 InvalidateRect,否则输入框残留旧主题底色(实测)**。浅色调色板为 Material3 亮色映射;**勿在徽章/文字上用 kSPrimaryFix 作前景色**(浅色主题下不可读,实测 GLM 徽章 G 改 kSPrimary)。带 capture 的 lambda 不能转 WNDENUMPROC(ModernizeDialog 实测),捕获less + 内部调 UiIsDark()。
- **卡片拖拽排序(F3)**:三个实测坑——①**ReleaseCapture 竞态第二例**:WM_LBUTTONUP 里先 ReleaseCapture 再读 `g_drag_from` 永远拿到 -1(同步派发 WM_CAPTURECHANGED 清零,与滑杆落盘同款),必须先快照;②`ComputeDragTarget` 的 pt 与 `QuotaLayout` 矩形**同为物理像素**(ComputeQuota 全程 LS 换算),不要再除 DPI 转换(双重转换曾使 GLM 落点从 slot2 漂到 slot0);③落位判据=其余卡片中线,同卡行内再按水平中线分左右,拖到内容区空白=移到末位。落位后 `MoveAccountOrder`(内含 secrets 对位重写,见关键不变量)+ `RebuildPage()`(可见序变=动态 id/输入框全重建)。
- **CUA 真机点击 Bring-to-front**:主窗口被宿主应用全屏覆盖时,`SetForegroundWindow`/`SwitchToThisWindow`/`AttachThreadInput`/HWND_TOPMOST(从 PowerShell)都会被前台锁或 z 序压住——**正路是 computer-use 的 `open_application(app:{pid,name:窗口标题}, activate:true)`**(实名校验用窗口标题"QuotaBar — 额度详情"而非进程名);前台后真实点击链路(down/up/drag/TrackPopupMenu)全部可测。`tools/show_main.ps1` 可从托盘态唤醒主窗口。
- **PrintWindow 截图坑(2026-09-13)**:对自绘双缓冲+自定义框架窗口,无 `SetProcessDPIAware()` 的 PowerShell 进程读 `GetWindowRect` 是 **96 DPI 虚拟化坐标**(2026 实宽读成 1013),PrintWindow 又把真实表面渲染进小位图——表现为"布局被裁掉一半/尺寸对不上"的假象;截图像素可疑时先查脚本 DPI 感知。PrintWindow(flags=0→WM_PRINT) 同步走 `PaintDetail` 时内容是新鲜的,但 flags=2 等 DWM 路径可能给陈旧表面(数据时间戳一眼可辨)。**posted(合成的) WM_MOUSEMOVE 会立即触发系统 WM_MOUSELEAVE**(真实光标不在窗口内),`g_hover` 被马上清零——合成悬停永远验证不了 hover 视觉,真机鼠标才是有效验证(CUA mouse_move 或用户肉眼)。自动化脚本(.ps1)注释只写 ASCII:无 BOM 时 PowerShell 按 ANSI(GBK) 解析,中文注释尾字节会吞掉下一行行首字符。**改 config.json 用 Python 无 BOM 写**:PowerShell 5 的 `Set-Content -Encoding UTF8` 会带 BOM(`EF BB BF`),应用 JSON 解析失败→静默回退默认配置(mode=Docked/slot=0/cred_rev=0)并落盘,表现为"改了配置没生效";核对 config.json 是否被意外重置先查 BOM。

## 2026-09-18 四专家检视与安全重构（178→187 项自测全绿）

架构/工程/UI/编码四路只读检视后,零功能变更地修复了一批潜伏缺陷。**新增不变量(改动相关代码前先读)**:

- **JSON 字符串落盘必须走 `JsonQuote`(json.hpp)**:storage 的 JStr 已改为委托它。旧 JStr 漏转义 `\t`/`\r` 等 C0 控制字符——别名一旦混入 Tab,config.json 即成非法 JSON,下次启动 `LoadConfig` 失败→静默回退默认配置→**全部账户丢失**。手写 JSON 的任何新增字段同样必须走 JsonQuote。
- **`TrimW` 宽字符空白按码点判**(util.cpp):禁止 `(unsigned char)wchar` 截断判空白——CJK 低字节 ≤0x20 的字(一/三/上/下…)会被静默剪掉(别名首字丢失实测链路)。窄字符 `Trim` 不受影响(UTF-8 连续字节恒 >0x20)。
- **主题画刷缓存的 gen 必须随 cache 成对存储**:theme.cpp `ThemedBrushCache`/tray_ui `ThemeInputBrush(brush,gen&)`。函数级 `static gen` 服务多个缓存时,主题切换后先到的调用方把 gen 盖到最新,后到的缓存**永不重建**(禁用输入框残留旧主题底色)。新增按主题缓存的 GDI 资源照此模式。
- **调度器空密钥也推进 next_fetch**(scheduler.cpp StartFetch):直接 return 会让 deadline 停在过去,RescheduleTimer 钉在 100ms 下限→RunDue 每 tick 重进(每秒 10 次文件读+日志的定时器风暴,codex 登出后实测触发)。密钥恢复走 `OnAccountChanged` 清零退避并立即重查。
- **`PrimaryBalance`(state.hpp)是"主币种=CNY 优先否则首项"的唯一实现**:通知/常驻行/圆环/详情/历史采样五处原先各写一份,口径漂移源;新增消费方直接调用,不得再内联。
- **别名长度在 ConfigIo::Load 钳制到 64 宽字符**(与 EM_SETLIMITTEXT 同口径):外部手改 config.json 的超长别名会撑爆通知文案的 `swprintf_s(buf[96])`(secure CRT→invalid parameter 崩溃)。
- **`JsonValue::AsInt` 有符号解析+ERANGE 检查**(json.cpp):旧 `_strtoui64` 对溢出静默返回 _UI64_MAX、负数实现定义回绕——错误值进 cred_rev 会让凭证隔离失真。
- **overlay 文字模式绘制字号=测量字号**(Paint 与 MeasureModel 同用 `font_px*72/96` pt):旧绘制用裸 `font_size_pt` UnitPoint,DIB Graphics 恒 96 DPI→高 DPI 下字比测量窗口小 dpi/96 倍(200% 只有一半大)。圆环/嵌入槽/主题按钮全部早已是像素口径,此为最后一处。
- **`Overlay::OnFullscreenCheck` 必须走 Show()/Hide() 状态机**:直调 ShowWindow 会让 `visible_` 与窗口态失配(Show 守卫短路、Update 的 !visible_ 补显也短路)。配套:Show() 在无位图时**不得置位 visible_**(否则标志已真窗口未亮,补显永久失效);ULW 提交失败不置 `has_bitmap_`(下次 Update 强制重画自愈)。
- **CommitConfig 保留运行值的单调字段**(account_seq 取 max、first_run_done 取或):next 常是 RebuildPage 草稿快照,旧 seq 回退会让下一次 AddAccount 撞 id(cache/secrets/调度按 id 路由全错乱)。
- **托盘 RecreateIcon(Explorer 重启)后必须补 NIM_SETVERSION**:NIM_ADD 回退 v0 回调语义(丢 WM_CONTEXTMENU/键盘通知路径)。
- **HTTP 回调线程 wnd/msg 在锁内快照、PostMessage 失败就地 delete**(http.cpp StatusCallback):锁外再读 `self->` 与 Finalize 的 delete 有竞态窗口;队列满时 HttpEventMsg 泄漏。
- 其余:theme 按钮子类化 WM_NCDESTROY 先存 old 再 delete(旧写法 break 落到统一转发是 UAF);`EstimateDepletion` 自变量先减均值(绝对 epoch 小时的 `n*sxx-sx*sx` 灾难性消去,斜率数学恒等);taskbar_slot `GetDpiForWindow` 0 值护栏;LoadCache 先取布尔再 move;NowMono 缓存 QPF;DrawBadgePx 改 UnitPixel。
- **tests.cpp `TestRegressions`** 锁定上述行为(TrimW CJK/JsonQuote 往返/AsInt 溢出拒绝/PrimaryBalance/别名钳制)。

检视结论(未动的部分,留作后续):tray_ui.cpp 3680 行 God Module 的拆分(ui_icons/quota_page/display_page/window_shell/tray_menu 天然切缝)建议先补 ComputeQuota/HitAt/HandleAction 纯函数测试再动;overlay Paint 行距 1.15 双乘(测量已含,绘制又乘,n=2 时亚像素级)未改避免视觉变化;嵌入槽 L 锚失败时 slot_offset 被吞(taskbar_slot offset_applied 置位过早)属锚定链实测锁定区,动前需 slot_probe 复测。

## 2026-09-18 第二轮：ui_layout 抽出 + ProviderOps 分派表（187→211 项自测全绿）

God Module 拆分第一步与 provider 知识收敛，零行为变更（shot_pages 六页截图 + 四账户真机联调核验）：

- **ui_layout 模块**（src/ui_layout.{hpp,cpp}）：tray_ui 三个事故高发判定（滑杆取值/拖拽落位/命中测试核心）抽为无全局纯函数；tray_ui 保留同名薄包装（喂 g_layout_dpi/g_qlayout/g_hits），全部调用点零改动。**后续拆 tray_ui 按此模式推进**：先抽纯逻辑+补测试，再移绘制段。
- **ProviderOps 分派表**：providers.hpp 八元组一行一平台（id/契约/徽章/控制台/别名/用Key/build/parse）；scheduler 组请求（原 switch）、选解析器（原 4 路 if-chain）、写契约版本（原 4 路 if-chain）三处与 providers 五个辅助 switch 全部查表。未知 provider_id 走 pr 默认（ok=false）→ ProtocolError 停机，与旧未命中分支同语义。
- **CfgRef() 删除**：App 不再外泄可变配置引用；嵌入条偏移落盘改走显式命令 `App::CommitSlotOffsetDip(dip)`（±200 钳制+落盘+RefreshDisplay），tray_ui CommitSlot 不再自赋值式 CommitConfig。
- **测试**：TestUiLayout（滑杆端点/中点/越界/退化/高DPI；拖拽上/左/右/中线/空白/非法序；命中优先级/滚动裁剪/chrome/豁免区）+ TestProviderOps（四平台表项一致性/id 查表/未知 id）共 24 项。
- **build.bat 事故与重建**：MSYS2 `sed -i` 对 GBK 文件即使零命中也会毁写（中文→`?`、CRLF→LF，cmd 吞行首字符整批报废）；已按 GBK+CRLF 完整重建（命令字节与原一致，中文注释按原意恢复）。**教训已写入构建节：改 build.bat 只用 Python 二进制读写**。

## 2026-09-18 第三轮：品牌视觉收敛 + overlay 表面去重（211→213 项自测全绿）

- **ProviderOps 扩为"类型→平台行为+视觉身份"单点**：新增 `brand_color`（原 theme.hpp kBrand* 四色迁入,theme 只留随主题变的 token）/`subtitle`（原 state.cpp type_subtitle）/`type_tag`（原 tray_ui.cpp TypeTag）；新增 `ProviderOpsByBadge(badge)`（卡片绘制只持徽章字母,未知字母回落 Kimi 与旧 switch 同）。**state.cpp 从此不再 include theme.hpp**——恢复 state.hpp"显示模型不携带界面配色"的自述契约；消费方只剩 providers 分派表一处定义。
- **overlay 绘制表面去重**：`UlwSurface` RAII 帮手（顶向下 32bpp PARGB DIB+兼容 DC,Paint/PaintRings 共用,原两段 ~20 行样板合一）+ `Overlay::PresentSurface_`（ULW 提交+has_bitmap_ 成功才置位,原两段 ~15 行合一）；Paint/PaintRings 绘制主体未动。
- 验证：TestProviderOps 增视觉字段逐字断言（色值/副题/类型章/徽章回落）共 2 项;shot_pages 六页+四卡徽章放大核验（K 灰/C 青/D 蓝/G 紫、类型章、副题逐字一致）;**实机切圆环模式端到端核验**（四环/四色/徽章-数值配对 K-62%/C-90%/D-¥23/G-95% 正确,新 UlwSurface 路径工作正常）后恢复 Embed。

## 2026-09-18 第四轮：ComputeQuota 纯函数化（213→232 项自测全绿）

tray_ui 拆分第二步（继 ui_layout）：**额度页布局计算器整体抽出**为 `quota_layout.{hpp,cpp}`——
`QuotaCardR/QuotaLayout/QuotaPageIn(QuotaCardIn)` 类型 + `ComputeQuotaLayout(in,content,dpi,scroll_out)`
纯函数（App/全局依赖→显式输入快照；RC brace-init 纪律与滚动钳制写回语义原样保留）。
tray_ui 侧 `ComputeQuota(App*,RECT)` 变为快照薄包装（g_q.scroll 经引用钳制写回），
全部调用点与 `using ui::QuotaLayout/QuotaCardR` 引用零改动。

- **TestQuotaLayout 19 项**锁定布局不变量：2×2 网格同行等高顶对齐/列间行间不重叠/等宽、
  隐藏分组 vis/hidden 序、展开卡 expand_h 恰增卡高（含阈值行）、趋势带 122 DIP 占位/卡加高 142、
  添加面板控件链与首卡下移、滚动钳制（超大→max_scroll/100px 平移/装得下→0）、DPI 尺寸加倍。
- 回归：构建 /W4 零警告；shot_pages 六页 + 展开/添加面板路径截图核验通过；四账户实机运行正常。
- 剩余拆分目标：ComputeDisp 同模式（display_page）、绘制段迁移（PaintQuota/PaintQuotaCard）、
  窗口壳（DetailProc 族）。**模式已定型：抽纯计算→补不变量测试→再移绘制**。

## 2026-09-18 第五轮：ComputeDisp 纯函数化（232→242 项自测全绿）

tray_ui 拆分第三步（ui_layout/quota_layout 同模式）：**显示设置页布局计算器抽出**为
`disp_layout.{hpp,cpp}`——`DispLayout` + `DispPageIn{mode,notify_enabled,scroll}` 输入快照 +
`ComputeDispLayout(in,content,dpi,scroll_out)` 纯函数（分组卡片/行游标/滚动钳制原样迁移）。
tray_ui 侧 `ComputeDisp` 变为 g_draft.cfg 快照薄包装，`using ui::DispLayout` 保持引用零改动。

- **TestDispLayout 10 项**：Embed=嵌入说明+偏移滑杆/隐藏排版字号字色；Docked=反向；
  仅托盘=位置区整体隐藏且内容更短（分区序号紧凑不空占）；预警开关→阈值两行；
  五模式行/分区卡有序；设置列 640 DIP 居中；滚动钳到 max_scroll；DPI 192 行高 54→108。
- 回归：/W4 零警告一次通过；shot_pages 六页核验（显示页五分组卡片无偏差）；实机运行正常。
- 纯计算三模块（ui_layout/quota_layout/disp_layout）已就位，tray_ui 剩余主体为绘制段
  （PaintQuota/PaintQuotaCard/PaintDisplay）与窗口壳（DetailProc 族/Hover 状态机/托盘）。

## 2026-09-18 第六轮：图标库抽出 ui_icons（242 项自测全绿）

tray_ui 拆分第四步：`Ic` 枚举（48 个）+ `Icon()`/`DrawLogo()` 迁至 `ui_icons.{hpp,cpp}`
（自 tray_ui 原样搬移，零调用点改动）；DPI 经 `SetIconsDpi` 注入——**tray_ui 的
SetLayoutDpi 必须同步调用它**（新增窗口/换屏路径若只设 g_layout_dpi 不设图标 DPI，
高 DPI 下描边会按 96 渲染）。注意 GDI+ 头前置要求 `<objidl.h>`。

- tray_ui.cpp 3680 → 3036 行；新模块累计：ui_icons(291)/ui_layout(53)/quota_layout(168)/disp_layout(171)。
- 验证：/W4 零警告；242/242；shot_pages 六页图标全链路核验（logo 渐变环/标题栏三键/
  刷新/加号/卡片小图标无缺失变形）；实机运行正常。
- 剩余：PaintQuota/PaintQuotaCard/PaintDisplay 绘制段迁移、DetailProc 窗口壳/悬停状态机、
  托盘段。绘制段迁移需要先立 `tray_ui_internal.hpp`（暴露共享 g_* 全局与 DrawBtn 族原语）。

## 2026-09-19 第七轮：绘制段迁移 quota_page/display_page + tray_ui_internal.hpp（242 项自测全绿）

tray_ui 拆分第五步（God Module 终局阶段之一）：新增 3 个文件，绘制主体离开 tray_ui.cpp。

- `tray_ui_internal.hpp`(246 行)：三个绘制 TU（tray_ui/quota_page/display_page）的内部共享契约
  ——常量、kW* 页面枚举、kAcc*/kEdit* 控件 id 与 IsQuotaEditId、hit/hover extern、PageLink/
  Draft/QuotaUi/BtnStyle/ModeDef 结构、Draw* 原语声明、`extern const AccountType kAddTypes[4]`。
  **归属铁律：默认实参只在头文件**（cpp 定义处重复默认实参即 C2572）；**g_* 全局只在
  tray_ui.cpp 定义**（页 TU 只 extern 引用）；结构体成员函数内联在头文件里（配 `<algorithm>`）。
- `quota_page.cpp`(702 行)：PaintQuota/PaintQuotaCard/DrawTrendChart(GDI+)/添加面板族 +
  kAddTypes/AddTypeLabel/AddTypeKeyPlaceholder 定义 + ComputeQuota 薄包装（快照 App 账户与
  g_q → ui::QuotaPageIn）。
- `display_page.cpp`(276 行)：PaintDisplay + `const ModeDef kModes[5]` 定义 + ComputeDisp
  薄包装（快照 g_draft.cfg）。
- tray_ui.cpp 3036 → **1965 行**（只剩窗口壳：标题栏/侧栏/DetailProc/HandleAction/Relayout/
  CreatePageControls/托盘）。匿名 namespace 已拆除（共享符号全部回到 qb namespace）。

编译教训（本轮 ~8 次编译迭代沉淀）：
1. `using ui::QuotaLayout` 等 using 声明**留在 tray_ui.cpp 不会传导**——每个页 TU 必须自带。
2. 头文件 extern 数组必须 `const` 而非 `constexpr`（constexpr 无外部链接，cpp 定义处 LNK2001）。
3. GDI+ 页 TU 前置 `<objidl.h>` 再 `<gdiplus.h>`（ui_icons 踩过，本轮 quota_page 又踩）。
4. 大段剪切用「标记注释 + Python 断言后写入」，标记行相邻时先重排标记再切（[B] 尾标吞掉
   [C] 头标会让断言失败但已避免写坏文件）。

验证：/W4 零警告；selftest 242/242；截图回归全绿——额度页四卡、添加面板（四 chips/别名/
Key 输入/眼睛复制清除/创建取消）、展开卡（**GDI+ 趋势图折线+渐变+数据点**、四操作按钮行）、
显示页五分组、**滚轮下滚后底部两组+页脚正常**（静态截图只见三组是视口未滚的正常表现，
非回归；滚动验证脚本 build/shot_scroll.ps1）、窄窗 rail（本轮窗口壳未动沿用前轮结论）。
实机重启运行正常。

- 剩余：DetailProc 窗口壳/悬停状态机/Relayout + 托盘段（tray_ui 最后两块）；C 类收尾
  （http SECURE_FAILURE 位、log.cpp 末条 flush、死代码清理 kWAddHelp/kWPosDec/kWPosInc、
  ~15 个未用图标枚举、EDIT id 守卫）。

## 2026-09-19 第八轮：C 类收尾四项（242→246 项自测全绿）

1. **http SECURE_FAILURE 位修复（真 bug）**：回调 `lpvStatusInformation` 是指向
   WINHTTP_CALLBACK_SECURE_FLAG_* 标志 DWORD 的**指针**，旧代码把指针低位当错误码存。
   现解引用取位、置 `0x80000000` 标记与 Win32 码区分；新增导出 `SecureFailureText()`
   解码（CN不符/证书过期/未知CA/吊销…），scheduler 网络失败日志据此输出人读原因。
   自测 +4 项（位解码/无标记不误判/空位回退十六进制）。
2. **log.cpp 合并行顺序修复（真 bug）**：同类合并的缓冲行写出时被追加在**新行之后**
   （`line += "\n" + g_last_line(xN)`），文件内时间序颠倒；改为旧行(xN)先落盘再写新行。
   注：日志在 `%LOCALAPPDATA%\QuotaBar\logs\quota_1.log`（logs 子目录，别 tail 错路径）。
3. **死代码清理**：kWAddHelp/kWPosDec/kWPosInc（UI 早改为滑杆/无 help 按钮，枚举+case 删）；
   26 个未引用图标（IcPerson/IcKey/IcHelp/IcWarn/IcRobot 等 24 个 case 臂 + IcNet 无臂 +
   IcDonutS 单行臂连带 IcDock）。enum Ic 49→23。**项目无 git，删改前快照在
   build/backup_r8/**。Ic 为非作用域自动编号枚举、无序列化/数值透传，删项移位无害；
   但删除脚本须警惕**单行臂**（`case X: …; break;` 同行）——按 `^\s*break;` 找臂尾会
   吞掉下一臂（本次恰好两个都在删除清单，结果正确，断言计数差一暴露了这一点）。
4. **WM_COMMAND 归属守卫**：EN_ 通知分派前校验 `GetAncestor(ctl, GA_ROOT) == h`，
   防对话框转发等外来同值 id 误触输入框联动重绘。id 区间核对无碰撞
   （kInterval 1214-1216 / kEditAdd 1301-1302 / kEditAccAlias 1400-1499 /
   kEditAccKey 1500-1599 / kWAccBase 2000+ stride20 / kW* 小整数）。

验证：/W4 零警告；246/246；shot_pages 六页全绿（展开卡小图标眼睛/复制/清除/刷新 +
rail 图标栏 nav 高亮逐项核验无缺失错乱）；实机 fetch 成功、日志正常落盘。

- 剩余（tray_ui 拆分收官）：DetailProc 窗口壳/悬停状态机/Relayout 与托盘段两块；
  以及可选的发布打包流程（release zip + RELEASE-NOTES，用户未要求）。

## 2026-09-19 第九轮：tray_ui.cpp 解体收官 + 发行打包（246 项自测全绿）

God Module 拆分终局：tray_ui.cpp（1967 行）**删除**，解体为三个 TU——

- `ui_draw.cpp`(460)：布局 DPI/编辑辅助、绘制原语（Blend/Ts/Txt/FillRr/DrawBtn 族/
  DrawNoteBubble）、**全部 g_* 共享全局唯一定义**（g_layout_dpi/g_hits/交互态/g_draft/
  g_q/g_qlayout/g_cards/g_dscroll）、BrandLook/TypeTag/AppVersionText/IsAsciiText/
  ToneColor/RemainText；**顺剔死代码 TimeOfDay**（全项目仅定义零调用）。
- `detail_shell.cpp`(1341)：标题栏/侧栏/内容绘制、交互辅助（滑杆包装/剪贴板/掩码）、
  ShowHelpForType、HandleAction、ThemeInputBrush、**DetailProc（悬停状态机/滚轮/拖拽）**、
  TrayUi 页面管理（SetPage/RebuildPage/Relayout/CreatePageControls/OnCommand/
  PreTranslate）。
- `tray_area.cpp`(170)：kTrayIconId/kIconResId + 图标生命周期（Init/Shutdown/
  RecreateIcon/UpdateTip/ShowBubble）+ 右键菜单（RefreshMenuState/OnNotify）。
- tray_ui_internal.hpp 归属注释更新；新增跨 TU 声明：kDetailClass（constexpr 入头）、
  DetailProc、AppVersionText。using 声明逐 TU 自备（QuotaLayout→ui_draw+detail_shell、
  QuotaCardR/DispLayout→detail_shell）。

切分方法论沉淀：**内容断言锚点**定位边界（marker 子串 + 行内容 assert），不用裸行号；
死代码在切分中顺路剔除需全项目 grep 佐证。

工程教训（本轮新增）：
1. **Bash heredoc 会吃一层反斜杠**：`\\t` 塌缩成 TAB 写进 build.bat（`src\tray_area.cpp`
   变 `src`+TAB+`ray_area.cpp`，C1083 报错文件名少 t）。`\u` 恰好非法转义按字面保留才
   没坏 ui_draw 项。**凡写 build.bat：Python 一律用 bytes([92]) 构造反斜杠，禁字面量**。
2. subprocess 跑 build.bat 的输出是 GBK，`text=True` 按 UTF-8 解码直接 UnicodeDecodeError；
   要 capture 字节 + `decode('gbk', errors='replace')`。
3. `str.rstrip('.0')` 是按字符集剥离不是按后缀（"1.1.0.0"→"1.1"）；版本号清洗用
   split/pop 尾零。

发行打包（新流程）：
- `tools/make_release.py`：构建 → 零警告断言 → selftest 门禁（失败 0 才继续）→
  `dist/QuotaBar-v1.1.0-x64.zip`（exe + README + RELEASE-NOTES，zipfile 完整性自检）。
- `RELEASE-NOTES.md` 新立：1.1.0 功能/工程/修复摘要（P0×6、P1×~15、TLS 原因解码、
  日志时序）+ 已知受限项（taskbar_slot 锚定、嵌入行高双乘——**实测踩坑区锁定的两项**）。
- README「构建」「发布包」段同步（旧 release/ 目录说法作废 → dist/ + 脚本）。

验证：/W4 零警告；246/246；六页截图全绿（额度页/添加面板/滚动页逐张核对，壳的滚轮
分发与页脚/底部卡片无回归）；**托盘往返实测**——shot_pages 关窗后 shot_expand 经托盘
消息唤出成功（OnNotify 左键路径 + ShowDetail + 滚轮 x8 日志 + 合并行 (x7) 时序正确，
R8-2 实证生效）；发行 zip 产物完整。

- 至此重构计划全部完成：tray_ui 3680 行 God Module → 9 个职责单一模块；遗留仅
  CLAUDE.md 锁定的两个实测踩坑项（不动）与 8 小时长稳观测（未要求）。

## 2026-09-19 第十轮：额度页滚动视口裁剪（246→251 项自测全绿）

用户实测 bug：额度页下滚后卡片墙整体盖住自定义标题栏与最小化/关闭按钮。根因 =
`PaintQuota` 滚动内容随 `g_q.scroll` 上移但**没有任何 DC 裁剪**（显示页有
`IntersectClipRect`，额度页在 2026-09-17 stitch 重构加滚动时漏配）；壳层先画标题栏
再画页面，卡片直接压在标题栏上。同源隐患：命中区不裁剪时，滚到标题栏底下的卡片
仍可点（点击落 WM_NCHITTEST 的 HTCLIENT 分支触发卡片动作，且标题栏拖拽被吞）。

- `ui_layout` 新增纯函数 `ClipHitRect(rc, clip)`（交集；空 clip/无交集返回空矩形）。
- `AddHit`（tray_ui_internal.hpp 内联）接 `g_hit_clip` 全局（定义在 ui_draw.cpp）：
  非空时命中注册裁到可见部分，完全滚出不注册。额度页 PaintQuota 滚动段置位/复位。
- `PaintQuota` 滚动段（添加面板/隐藏区/卡片墙/拖拽可视化）包
  `SaveDC+IntersectClipRect`，视口 `{content.top+LS(kQuotaHeadDip)..content.bottom}`；
  新常量 `kQuotaHeadDip=52`（与 quota_layout 的 after_head 同源）；拖拽插入线
  y 钳到视口顶。
- `PaintDetail` 额度页：`g_hit_vp` 同视口矩形、`g_hit_extra`=页头带（页头按钮在
  视口上方，靠豁免区保持可点）；帧首清 `g_hit_clip` 防异常早退残留。
- `Relayout` 额度页 qvp 同矩形：滚出视口的 EDIT 隐藏（原生 EDIT 不受 DC 裁剪）。
- 测试 +5（TestUiLayout）：ClipHitRect 交集/全滚出/空 clip + 裁剪后命中在页头带
  不响应/可见部分正常响应。
- 验证：/W4 零警告；251/251；实机滚轮回归（`build/shot_quota_scroll.ps1` +
  `shot_quota_upscroll.ps1` + `shot_quota_expand_scroll.ps1`）：下滚标题栏/页头完整、
  卡片在页头线下干净裁掉；上滚回顶一致；展开卡原生输入框滚动后与自绘底保持对齐。
  **脚本坑**：WM_MOUSEWHEEL 的 delta 在 wParam **高 16 位**——`[IntPtr](-120)` 会被
  读成 HIWORD=0xFFFF=-1（方向还对但距离错）、`[IntPtr]120` 读成 0（仍判向下），
  必须发 `0x00780000`（+120）/`0xFF880000`（-120）；PowerShell 5 里
  `0xFF880000` 字面量按 Int32 解析为负数，`[uint32]` 转型会炸，走
  `[IntPtr][int64]0xFF880000` 路径。
- **同日用户复检追加两修**：①**贴边描边被裁剪削半**（"Codex 卡右边框显示不全"）——
  右列卡右缘恰好=content.right、首行顶=视口顶（quota_layout 推导），裁剪矩形与卡缘
  相切，GDI+ 描边以路径为中心 → 外半被削剩发丝线（像素实证 `1C2233` 单点）；
  修=绘制裁剪外扩 2 DIP（页头下有 14 DIP 空隙、底缘 24 DIP 边距，越界不可见），
  命中裁剪保持精确视口。凡"裁剪矩形+贴边描边"组合都要外扩约一个描边宽。
  ②**卡级 hover 提亮不及时**——g_cursor_pt 判卡级 hover（09-17 引入）但
  WM_MOUSEMOVE 只在命中 id 变化时 InvalidateRect，光标从空隙直进卡片 body 命中
  id 不变（-1→-1）不重绘，提亮要等下一次命中变化；修=新增 `g_card_hover`
  （光标所在卡可见序，ui_draw 全局），跨卡进出单独判定即失效整帧
  （仅额度页判定，g_qlayout 别页过期）；WM_MOUSELEAVE/RebuildPage/OnDetailClosed
  三处复位。验证用**真实光标**（合成 WM_MOUSEMOVE 必触发 MOUSELEAVE 验证不了），
  SetForegroundWindow 后 SetCursorPos + CopyFromScreen 采样：
  fill `141B2B→182236`、border `1C2233→5E6675` 即时翻转。另注意
  CopyFromScreen 截屏前窗口必须在前台——验证脚本自己的控制台窗口盖上去采到的是
  控制台底色（曾误判 hover 无效）。

## 2026-09-19 第十一轮：动作回显与静默失败治理（251→260 项自测全绿）

用户反馈「点击保存设置没有任何回显」，举一反三排查出九处反馈缺口统一治理：

- **窗口内轻量回显机制**：`g_echo`（单槽消息：kEchoInfo/Ok/Err + until 单调钟）+
  `g_save_fb`（保存键变形态）定义在 ui_draw.cpp；`kTimerEcho`（id=3，200ms 粒度）在
  DetailProc WM_TIMER 驱动过期清态、全部过期停表；`PaintEchoPill`（detail_shell）在
  PaintDetail 末尾、BitBlt 前最上层叠加（**不注册命中**——瞬时提示不劫持下层点击）。
  `UiEcho(hwnd,kind,text)` 一行回显；`TrayUi::ShowEcho` 窗口开着走窗内、关着回落托盘
  气泡（App 层动作经此路由）。OnDetailClosed 清两态（定时器随窗口销毁自清）。
- **保存设置回显（主诉求）**：`UiSaveFeedback` 让保存键点击后 2.2s 变形——成功=绿底
  「已保存」（kSSecCont）/失败=红底「保存失败」（kSErrCont）；间隔/阈值**钳制值保存后
  回写 EDIT**（旧实现框里显示"5"实际已存 60，所见非所存直到页面重建）；自启动注册表
  写入失败在 `CommitConfig` 内**回拍真实开关态**，kWSaveCfg 比对提交前后 autostart
  发现并红字回显（旧实现只写日志，注释声称"提示一次"实则无提示）。
- **落盘失败贯通**：`CommitConfig/ApplyDisplayLive/CommitSlotOffsetDip/SetAccountEnabled/
  Visible/AlertPct/MoveAccountOrder/SetUiThemeMode` 全部改返 bool（config.json 或
  secrets 落盘失败=false；无改动/口径外值=true 不误报），HandleAction 全部调用点失败即
  回显「已应用，但写入配置失败」；`UpdateAccount` 的 config 写失败归入 IoError（codex
  别名-only 分支同样），IoError 弹窗文案改为"磁盘或 DPAPI 加密写入出错"通吃两态。
- **刷新不再静默**：`ManualKick` 七态枚举 + `RequestManual(id,&retry_after_sec)` 返回
  拒绝原因；`SummarizeManualKicks`/`ManualKickEchoText` 纯函数（scheduler，selftest 九例）
  汇总成文案——「已开始刷新 N 张卡片」（附未配置/刚刷过/已停用尾注）、「平台限频中，
  约 N 分钟后可再刷新」（Retry-After 秒→分钟进位）、「刚刚已刷新过」、「请先在卡片设置
  中配置 API Key」、「已在查询中」、「没有可刷新的卡片」。暂停态手动刷新语义不变。
- **复制 Key 回显**：`CopyTextToClipboard` 返 bool（EmptyClipboard/SetClipboardData 失败
  路径补全，失败自释放不泄漏），成功「已复制到剪贴板」/失败「剪贴板被其他程序占用」。
- **卡片保存（kAccSave）**：NoChange 回显「没有改动」（旧实现点击毫无反应）；**清空 Key
  保存=删除密钥，现要求确认**（与删除卡片同级危险操作，旧实现静默直接清掉且不可恢复）；
  成功「已保存」。
- **脏草稿守卫**：`DisplayDraftDirty(h, base)`（display_page.cpp）= 草稿字段（interval/
  阈值/节能/预警/自启动）+ EDIT 实时文本 vs 已提交配置（打字不写草稿所以 EDIT 必须单独
  比对；节能态 EDIT 恒显 900 跳过区间比对防假脏；空框按未改处理）。页脚：脏时琥珀
  「● 有未保存的更改」+「放弃变更」激活，干净态置灰且**不注册命中**；**切页
  （SetPage）/关窗（CloseDetail——X/Esc/WM_CLOSE/托盘 toggle 同径）前 ConfirmDiscardDraft
  确认**（MB_DEFBUTTON2）；退出进程走 Shutdown 直 DestroyWindow 不打扰。
- **清除数据**确认后回显「已清除本地数据，账户已复位为默认卡片」。
- 实机验证（build/verify_echo2.ps1，PrintWindow flags=0 走 WM_PRINT 同步 PaintDetail 才
  能抓到回显瞬态，flags=2 可能给陈旧表面）：保存键绿「已保存」/刷新汇总条（真实 4 账户：
  「已开始刷新 3 张卡片，1 张已停用」）/脏草稿页脚/关窗确认（弹窗枚举断言→IDNO 保窗→
  放弃后干净关闭）全过。
- **坑：脚本按设计稿 780 DIP 估页脚坐标会点空**——窗口外框经 AdjustWindowRectEx 加框后
  SetWindowPos 只缩不放，客户区=整窗矩形（本机实测 ≈818 DIP 高）≠ 设计高；页脚点击一律
  GetClientRect 运行时反推（save=(dipW-75, dipH-26)、discard=(dipW-188, dipH-26)）。

## 2026-11-27 Antigravity 接入（295→339 项自测全绿）

新增第五个数据源 Google Antigravity（经本机 hub,免填 Key）。路线取舍与踩坑见「已知状态」
的 Antigravity 条目和 `experiments/ag_probe/README.md`,此处只记代码落点:

- **`AccountType` 末尾追加 `Antigravity`(=4)**、`kAccountTypeCount` 取代散落的魔数 4
  （provider_icons 定长表、config 类型上界、添加面板 chip 数 `ui::kAddTypeCount`）。
  既有测试夹具是「默认账户集 × `snaps[4]`」按 `accs.size()` 遍历——antigravity 没进默认集,
  所以那些夹具原样有效;以后再往默认集加类型务必同步扩这些数组,否则越界读。
- **`ProviderOps` 表加一行 + 两个列/判定**:`local_hub`（本机 hub 数据源标志,调度器据此
  改走短重探、永不停机）;`build_request` 签名改为带 `local_base` 入参（其余四个忽略）;
  `ProviderSingleCard`/`ProviderAlertCapable`。`ProviderRequestSpec` 新增 `method/body/auth_header`。
- **宿主接口**新增 `ISchedulerHost::GetEndpointBase`（antigravity 返回现场解析的
  `http://127.0.0.1:<httpPort>`,其余返回空）;`App::FindAntigravityHub` 扫四个产品目录的
  `daemon/hub_*.json`。`ManualKick` 新增 `SourceOffline` 态与对应回显文案。
- **`AppendKeyedPercentLine`**（state.cpp 匿名空间）:把 GLM 分支的"按 key 取比例窗口值段"
  抽成带文案参数的共用函数,GLM 与 Antigravity 共用（防第五份复制粘贴漂移;GLM 行为不变,
  由既有 display/embed/ring 用例回归）。`push_ordered_rows` 增加 `max_rank` 参数——
  Antigravity 只取主池两窗进数值区,Claude&GPT 池与未映射池进 ⓘ 注解（此前"其余"窗口会
  作为第三行推进 rows,虽然绘制只画前两格,但语义是错的）。
- **无 Key 类判定收敛**:app/storage/detail_shell/quota_page 里十几处 `type == Codex` /
  `!= Codex` 改查 `ProviderUsesApiKey`/`ProviderSingleCard`/`ProviderAlertCapable`;
  凭证行文案抽成 `LocalCredentialStatus(type, ready, compact)`,安全说明行随类型。
- **顺手修的既有布局问题**（无 Key 卡展开区,原只有 Codex 会走到,从未被仔细看过）:
  「重新读取」按钮原摆在 `ex_key.right` 之外 = 伸出卡片右缘 ~50 DIP（按旧布局矩形推算:
  左列卡的部分被右邻卡盖住、右列卡的部分被窗口右缘裁掉;**没有旧版截图对照,发版时
  展开态的新布局也尚未做可视验证,只过了布局用例**）,现右对齐收进卡内,状态框收窄让位、
  文案分完整/紧凑两档;
  类型章改为"放得下才整章画"——旧阈值 4 DIP 只够剩一个字,标题行一有状态 chip 就画出
  「周…」「本」这类残章（antigravity 标题长,第一眼就是残章）。
- **自测 +44**:`TestAntigravity`（请求规格/分派表/解析器:真机样本、三种信封、零值省略、
  disabled、钳制、未映射池/配置 type=4 往返与未知类型丢弃/显示模型四路/历史采样/快照往返/
  布局回归/刷新回显）+ `TestSchedulerBasics` 新增 `HubHost`（401 不断机、连接被拒 60s 重探、
  坏响应不停机、成功发布快照）。写用例时自己踩的坑:JSON 字面量多一个 `]` 导致解析失败,
  而断言没做长度守卫直接越界 → selftest 段错误;**失败路径上的下标访问一律先验长度**。
- **真机联调结论**:隔离 profile + 真 hub,生产 WinHTTP 路径 200 → 卡片 5h/7d 环、趋势带、
  历史落盘、手动刷新全通;残留描述文件被正确忽略;真实实例上用户经「添加卡片」建卡即查成功。
  未能可视验证的只剩展开态卡片的最终观感（联调时工位锁屏,见探针 README）。
