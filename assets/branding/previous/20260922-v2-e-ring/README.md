# QuotaBar 图标

设计（2026-09-22，exploration-v3 E 方向「三段环」转正，同日 E2 矢量精修）：透明底上的
粗弧段圆环——上半 180° 绿弧（#1FB583）+ 下半两段蓝色弧（#2B5BC7）+ 圆心指示点，
三个等宽缺口分隔；抽象表达「多窗口额度 + 当前窗口」，与应用圆环呈现模式同源。
无底片、纯平色、无渐变。来源：SenseNova u1.5-lite 白底出图选型（E），后按反馈
（圆心中部空洞、绿色过艳）由 exploration-v3/refine_e2.py 矢量重绘精修定型。
原始提示词见 prompt.txt，探索与精修过程见 exploration-v3/。

## 文件

- `quotabar-master.png`：1024×1024 RGBA 官方母版。
- `../quota_bar.ico`：16 个 32-bit RGBA 帧；256 像素帧采用 PNG，其余采用 DIB。
- `png/quotabar-{size}.png`：18 种透明 PNG：16、20、24、28、30、32、36、40、48、60、64、72、80、96、128、256、512、1024。
- `preview.png`：浅色/深色底实际像素尺寸对照（16–128）。

## 用途

16–32：系统托盘、小图标与常见 DPI；24–48：标题栏与任务栏；48–256：快捷方式、文件列表和大图标；512/1024：宣传及后续素材源。当前项目为 Windows Win32 应用，不需要 Android/iOS/macOS 打包资源。

## 再次导出

在项目根目录执行 `powershell -NoProfile -ExecutionPolicy Bypass -File tools/gen_icon.ps1`。不调用在线服务，直接从母版重建所有 PNG 和 ICO（脚本需 UTF-8 BOM，已在 2026-09-22 补上，否则 Windows PowerShell 5.1 按 ANSI 解码中文注释会吞行报错）。

程序资源 ID 保持 102。标题栏、窗口类与托盘均使用该资源；修改不会改变额度业务逻辑。上一版（Q 形额度环 + 深蓝底片）完整备份在 `previous/20260922-v1-q-ring/`，含当时的母版、ICO、PNG 与 prompt.txt，可用于恢复。
