// disp_layout.cpp — 显示设置页布局实现（自 tray_ui.cpp ComputeDisp 原样迁移；
// 行为逐字节等价，改动须带 selftest 回归依据）。
// 2026-09-21 模式合并重构：4 模式行；外观卡（主题/排版/字号/字色/低额变色/品牌色
// [+嵌入内容]）；位置卡三形态（嵌入滑杆 / 浮条吸附+XY / 圆环吸附偏移）。
#include "disp_layout.hpp"
#include <algorithm>

namespace qb::ui {

DispLayout ComputeDispLayout(const DispPageIn& in, const RECT& content, int dpi,
                             int& scroll_out) {
    auto LS = [dpi](int v) { return v * dpi / 96; };
    DispLayout L{};
    L.embed = in.mode == DisplayMode::Embed;
    L.free = in.mode == DisplayMode::Free;
    L.rings = in.mode == DisplayMode::Rings;
    L.snap = in.snap;
    L.thr_visible = in.notify_enabled;
    L.pos_visible = L.embed || L.free || L.rings; // 仅托盘无位置语义，整区隐藏
    const int cw_full = content.right - content.left;
    const int col_w = std::min(cw_full - LS(4), LS(640));
    const int cx = content.left + std::max(0, (cw_full - col_w) / 2);
    const int vp_top = content.top + LS(52);
    const int vp_bottom = content.bottom - LS(28);
    // 右缘控件基准：距卡右缘 16；标签基准：距卡左缘 20
    const int rx = cx + col_w - LS(16);
    int y = 0;
    int sec_i = 0;
    int card_top = 0;
    auto row = [&](int h) {
        RECT r{cx, vp_top + LS(y), cx + col_w, vp_top + LS(y + h)};
        y += h;
        return r;
    };
    auto begin_card = [&]() {
        L.sec[sec_i] = RECT{cx + LS(4), vp_top + LS(y), cx + col_w, vp_top + LS(y + 20)};
        y += 20 + 8;
        card_top = y;
    };
    auto end_card = [&]() {
        L.card[sec_i] = RECT{cx, vp_top + LS(card_top), cx + col_w, vp_top + LS(y)};
        y += 24;
        ++sec_i;
    };

    // ---- 显示模式（4 行） ----
    begin_card();
    for (int i = 0; i < 4; ++i) L.mode[i] = row(54);
    end_card();

    // ---- 外观 ----
    begin_card();
    {
        RECT r = row(48);
        L.theme_label = r;
        L.theme_seg = RECT{rx - LS(240), r.top + LS(10), rx, r.top + LS(38)};
    }
    if (L.embed) {
        L.embed_note = row(36);
    } else {
        {
            RECT r = row(48);
            L.layout_label = r;
            L.seg_d = RECT{rx - LS(240), r.top + LS(10), rx - LS(122), r.top + LS(38)};
            L.seg_s = RECT{rx - LS(118), r.top + LS(10), rx, r.top + LS(38)};
        }
        {
            RECT r = row(48);
            L.font_label = r;
            L.font_inc = RECT{rx - LS(28), r.top + LS(10), rx, r.top + LS(38)};
            L.font_val = RECT{rx - LS(28) - LS(4) - LS(52), r.top, rx - LS(32),
                              r.bottom};
            L.font_dec = RECT{rx - LS(116), r.top + LS(10), rx - LS(88), r.top + LS(38)};
        }
    }
    {
        RECT r = row(48);
        L.color_label = r;
        L.color_seg = RECT{rx - LS(240), r.top + LS(10), rx, r.top + LS(38)};
    }
    {
        RECT r = row(48);
        L.lowc_label = r;
        L.lowc_toggle = RECT{rx - LS(44), r.top + LS(12), rx, r.top + LS(36)};
    }
    {
        RECT r = row(48);
        L.brand_label = r;
        L.brand_toggle = RECT{rx - LS(44), r.top + LS(12), rx, r.top + LS(36)};
    }
    {
        RECT r = row(48);
        L.embicon_label = r;
        L.embicon_toggle = RECT{rx - LS(44), r.top + LS(12), rx, r.top + LS(36)};
    }
    if (L.embed) {
        RECT r = row(48);
        L.embname_label = r;
        L.embname_toggle = RECT{rx - LS(44), r.top + LS(12), rx, r.top + LS(36)};
        r = row(48);
        L.embwin_label = r;
        L.embwin_toggle = RECT{rx - LS(44), r.top + LS(12), rx, r.top + LS(36)};
    }
    end_card();

    // ---- 位置微调（仅托盘整区隐藏） ----
    if (L.pos_visible) {
        begin_card();
        if (L.embed) {
            RECT r = row(48);
            L.big_val = r;
            L.b_zero = RECT{rx - LS(64), r.top + LS(9), rx, r.top + LS(39)};
            L.slider = row(40);
            L.kbd_line = RECT{cx + LS(20), vp_top + LS(y), cx + col_w - LS(16),
                              vp_top + LS(y + 22)};
            y += 22 + 6;
        } else {
            if (L.free) {
                RECT r = row(48);
                L.snap_row = r;
                L.snap_toggle = RECT{rx - LS(44), r.top + LS(12), rx, r.top + LS(36)};
            }
            {
                RECT r = row(48);
                L.x_label = r;
                L.x_val = RECT{rx - LS(96), r.top, rx, r.bottom};
            }
            L.x_slider = row(40);
            if (L.free && !L.snap) {
                RECT r = row(48);
                L.y_label = r;
                L.y_val = RECT{rx - LS(96), r.top, rx, r.bottom};
                L.y_slider = row(40);
            }
            L.pos_hint = RECT{cx + LS(20), vp_top + LS(y), cx + col_w - LS(16),
                              vp_top + LS(y + 22)};
            y += 22 + 6;
        }
        end_card();
    }

    // ---- 刷新与提醒 ----
    begin_card();
    {
        RECT r = row(48);
        L.int_label = r;
        L.int_edit = RECT{rx - LS(28) - LS(64), r.top + LS(10), rx - LS(28),
                          r.top + LS(38)};
    }
    L.int_slider = row(38);
    {
        RECT r = row(48);
        L.row_eco = r;
        L.eco_toggle = RECT{rx - LS(44), r.top + LS(12), rx, r.top + LS(36)};
    }
    {
        RECT r = row(48);
        L.row_notify = r;
        L.notify_toggle = RECT{rx - LS(44), r.top + LS(12), rx, r.top + LS(36)};
    }
    if (L.thr_visible) {
        RECT r = row(48);
        L.thr_k = r;
        L.thr_k_edit = RECT{rx - LS(28) - LS(64), r.top + LS(10), rx - LS(28),
                            r.top + LS(38)};
        r = row(48);
        L.thr_d = r;
        L.thr_d_edit = RECT{rx - LS(28) - LS(64), r.top + LS(10), rx - LS(28),
                            r.top + LS(38)};
    }
    end_card();

    // ---- 启动与数据 ----
    begin_card();
    {
        RECT r = row(48);
        L.row_auto = r;
        L.auto_toggle = RECT{rx - LS(44), r.top + LS(12), rx, r.top + LS(36)};
    }
    {
        RECT r = row(48);
        L.row_clear = r;
        L.clear_btn = RECT{rx - LS(112), r.top + LS(9), rx, r.top + LS(39)};
    }
    end_card();

    L.content_bottom = vp_top + LS(y);
    L.viewport_bottom = vp_bottom;
    // 滚动钳制后整体上移（与 ComputeQuota 同口径）
    const int max_scroll = std::max(0, L.content_bottom + LS(12) - L.viewport_bottom);
    scroll_out = std::min(std::max(0, in.scroll), max_scroll);
    const int dy = -scroll_out;
    auto shift = [dy](RECT& r) {
        if (r.right > r.left || r.bottom > r.top) { r.top += dy; r.bottom += dy; }
    };
    for (int i = 0; i < 5; ++i) {
        shift(L.sec[i]);
        shift(L.card[i]);
    }
    for (int i = 0; i < 4; ++i) shift(L.mode[i]);
    shift(L.theme_label); shift(L.theme_seg); shift(L.embed_note);
    shift(L.layout_label); shift(L.seg_d); shift(L.seg_s);
    shift(L.font_label); shift(L.font_dec); shift(L.font_inc); shift(L.font_val);
    shift(L.color_label); shift(L.color_seg);
    shift(L.lowc_label); shift(L.lowc_toggle);
    shift(L.brand_label); shift(L.brand_toggle);
    shift(L.embicon_label); shift(L.embicon_toggle);
    shift(L.embname_label); shift(L.embname_toggle);
    shift(L.embwin_label); shift(L.embwin_toggle);
    shift(L.big_val); shift(L.b_zero); shift(L.slider); shift(L.kbd_line);
    shift(L.snap_row); shift(L.snap_toggle);
    shift(L.x_label); shift(L.x_val); shift(L.x_slider);
    shift(L.y_label); shift(L.y_val); shift(L.y_slider);
    shift(L.pos_hint);
    shift(L.int_label); shift(L.int_edit); shift(L.int_slider);
    shift(L.row_eco); shift(L.eco_toggle);
    shift(L.row_notify); shift(L.notify_toggle);
    shift(L.thr_k); shift(L.thr_k_edit); shift(L.thr_d); shift(L.thr_d_edit);
    shift(L.row_auto); shift(L.auto_toggle);
    shift(L.row_clear); shift(L.clear_btn);
    return L;
}

} // namespace qb::ui
