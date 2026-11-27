// quota_layout.cpp — 额度页布局实现（自 tray_ui.cpp ComputeQuota 原样迁移；
// 行为逐字节等价，改动须带 selftest 回归依据）。
#include "quota_layout.hpp"
#include <algorithm>

namespace qb::ui {

namespace {
inline RECT RC(int x, int y, int w, int h) { return RECT{x, y, x + w, y + h}; }
} // namespace

QuotaLayout ComputeQuotaLayout(const QuotaPageIn& in, const RECT& content, int dpi,
                               int& scroll_out) {
    auto LS = [dpi](int v) { return v * dpi / 96; };
    QuotaLayout L;
    const int x0 = content.left, cw = content.right - content.left;
    const int y0 = content.top;
    const auto& accs = in.cards;
    L.empty_state = accs.empty();

    // 页头固定控件（不随内容滚动；右侧统计文本块由 paint 从 refresh_btn 派生）
    L.add_btn = RC(content.right - LS(124), y0 + LS(2), LS(124), LS(36));
    L.refresh_btn = RC(L.add_btn.left - LS(10) - LS(36), y0 + LS(2), LS(36), LS(36));
    for (size_t i = 0; i < accs.size(); ++i)
        if (!accs[i].visible) L.hidden_idx.push_back((int)i);
    if (!L.hidden_idx.empty())
        L.hidden_chip = RC(L.refresh_btn.left - LS(10) - LS(112), y0 + LS(3), LS(112),
                           LS(34));
    L.has_hidden_chip = !L.hidden_idx.empty();
    // 右上统计块右界：有隐藏 chip 时让出（chip 后画，重叠 = 盖住「计费中」文字右半，
    // 2026-09-22 用户实测）；无 chip 时维持贴刷新按钮左缘的原位
    const int stats_right = L.has_hidden_chip ? L.hidden_chip.left - LS(10)
                                              : L.refresh_btn.left - LS(14);
    L.stats_block = RC(stats_right - LS(190), y0 + LS(1), LS(190), LS(33));
    // 可见/隐藏分组
    for (size_t i = 0; i < accs.size(); ++i)
        if (accs[i].visible) L.vis_idx.push_back((int)i);

    // ---- 添加面板（展开时占页头下方）----
    int after_head = y0 + LS(52); // 页头（标题行 + 间距）
    if (in.add_open) {
        L.add_panel = RC(x0, after_head, cw, 0);
        const int px = L.add_panel.left + LS(20);   // 面板内边距：chips/文案不贴圆角边
        const int pw = cw - LS(80);
        int py = L.add_panel.top + LS(14);
        py += LS(26) + LS(10);                       // 面板标题行
        const int chip_w = (pw - LS(6) * (kAddTypeCount - 1)) / kAddTypeCount;
        for (int i = 0; i < kAddTypeCount; ++i)
            L.ap_type[i] = RC(px + i * (chip_w + LS(6)), py, chip_w, LS(34));
        py += LS(34) + LS(12);
        L.ap_alias = RC(px + LS(52), py, pw - LS(52), LS(34));
        py += LS(34) + LS(8);
        L.ap_key = RC(px + LS(52), py, pw - LS(52), LS(36));
        L.ap_key_eye = RC(L.ap_key.right - LS(10) - LS(28), py + LS(4), LS(28), LS(28));
        L.ap_key_copy = RC(L.ap_key_eye.left - LS(2) - LS(28), py + LS(4), LS(28), LS(28));
        L.ap_key_clear = RC(L.ap_key_copy.left - LS(2) - LS(28), py + LS(4), LS(28), LS(28));
        py += LS(36) + LS(8);
        L.ap_error = RC(px + LS(52), py, pw - LS(52), LS(18));
        py += LS(18) + LS(8);
        const int foot_h = LS(34);
        L.ap_create = RC(px + pw - LS(96), py, LS(96), foot_h);
        L.ap_cancel = RC(L.ap_create.left - LS(8) - LS(76), py, LS(76), foot_h);
        L.ap_console = RC(px, py + LS(8),
                          std::max(LS(120), (int)L.ap_cancel.left - LS(16) - px), LS(18));
        py += foot_h + LS(14);
        L.add_panel.bottom = L.add_panel.top + (py - L.add_panel.top);
        after_head = L.add_panel.bottom + LS(14);
    }

    // ---- 已隐藏区 ----
    if (in.hidden_open && !L.hidden_idx.empty()) {
        L.hidden_box = RC(x0, after_head, cw, LS(10));
        int hy = L.hidden_box.top + LS(10);
        for (size_t i = 0; i < L.hidden_idx.size(); ++i) {
            L.hidden_rows.push_back(RC(x0 + LS(8), hy, cw - LS(16), LS(40)));
            hy += LS(40) + LS(6);
        }
        L.hidden_box.bottom = hy + LS(6);
        after_head = L.hidden_box.bottom + LS(12);
    }

    // ---- 卡片网格（2 列；行高=两卡较大值，顶对齐；展开卡更高；有趋势数据的卡更高一档）----
    // 结构（stitch，DIP）：p=24 内边距；头行 40；+20；数值区 64；+20；
    //（趋势带 122 + 20）+ 页脚行 26 + 24 底边距。
    const int gap = LS(20);
    const int card_w = (cw - gap) / 2;
    const size_t n = L.vis_idx.size();
    L.cards.resize(n);
    L.cr.resize(n);
    int ry = after_head;
    for (size_t i = 0; i < n;) {
        const size_t j = (i + 1 < n) ? i + 1 : i;
        const int p = LS(24);
        for (size_t k = i; k <= j; ++k) {
            const int cx0 = x0 + (k % 2) * (card_w + gap);
            const int cright = cx0 + card_w;
            QuotaCardR& r = L.cr[k];
            const int ai = L.vis_idx[k];
            const bool has_trend = accs[ai].has_trend;
            const int stats_top = ry + p + LS(40) + LS(20);
            const int trend_top = stats_top + LS(64) + LS(20);
            const int foot_top = has_trend ? trend_top + LS(122) + LS(20)
                                           : stats_top + LS(64) + LS(20);
            const int base_h = (foot_top - ry) + LS(14) + LS(26) + LS(24);
            L.cards[k] = RC(cx0, ry, card_w, base_h);
            r.expand_h = 0;
            // 含计算端点的矩形一律 brace-init（RC 第三/四参是宽高，传绝对坐标即炸版式）
            r.stats = RECT{cx0 + p, stats_top, cright - p, stats_top + LS(64)};
            if (has_trend) r.trend = RECT{cx0 + p, trend_top, cright - p, trend_top + LS(122)};
            r.toggle = RC(cright - p - LS(44), ry + p + LS(8), LS(44), LS(24));
            r.eye = RC(r.toggle.left - LS(6) - LS(28), ry + p + LS(8), LS(28), LS(28));
            r.settings_btn = RC(cright - p - LS(76), foot_top + LS(14), LS(76), LS(26));
            if (accs[ai].expanded) {
                // 展开区起点：页脚行之下，杜绝叠压
                int ey = r.settings_btn.bottom + LS(16);
                r.ex_alias = RECT{cx0 + p + LS(52), ey, cright - p, ey + LS(34)};
                ey += LS(34) + LS(8);
                // 无 Key 类凭证行:右端预留「重新读取」按钮（88 宽 + 10 间距）,状态框收窄让位。
                // 旧写法把按钮摆在 ex_key.right 之外 = 卡片右内边距外侧,画出卡外 ~74 DIP
                //（左列卡压住右邻卡、右列卡被窗口右缘裁掉）——2026-11-27 接入 antigravity 时修正。
                const int sync_reserve = accs[ai].uses_api_key ? 0 : LS(88) + LS(10);
                r.ex_key = RECT{cx0 + p + LS(52), ey, cright - p - sync_reserve, ey + LS(36)};
                r.ex_key_eye = RC(r.ex_key.right - LS(10) - LS(28), ey + LS(4), LS(28),
                                  LS(28));
                r.ex_key_copy = RC(r.ex_key_eye.left - LS(2) - LS(28), ey + LS(4), LS(28),
                                   LS(28));
                r.ex_key_clear = RC(r.ex_key_copy.left - LS(2) - LS(28), ey + LS(4), LS(28),
                                    LS(28));
                ey += LS(36) + LS(8);
                if (accs[ai].alert_capable) {
                    r.ex_alert = RECT{cx0 + p + LS(96), ey, cright - p, ey + LS(30)};
                    ey += LS(30) + LS(8);
                }
                // 安全说明行占说明行高 18 + 间距（与 PaintQuotaCard 的 note_y 同游标）
                const int btn_y = ey + LS(18) + LS(8);
                r.ex_delete = RC(cx0 + p, btn_y, LS(96), LS(34));
                r.ex_save = RC(cright - p - LS(96), btn_y, LS(96), LS(34));
                if (!accs[ai].uses_api_key)
                    r.ex_codex_sync = RC(cright - p - LS(88), r.ex_key.top + LS(4), LS(88),
                                         LS(28));
                r.expand_h = btn_y + LS(34) + LS(6) - (ry + base_h);
                L.cards[k].bottom = ry + base_h + r.expand_h;
            }
        }
        const int row_h = std::max(L.cards[i].bottom, L.cards[j].bottom) - ry;
        for (size_t k = i; k <= j; ++k) L.cards[k].bottom = ry + row_h; // 行内等高、顶对齐
        ry += row_h + gap;
        i = j + 1;
    }
    L.content_bottom = ry;
    L.viewport_bottom = content.bottom;
    // 滚动钳制后整体上移（页头按钮除外；hits 由 paint 同坐标注册）
    const int max_scroll = std::max(0, L.content_bottom + LS(8) - L.viewport_bottom);
    scroll_out = std::min(std::max(0, in.scroll), max_scroll);
    const int dy = -scroll_out;
    auto shift = [dy](RECT& r) {
        if (r.right > r.left || r.bottom > r.top) { r.top += dy; r.bottom += dy; }
    };
    shift(L.add_panel);
    for (int i = 0; i < kAddTypeCount; ++i) shift(L.ap_type[i]);
    shift(L.ap_alias); shift(L.ap_key);
    shift(L.ap_key_eye); shift(L.ap_key_copy); shift(L.ap_key_clear);
    shift(L.ap_error); shift(L.ap_create); shift(L.ap_cancel); shift(L.ap_console);
    shift(L.hidden_box);
    for (auto& r : L.hidden_rows) shift(r);
    for (auto& c : L.cards) shift(c);
    for (auto& r : L.cr) {
        shift(r.toggle); shift(r.eye); shift(r.note); shift(r.settings_btn);
        shift(r.stats);
        shift(r.ex_alias); shift(r.ex_key); shift(r.ex_alert);
        shift(r.ex_key_eye); shift(r.ex_key_copy); shift(r.ex_key_clear);
        shift(r.ex_save); shift(r.ex_delete); shift(r.ex_codex_sync);
        shift(r.trend);
    }
    return L;
}

} // namespace qb::ui
