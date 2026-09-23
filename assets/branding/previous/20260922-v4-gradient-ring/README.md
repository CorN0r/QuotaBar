# QuotaBar 图标

设计（2026-09-22 晚，v4「渐变进度环」）：透明底上的粗进度环——主弧约 290°，正上方
圆头起，顺时针经右侧、底部到 8 点位附近；颜色沿弧连续渐变：顶部长春花蓝（#5A80FA）
→ 右侧蔚蓝 → 左下鲜艳紫（#A250FA）。缺口在左上：一段深灰剩余轨（#383E4E，贴圆头侧）
+ 一颗薄荷绿指示点（#3CE6C2，约 140° 方向，标记"当前位置"）。抽象表达「额度消耗
进度 + 当前窗口」，与应用圆环呈现模式同源。无底片、平面矢量、除主弧渐变外无阴影高光。
来源：Codex 出图（原稿 1254×1254，exploration-v4/source-raw-1254.png），
exploration-v4/process.py 清理 alpha 噪点并居中定型为 1024 母版。等效生成提示词见
prompt.txt。

## 文件

- `quotabar-master.png`：1024×1024 RGBA 官方母版。
- `../quota_bar.ico`：16 个 32-bit RGBA 帧；256 像素帧采用 PNG，其余采用 DIB。
- `png/quotabar-{size}.png`：18 种透明 PNG：16、20、24、28、30、32、36、40、48、60、64、72、80、96、128、256、512、1024。
- `preview.png`：浅色/深色底实际像素尺寸对照（16–128）。

## 用途

16–32：系统托盘、小图标与常见 DPI；24–48：标题栏与任务栏；48–256：快捷方式、文件列表和大图标；512/1024：宣传及后续素材源。当前项目为 Windows Win32 应用，不需要 Android/iOS/macOS 打包资源。

## 再次导出

在项目根目录执行 `powershell -NoProfile -ExecutionPolicy Bypass -File tools/gen_icon.ps1`。不调用在线服务，直接从母版重建所有 PNG 和 ICO（脚本需 UTF-8 BOM，已在 2026-09-22 补上，否则 Windows PowerShell 5.1 按 ANSI 解码中文注释会吞行报错）。

程序资源 ID 保持 102。标题栏、窗口类与托盘均使用该资源；修改不会改变额度业务逻辑。
上一版（E 方向「三段环」：绿弧 + 双蓝弧 + 圆心指示点，纯平色）完整备份在
`previous/20260922-v2-e-ring/`，含当时的母版、ICO、PNG、preview、prompt 与 README；
更早的 Q 形额度环 + 深蓝底片在 `previous/20260922-v1-q-ring/`，可用于恢复。
