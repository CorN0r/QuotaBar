// ui_layout.cpp — 纯布局/命中测试实现（与 tray_ui 抽出前逐行为等价；改动需带回归依据）。
#include "ui_layout.hpp"
#include <algorithm>

namespace qb::ui {

int SliderValueFrom(const RECT& rc, int vmin, int vmax, int x, int dpi) {
    int inset = 7 * dpi / 96;
    int track_l = rc.left + inset, track_r = rc.right - inset;
    if (track_r <= track_l) return vmin;
    int t = (x - track_l) * (vmax - vmin) / (track_r - track_l);
    return std::min(std::max(vmin, vmin + t), vmax);
}

int ComputeDragTarget(POINT pt, int from_vis, const std::vector<RECT>& cards) {
    if (from_vis < 0 || from_vis >= (int)cards.size()) return -1;
    int slot = 0;
    int j = 0;
    for (int i = 0; i < (int)cards.size(); ++i) {
        if (i == from_vis) continue;
        const RECT& c = cards[i];
        bool after;
        if (pt.y >= c.top && pt.y < c.bottom)
            after = pt.x >= (c.left + c.right) / 2; // 同行：水平中线分左右
        else
            after = pt.y >= (c.top + c.bottom) / 2;
        if (after) slot = j + 1;
        ++j;
    }
    return slot;
}

int HitTestAt(const std::vector<HitW>& hits, POINT pt, const RECT& content,
              const RECT& vp, const RECT& extra) {
    for (const auto& h : hits) {
        if (!PtInRect(&h.rc, pt)) continue;
        if (content.right > content.left) {
            RECT ix{};
            if (IntersectRect(&ix, &h.rc, &content) != FALSE) {
                const RECT& clip = vp.right > vp.left ? vp : content;
                bool in_vp = IntersectRect(&ix, &h.rc, &clip) != FALSE;
                bool in_extra = extra.right > extra.left &&
                                IntersectRect(&ix, &h.rc, &extra) != FALSE;
                if (!in_vp && !in_extra) continue; // 已滚出视口的列表项不可点
            }
            // 与内容区无交集 = 侧栏/标题栏 chrome，恒可点
        }
        return h.id;
    }
    return -1;
}

RECT ClipHitRect(const RECT& rc, const RECT& clip) {
    RECT ix{};
    if (clip.right <= clip.left || clip.bottom <= clip.top) return ix;
    IntersectRect(&ix, &rc, &clip); // 无交集时 IntersectRect 置空矩形并返回 FALSE
    return ix;
}

} // namespace qb::ui
