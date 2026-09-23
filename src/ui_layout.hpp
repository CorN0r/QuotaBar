// ui_layout.hpp — tray_ui 的纯布局/命中测试逻辑（无窗口依赖、无全局状态）。
// 2026-09-18 从 tray_ui.cpp 抽出：滑杆取值/拖拽落位/命中测试核心是事故高发区
// （CLAUDE.md 记录的滑杆落盘竞态、kWLink0 判界、滚动误触均在此），抽成无副作用
// 纯函数后 selftest 可直接覆盖，也为 tray_ui God Module 后续拆分打测试地基。
// tray_ui 侧保留同名薄包装（喂入 g_layout_dpi / g_qlayout / g_hits），调用点零改动。
#pragma once
#include <windows.h>
#include <vector>

namespace qb::ui {

// 命中区域项（tray_ui 的 g_hits 元素类型）
struct HitW {
    RECT rc;
    int id;
};

// 滑杆：客户区 x → [vmin, vmax]。轨道左右各内缩 7 DIP（把手半径），dpi 参与换算；
// 轨道退化（宽 ≤ 内缩）返回 vmin。越界 x 钳到端点。
int SliderValueFrom(const RECT& rc, int vmin, int vmax, int x, int dpi);

// 卡片拖拽排序落位点（F3）：按其余卡片中线判定插入槽（同行内再按水平中线分左右）。
// cards=卡片矩形（客户区物理像素，与 ComputeQuota 同口径），from_vis=拖动源可见序；
// 拖到内容区空白=移到末位（自然推出）；非法源序返回 -1。
int ComputeDragTarget(POINT pt, int from_vis, const std::vector<RECT>& cards);

// 命中测试核心（tray_ui::HitAt 的无全局版）：
//  - 命中列表顺序即优先级：先注册者胜（重叠区域由注册次序决定）
//  - content 非空时：与内容区有交集的命中须同时与视口 vp（或豁免区 extra）相交，
//    已滚出视口的列表项不可误触；与内容区无交集 = 侧栏/标题栏 chrome，恒可点
//  - content 为空（right<=left）= 不限制，一切命中恒可点
//  - 未命中返回 -1
int HitTestAt(const std::vector<HitW>& hits, POINT pt, const RECT& content,
              const RECT& vp, const RECT& extra);

// 命中注册裁剪（2026-09-19 额度页滚动覆盖标题栏事故）：返回 rc ∩ clip，
// 使命中区与 DC 裁剪后的可见部分一致（滚到页头/标题栏底下的卡片不再留幽灵点击带）。
// clip 为空（right<=left）或无交集返回空矩形——调用方据此跳过注册。
RECT ClipHitRect(const RECT& rc, const RECT& clip);

} // namespace qb::ui
