# QuotaBar 图标

设计（2026-09-23，v4.4「渐变闭环」）：透明底上的**完整粗圆环**（无缺口、无轨段、
无指示点），对角连续渐变：右上蓝（#4C7EF9）→ 左下紫（#B43BFB）。平面矢量、无底片、
无阴影高光。语义从 v4.0–v4.3 的「消耗进度（弧段+灰轨+绿点）+ 当前窗口」简化为
纯环标志。迭代史：初版（v4.0，环身较细）→ 加粗（v4.1，占 80%）→ balanced
（v4.3，占 86%）→ gradient-ring 完整闭环（占 ~87%，边距 L/R 63px、T 72px、B 66px）。
来源：Codex 出图，**未加工原稿直出**（exploration-v4/source-raw-gradient-ring-1024.png；
process_bold.py 清理链路仅保留备查，不再用于母版）。等效生成提示词见 prompt.txt。

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
当前母版为未加工原稿直出；上一版（v4.3 balanced raw）备份在
`previous/20260923-v4.3-balanced-raw/`；v4.2 加粗 raw 在
`previous/20260923-v4.2-bold-raw/`；加工版（清噪 + 去品红描边）在
`previous/20260922-v4.1-bold-processed/`；v4.0 渐变环初版（环身较细）在
`previous/20260922-v4-gradient-ring/`；E 方向「三段环」在
`previous/20260922-v2-e-ring/`；更早的 Q 形额度环 + 深蓝底片在
`previous/20260922-v1-q-ring/`，可用于恢复。
