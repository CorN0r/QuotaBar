# 验证记录

2026-09-23（晚）：v4.4「渐变闭环」替换（未加工原稿直出）。
- 母版 = exploration-v4/source-raw-gradient-ring-1024.png 原样拷贝（1024×1024，
  完整闭环无缺口/灰轨/绿点，对角渐变蓝 #4C7EF9 → 紫 #B43BFB，占画布 ~87%；
  SHA256 与 D:\Codex 原稿一致）。
- ICO 16 帧解码/长度校验通过；18 种 PNG 与 preview.png 重建，16–128px 深浅双底
  目检合格。
- build.bat 构建成功；--selftest 295/295 全过（exit 0）；ExtractAssociatedIcon
  确认 exe 内嵌为渐变闭环图标。
- 上一版（v4.3 balanced raw）完整备份：previous/20260923-v4.3-balanced-raw/。
- release\ 与 dist 发行包未替换，随下次发版更新。

2026-09-23：v4.3「balanced」变体替换（未加工原稿直出）。
- 母版 = exploration-v4/source-raw-balanced-1024.png 原样拷贝（1024×1024，四边距
  72px 居中，内容占 ~86%；SHA256 与 D:\Codex 原稿一致）。环身粗细介于 standard
  与 bold 之间。
- ICO 16 帧解码/长度校验通过；18 种 PNG 与 preview.png 重建，16–128px 深浅双底
  目检合格（绿点 16px 仍可辨）。
- build.bat 构建成功；--selftest 295/295 全过（exit 0）；ExtractAssociatedIcon
  确认 exe 内嵌为 balanced 版图标。
- 上一版（v4.2 加粗 raw 直出）完整备份：previous/20260923-v4.2-bold-raw/。
- release\ 与 dist 发行包未替换，随下次发版更新。

2026-09-22（晚三）：按用户要求，母版改用**未加工原稿直出**。
- 母版 = exploration-v4/source-raw-bold-1024.png 原样拷贝（1024×1024，未经
  process_bold.py 清理；噪点/品红描边按用户决定保留原样）。
- ICO 16 帧解码/长度校验通过；18 种 PNG 与 preview.png 重建，16–128px 深浅双底
  目检与加工版肉眼无差。
- build.bat 构建成功；--selftest 295/295 全过（exit 0）；ExtractAssociatedIcon
  确认 exe 内嵌为原稿直出版图标。
- 上一版（v4.1 加工版）完整备份：previous/20260922-v4.1-bold-processed/。
- release\ 与 dist 发行包未替换，随下次发版更新。

2026-09-22（晚二）：v4.1「渐变进度环」加粗定稿替换。
- 原稿：Codex 出图 1024×1024 居中（exploration-v4/source-raw-bold-1024.png，standard/
  fuller 备选同目录存档）；process_bold.py 清理 alpha<64 噪点 + MinFilter 腐蚀 1px 去贴边
  品红色差描边（边缘 mask 检测确认 1368 px → 清理后可见品红 0）+ 高斯回软，RGB 不动。
- ICO 16 帧解码/长度校验通过；18 种 PNG 与 preview.png 重建，16–128px 深浅双底目检合格。
- build.bat 构建成功；--selftest 295/295 全过（exit 0）。
- ExtractAssociatedIcon 从 build\QuotaBar.exe 提取 32px 帧确认为加粗版新图标。
- 上一版（v4.0 初版，环身较细）完整备份：previous/20260922-v4-gradient-ring/。
- release\ 与 dist 发行包未替换，随下次发版更新。

2026-09-22（晚）：v4「渐变进度环」全量替换。
- 原稿：Codex 出图 1254×1254 透明底（exploration-v4/source-raw-1254.png）；process.py
  清理 alpha<64 噪点（约 1.6 万杂质像素）并按内容框裁剪居中 → 1024×1024 母版。
- ICO 16 帧全部解码/长度校验通过，16px 帧四角透明；18 种 PNG 已重建；
  preview.png 重建（16–128 实际像素、深浅双底）。
- build.bat 构建成功；--selftest 295/295 全过（exit 0）。
- ExtractAssociatedIcon 从 build\QuotaBar.exe 提取 32px 帧确认为新图标
  （experiments/exe_icon_extracted.png）。
- 上一版（E 三段环）完整备份：previous/20260922-v2-e-ring/。
- release\ 与 dist 发行包未替换，随下次发版更新。

2026-09-21：图标资源与必要引用已接入。
- 母版 1254×1254，RGBA，Alpha 范围 0–255。
- ICO 16 帧均解码通过，四角透明；18 种 PNG 已导出。
- 已检查 16–128px 深色和浅色背景预览。
- MSVC 构建成功，未报告编译警告；260 项现有自测全过。
- 已直接读取构建 EXE 的资源表：ID 102 包含 16 帧。
- 验证版：dist/icon-preview/QuotaBar.exe。运行中的旧程序、正式发布包未替换。
- 未启动新版本进行真实桌面 UI 验收。退出旧程序后可启动验证版查看效果。
