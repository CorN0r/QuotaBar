# slot_probe — 「点开开始菜单后嵌入条跳到任务栏右侧」定位实验(2026-09-06)

## 结论(实机复现 + 截图/转储三重验证)

**根因:开始菜单打开期间,Win11 XAML 任务栏把左簇内容(含类名 `Start` 的开始按钮
HWND)从 `Shell_TrayWnd` 的子窗口树中摘走(换挂到 Start 体验宿主),`EnumChildWindows`
枚举不到 `Start` → `TaskbarSlot::ClusterLeft()`(src/taskbar_slot.cpp:969)失败 →
`RefreshGeometry`(src/taskbar_slot.cpp:564)锚定链回退到 ② `ClusterRight` →
条落到「任务按钮簇右缘 ~958 逻辑px + gap」,即用户看到的"任务栏右侧"。
关闭开始菜单后子树挂回,1s `Poll`(app.cpp:553)重算,条回到左侧。全程无降级日志
(空间足够,纯位移)。**

## 证据链

1. **关闭态基线(tray_closed.txt)**:`Start` 在 469,992,514,1040(vis=False 但可枚举,
   ClusterLeft 不查可见性所以工作);嵌入条 `QuotaBarSlotWnd` 在 121,1000,344,1032;
   `TrayNotifyWnd` 1337..1560;`ReBarWindow32/MSTaskSwWClass` 469..958。
2. **打开态(截图 11:07/11:13)**:开始菜单展开,嵌入条视觉跳到 x≈790 截图px =
   964 逻辑px = 簇右缘 958 + gap 6,与 ClusterRight 分支计算完全吻合。
3. **日志反推(quota_1.log)**:打开期间 fetch 触发的 `布局 avail=734` 物理px,
   精确等于 ClusterRight+真实托盘锚(2674-12-1928=734),证明该时段 `Start` 缺席、
   `TrayNotifyWnd` 在场。
4. **打开态转储(tray_open.txt 等)**:`Start`/`TrayNotifyWnd`/`ReBarWindow32`/
   `DesktopWindowContentBridge` 轮流从 EnumChildWindows 视野消失(换挂 churn,
   不同时刻快照不同;FindWindowExW 按类名仍能查到嵌入条,EnumChildWindows 却
   枚举不到,子窗口链处于非常规状态)。
5. **恢复(11:16 dismissed)**:完整子树回来,嵌入条同一 HWND(2032388)回到
   121,1000。

## 修复方向(未实施,按推荐排序)

1. **粘性锚点(推荐)**:ClusterLeft 成功时缓存 `Start` 矩形;查找失败但宿主仍活时
   沿用缓存锚(而非顺链回退),在 TaskbarCreated/DisplayChange/换宿主时清缓存。
   同类 flyout(搜索/任务视图/通知中心,同机制)一并覆盖。
2. 失败时冻结位置:ClusterLeft 失败时保持上次落位不重排 —— 会牺牲真·无开始按钮
   场景的正确回退,不如 1。
3. 检测 Start 菜单打开态暂停重排 —— 脆弱,依赖 Shell 内部窗口,不推荐。

## 修复验证(2026-09-06,同日)

修复 = `ClusterLeft` 粘性锚(`src/taskbar_slot.cpp`):现场枚举成功刷新
`start_hwnd_`/`start_cached_` 缓存;现场失败(摘挂 churn)时句柄仍活用其实时矩形、
句柄亡退缓存矩形,均按宿主几何+垂直重叠校验;`TryAttachHost`/`OnTaskbarCreated`
清缓存。build.bat 构建零警告,selftest 101/101 通过。

实机复测:`probe_open.ps1` 触发打开态(转储呈缩减树签名,与修复前 11:07 一致),
`probe_rect.ps1` 实测嵌入条矩形 **121,1000,344,1032**,与关闭态 `fixed_closed.txt`
完全一致——不再跳右。探针脚本可直接复跑复验。

## 附带发现

- 打开态 churn 同样波及 `TrayNotifyWnd`(回退链 ③ 的锚点),槽宽会随之变化;
  粘性锚点方案应覆盖整条锚定链(Start/簇/托盘)而非只缓存 `Start`。
- PowerShell 5.1 按 ANSI 解析 .ps1,UTF-8 中文串会坏(与 CLAUDE.md build.bat 同类坑),
  本目录脚本全部 ASCII。
- 探针对本进程 DPI 无感知,转储坐标为逻辑 px(物理 = 逻辑 × 2,本机 200%);
  QuotaBar 为 PMv2 走物理 px,对比时注意换算。
