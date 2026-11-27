// quota_page.cpp — 额度页绘制（趋势图 + 添加面板文案 + ComputeQuota 包装 +
// PaintQuota/PaintQuotaCard）。2026-09-18 自 tray_ui.cpp 迁出（God Module 拆分）：
// 布局来自 quota_layout（纯函数），控件原语与共享状态经 tray_ui_internal.hpp。
#include "tray_ui_internal.hpp"
#include "app.hpp"
#include <objidl.h> // GDI+ 头的前置要求
#include <gdiplus.h>
#include "providers.hpp"
#include "provider_icons.hpp"
#include "util.hpp"
#include <algorithm>
#include <cmath>

namespace qb {

using ui::QuotaLayout;
using ui::QuotaCardR;

// 趋势图表（2026-09-17 stitch 重构）：内嵌深色面板 + 面板内左缘刻度 + 虚线网格 +
// 渐变面积填充 + 折线 + 末端点环。rc 为整块图表面板（底/刻度/绘图区都画在内）。
// 百分比序列纵轴固定 0..100（诚实口径，不放大噪声）；金额序列按 min/max 取整刻度。
void DrawTrendChart(HDC hdc, const RECT& rc, const std::vector<double>& v, bool is_percent,
                    COLORREF color, int alpha) {
    FillRr(hdc, rc, 10, theme::kSInset);
    StrokeRr(hdc, rc, 10, theme::kSBorderSoft, 180, 1.0f);
    if (v.size() < 2 || rc.right - rc.left < LS(90) || rc.bottom - rc.top < LS(48)) return;
    double mn = v[0], mx = v[0];
    for (double x : v) {
        if (x < mn) mn = x;
        if (x > mx) mx = x;
    }
    // 纵轴范围：百分比固定 0..100；金额上下各留 12% 余量并避免零跨距
    double y0 = 0, y1 = 100;
    if (!is_percent) {
        y0 = mn - (mx - mn) * 0.12;
        y1 = mx + (mx - mn) * 0.12;
        if (y1 - y0 < 1e-9) {
            y0 -= 1.0;
            y1 += 1.0;
        }
        if (y0 < 0) y0 = 0;
    }
    const int pad = LS(8);    // 面板内边距（p-2）
    const int gutter = LS(38); // 面板内左缘刻度栏（右对齐 3 个标签；容下 "21.97" 五字符）
    RECT plot{rc.left + pad + gutter, rc.top + pad, rc.right - pad, rc.bottom - pad};
    auto fy = [&](double val) {
        double t = (val - y0) / (y1 - y0);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        return (int)(plot.bottom - (plot.bottom - plot.top) * t);
    };
    // 刻度文案：上/中/下三档；小数位按纵轴跨度选（避免小跨度时标签重复）
    const double ticks[3] = {y1, (y0 + y1) / 2, y0};
    wchar_t tl[3][16];
    const double span = y1 - y0;
    for (int i = 0; i < 3; ++i) {
        double t = ticks[i];
        if (is_percent)
            swprintf_s(tl[i], L"%.0f%%", t);
        else if (span < 0.5)
            swprintf_s(tl[i], L"%.2f", t);
        else if (span < 50 || std::abs(t) < 10)
            swprintf_s(tl[i], L"%.1f", t);
        else
            swprintf_s(tl[i], L"%.0f", t);
    }
    // 刻度文字（面板内左缘右对齐；纯 ASCII 才用等宽，防 CJK 回退宽度漂移）
    for (int i = 0; i < 3; ++i) {
        int ty = fy(ticks[i]);
        RECT lab{rc.left + LS(4), ty - LS(9), plot.left - LS(4), ty + LS(9)};
        Txt(hdc, lab, tl[i], theme::TypeSize::Mini, false, theme::kSOutline, 2, 1,
            theme::kFontMono);
    }
    // 网格（上/中两档虚线）+ 折线下方面积渐变填充 + 折线 + 末端点
    theme::EnsureGdiplus();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    auto Gc = [](COLORREF c, int a) {
        return Gdiplus::Color((BYTE)a, GetRValue(c), GetGValue(c), GetBValue(c));
    };
    Gdiplus::Pen grid(Gc(theme::kSGridLine, 255), 1.0f);
    grid.SetDashStyle(Gdiplus::DashStyleDash);
    for (int i = 0; i < 2; ++i) {
        float gy = (float)fy(ticks[i]);
        g.DrawLine(&grid, (float)plot.left, gy, (float)plot.right, gy);
    }
    const float pw = (float)(plot.right - plot.left);
    auto fx = [&](size_t i) { return (float)plot.left + pw * (float)i / (float)(v.size() - 1); };
    auto fyp = [&](double val) { return (float)fy(val); };
    Gdiplus::PointF area[4] = {
        Gdiplus::PointF(fx(0), fyp(v.front())),
        Gdiplus::PointF(fx(v.size() - 1), fyp(v.back())),
        Gdiplus::PointF((float)plot.right, (float)plot.bottom),
        Gdiplus::PointF((float)plot.left, (float)plot.bottom),
    };
    Gdiplus::RectF gr((float)plot.left, (float)plot.top, pw,
                      (float)(plot.bottom - plot.top));
    Gdiplus::LinearGradientBrush fill(gr, Gc(color, alpha * 77 / 255), Gc(color, 0), 90.0f,
                                      FALSE);
    g.FillPolygon(&fill, area, 4);
    Gdiplus::Pen pen(Gc(color, alpha), std::max(LSf(2.0f), 1.0f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    for (size_t i = 1; i < v.size(); ++i)
        g.DrawLine(&pen, fx(i - 1), fyp(v[i - 1]), fx(i), fyp(v[i]));
    // 末端点：底色圆 + 色环（stitch：fill=app.bg, stroke=折线色）
    const float ex = fx(v.size() - 1), ey = fyp(v.back());
    Gdiplus::SolidBrush dotb(Gc(theme::kSbg, 255));
    g.FillEllipse(&dotb, ex - 3.5f, ey - 3.5f, 7.0f, 7.0f);
    Gdiplus::Pen dotp(Gc(color, alpha), 2.0f);
    g.DrawEllipse(&dotp, ex - 3.5f, ey - 3.5f, 7.0f, 7.0f);
}

// 添加面板的类型 chips 序（与 g_q.add_type 对应）
const AccountType kAddTypes[ui::kAddTypeCount] = {AccountType::Kimi, AccountType::Codex,
                                                  AccountType::Glm, AccountType::Deepseek,
                                                  AccountType::Antigravity};
const wchar_t* AddTypeLabel(int i) {
    switch (kAddTypes[i]) {
        case AccountType::Codex: return L"Codex";
        case AccountType::Glm: return L"GLM";
        case AccountType::Deepseek: return L"DeepSeek";
        case AccountType::Antigravity: return L"Antigravity";
        default: return L"Kimi Code";
    }
}
const wchar_t* AddTypeKeyPlaceholder(int i) {
    switch (kAddTypes[i]) {
        case AccountType::Deepseek: return L"请输入 sk-dsk-... 凭据密钥";
        case AccountType::Glm: return L"请输入智谱 bigmodel.cn API Key...";
        case AccountType::Codex: return L"";
        case AccountType::Antigravity: return L"";
        default: return L"请输入 sk-kimi-... 凭证密钥";
    }
}
const wchar_t* LocalCredentialStatus(AccountType t, bool ready, bool compact) {
    const bool ag = t == AccountType::Antigravity;
    if (compact) {
        if (ag) return ready ? L"hub 已就绪" : L"Antigravity 未运行";
        return ready ? L"auth.json 已就绪" : L"未登录 Codex";
    }
    if (ag) return ready ? L"免填 Key · 本机 Antigravity hub 已就绪（只读）"
                         : L"未运行 · 启动 Antigravity 并登录后自动连接";
    return ready ? L"免填 Key · ~/.codex/auth.json 已就绪（只读）"
                 : L"未就绪 · 终端运行 codex auth login 后自动读取";
}
// 展开区安全说明行（无 Key 类）:没有密钥可加密,如实说明凭证由谁维护
static const wchar_t* LocalCredentialNote(AccountType t) {
    return t == AccountType::Antigravity
               ? L"免填 Key：凭证由本机 Antigravity 维护，QuotaBar 只读、不落盘。"
               : L"免填 Key：令牌由 Codex CLI 维护，QuotaBar 只读、不落盘。";
}

// 薄包装：从 App 账户状态 + g_q 会话态快照输入，纯计算在 quota_layout.cpp
// （selftest 覆盖）；g_q.scroll 经引用参数被钳制写回。
QuotaLayout ComputeQuota(App* app, const RECT& content) {
    ui::QuotaPageIn in;
    in.add_open = g_q.add_open;
    in.hidden_open = g_q.hidden_open;
    in.scroll = g_q.scroll;
    for (const auto& st : app->Accounts()) {
        ui::QuotaCardIn c;
        c.visible = st.cfg.card_visible;
        c.has_trend = app->AccountHasTrendFor(st.cfg.id);
        c.expanded = g_q.IsExpanded(st.cfg.id);
        c.alert_capable = ProviderAlertCapable(st.cfg.type);
        c.uses_api_key = ProviderUsesApiKey(st.cfg.type);
        in.cards.push_back(c);
    }
    return ui::ComputeQuotaLayout(in, content, g_layout_dpi, g_q.scroll);
}

void PaintQuotaCard(HDC m, App* app, HWND h, const RECT& card, const DetailCard& c,
                    QuotaCardR& r, int vis_i) {
    (void)h;
    const auto& accs = app->Accounts();
    const int ai = g_qlayout.vis_idx[vis_i];
    const AccountState& st = accs[ai];
    const bool enabled = st.cfg.enabled;
    const bool expanded = g_q.IsExpanded(st.cfg.id);
    const int base = kWAccBase + vis_i * kAccStride;
    const int p = LS(24);
    const int alpha = !enabled ? 130 : (c.dimmed ? 150 : 255);
    const bool has_trend = r.trend.bottom > r.trend.top && c.has_trend;

    // ---- 卡体：hover 提亮 + 细描边（stitch：card/card-hover + border white/8）----
    const bool card_hot = PtInRect(&card, g_cursor_pt) != FALSE;
    FillRr(m, card, 16, card_hot ? theme::kSSurfHigh : theme::kSSurf);
    StrokeRr(m, card, 16,
             card_hot ? Blend(theme::kSCardBorder, theme::kSOnSurf, 45) : theme::kSCardBorder,
             200, 1.0f);

    // ---- 头行：徽章 + 别名/类型章/副题 + 状态 chip + 显隐眼 + 启用开关 ----
    RECT badge = RC(card.left + p, card.top + p, LS(40), LS(40));
    COLORREF bbg, bbr, bfg;
    BrandLook(c.badge, bbg, bbr, bfg);
    FillRr(m, badge, 10, bbg);
    StrokeRr(m, badge, 10, bbr, 170, 1.0f);
    if (app->Cfg().display.embed_brand_icons && ProviderIconReady(st.cfg.type)) {
        // 原厂图标贴品牌色芯片内（保留芯片的置灰/停用语义，字母换成真标识）
        const int inset = LS(5);
        DrawProviderIcon(m, st.cfg.type,
                         RECT{badge.left + inset, badge.top + inset,
                              badge.right - inset, badge.bottom - inset});
    } else {
        wchar_t letter[2]{c.badge, 0};
        Txt(m, badge, letter, theme::TypeSize::Head, true, bfg, 1, 1);
    }
    int tx = badge.right + LS(12);
    DrawToggle(m, r.toggle, enabled, enabled ? theme::kSPrimCont : theme::kSSurfHigh2,
               base + kAccEnable);
    DrawIconBtn(m, r.eye, IcEye, theme::kSOutline, base + kAccEye, theme::kSSurfHigh2,
                theme::kSOnSurf, 16);
    int hdr_right = r.eye.left - LS(10);
    // 状态 chip：异常才发声（正常=环色已表达，不再挂「正常」签；停用优先级最高）
    std::wstring chip_text = enabled ? c.status_text : L"已停用";
    DetailTone chip_tone = enabled ? c.status_tone : DetailTone::Muted;
    if (!chip_text.empty() && chip_tone != DetailTone::Ok) {
        COLORREF ctone = ToneColor(chip_tone);
        SIZE stx = Measure(m, chip_text.c_str(), theme::TypeSize::Mini, true, ctone);
        const int chip_h = LS(22);
        const int chip_w = stx.cx + LS(6) + LS(6) + LS(16);
        RECT chr{r.eye.left - LS(10) - chip_w, card.top + p + LS(1), r.eye.left - LS(10),
                 card.top + p + LS(1) + chip_h};
        FillRr(m, chr, 11, Blend(ctone, theme::kSSurf, 40));
        StrokeRr(m, chr, 11, Blend(ctone, theme::kSSurf, 120), 150, 1.0f);
        Dot(m, chr.left + LS(10), (chr.top + chr.bottom) / 2, 6, ctone, alpha);
        Txt(m, RECT{chr.left + LS(16), chr.top, chr.right - LS(5), chr.bottom},
            chip_text.c_str(), theme::TypeSize::Mini, true, ctone, 0, 1, nullptr, alpha);
        hdr_right = chr.left - LS(10);
    }
    // 别名 + 类型章同行（章底/字随品牌三件套）
    SIZE tsz = Measure(m, c.title.c_str(), theme::TypeSize::Head, true, theme::kSOnSurf);
    Txt(m, RECT{tx, card.top + p - LS(2), tx + tsz.cx + LS(6), card.top + p + LS(20)},
        c.title.c_str(), theme::TypeSize::Head, true, theme::kSOnSurf, 0, 1, nullptr, alpha);
    {
        const wchar_t* tag = TypeTag(c.badge);
        SIZE gsz = Measure(m, tag, theme::TypeSize::Mini, true, bfg);
        RECT tchr{tx + tsz.cx + LS(8), card.top + p + LS(2), tx + tsz.cx + LS(8) + gsz.cx + LS(14),
                  card.top + p + LS(20)};
        // 窄卡不放章:放得下才整章画。旧阈值 4 DIP 只够剩一个字——标题行一有状态 chip
        // （未配置/未运行…）就被钳成「周…」「本」这类残章
        if (tchr.right <= hdr_right) {
            FillRr(m, tchr, 4, bbg);
            StrokeRr(m, tchr, 4, bbr, 150, 1.0f);
            Txt(m, tchr, tag, theme::TypeSize::Mini, true, bfg, 1, 1, nullptr, alpha);
        }
    }
    Txt(m, RECT{tx, card.top + p + LS(20), hdr_right, card.top + p + LS(38)},
        c.subtitle.c_str(), theme::TypeSize::Cap, false, theme::kSOutline, 0, 1, nullptr,
        alpha);
    // ⓘ 口径说明：副标题之后；超宽时钳到状态 chip 左侧
    RECT note_hit = RC(tx + Measure(m, c.subtitle.c_str(), theme::TypeSize::Cap, false,
                                    theme::kSOutline).cx +
                           LS(10),
                       card.top + p + LS(21), LS(18), LS(18));
    if (note_hit.right > hdr_right)
        note_hit = RC(hdr_right - LS(20), card.top + p + LS(21), LS(18), LS(18));
    Icon(m, IcInfo, RC(note_hit.left + LS(2), note_hit.top + LS(2), LS(14), LS(14)),
         Hot(base + kAccNote) ? theme::kSOnSurfVar : theme::kSOutline, alpha);
    AddHit(note_hit, base + kAccNote);
    r.note = note_hit;
    // 头部拖拽排序命中区（F3）：徽章到 ⓘ 之前的标题带；ⓘ/chip 等命中在其后注册可覆盖
    AddHit(RC(badge.left, card.top, tx - LS(6) - badge.left, LS(48)), base + kAccDrag);

    // ---- 核心数值区（内嵌深色面板：左数值格 + 细分隔线 + 右格） ----
    const RECT& stats = r.stats;
    FillRr(m, stats, 12, theme::kSInset);
    StrokeRr(m, stats, 12, theme::kSBorderSoft, 200, 1.0f);
    const int mid = (stats.left + stats.right) / 2;
    {
        RECT vline{mid, stats.top + LS(12), mid + 1, stats.bottom - LS(12)};
        SetDCBrushColor(m, theme::kSBorderSoft);
        FillRect(m, &vline, (HBRUSH)GetStockObject(DC_BRUSH));
    }
    if (c.rows.empty()) {
        // 凭证失效/查询中且无窗口数据：数值区给一行说明，不留空黑块
        std::wstring tip = !c.note.empty() ? c.note : L"暂无额度窗口数据";
        Txt(m, RECT{stats.left + LS(14), stats.top, stats.right - LS(14), stats.bottom},
            tip.c_str(), theme::TypeSize::Cap, false, theme::kSOutline, 0, 1, nullptr, alpha);
    } else if (c.badge == L'D') {
        // 金额账户：左格「可用余额 + CNY 章 + 大数」，右格「今日消耗 / 扣费状态」
        const DetailWindowRow& rw = c.rows[0];
        COLORREF vc = rw.tone == DetailTone::Neutral ? theme::kSOnSurf : ToneColor(rw.tone);
        Txt(m, RECT{stats.left + LS(14), stats.top + LS(10), mid - LS(10),
                    stats.top + LS(24)},
            L"可用余额", theme::TypeSize::Mini, false, theme::kSOutline, 0, 0, nullptr,
            alpha);
        RECT cny{stats.left + LS(14) + LS(46), stats.top + LS(9),
                 stats.left + LS(14) + LS(46) + LS(30), stats.top + LS(23)};
        FillRr(m, cny, 3, theme::kSSurfHigh2);
        Txt(m, cny, L"CNY", theme::TypeSize::Mini, false, theme::kSOutline, 1, 1,
            theme::kFontMono, alpha);
        Txt(m, RECT{stats.left + LS(14), stats.top + LS(26), mid - LS(10),
                    stats.bottom - LS(2)},
            rw.value_text.c_str(), theme::TypeSize::Num, true, vc, 0, 0, nullptr, alpha);
        // 右格两行：label 左对齐、值右对齐（今日消耗 / 扣费状态）
        std::wstring spend = c.spend_text;
        std::wstring spend_lab = L"今日消耗", spend_val;
        if (!spend.empty()) {
            size_t sp = spend.find(L' ');
            if (sp != std::wstring::npos) {
                spend_lab = spend.substr(0, sp);
                spend_val = spend.substr(sp + 1);
            } else {
                spend_val = spend;
            }
        }
        const int row_y[2] = {stats.top + LS(12), stats.top + LS(36)};
        RECT bill_row{mid + LS(16), row_y[0], stats.right - LS(14), row_y[0] + LS(16)};
        Txt(m, bill_row, spend_lab.c_str(), theme::TypeSize::Mini, false, theme::kSOutline, 0,
            0, nullptr, alpha);
        Txt(m, bill_row, spend_val.c_str(), theme::TypeSize::Mini, false, theme::kSOnSurf, 2,
            0, IsAsciiText(spend_val) ? theme::kFontMono : nullptr, alpha);
        RECT stat_row{mid + LS(16), row_y[1], stats.right - LS(14), row_y[1] + LS(16)};
        Txt(m, stat_row, L"扣费状态", theme::TypeSize::Mini, false, theme::kSOutline, 0, 0,
            nullptr, alpha);
        const bool bill_ok = st.snap.status == FetchStatus::Ok;
        // FetchStatusText 返回中文短语； Utf8ToWide 按值返回 → 必须先落局部再取 c_str
        std::wstring bill_w = bill_ok ? std::wstring(L"正常")
                                      : Utf8ToWide(FetchStatusText(st.snap.status));
        COLORREF bill_c = bill_ok ? theme::kSSecondary : theme::kSTertiary;
        SIZE bs = Measure(m, bill_w.c_str(), theme::TypeSize::Mini, false, bill_c);
        Dot(m, stat_row.right - bs.cx - LS(12), (stat_row.top + stat_row.bottom) / 2, 6,
            bill_c, alpha);
        Txt(m, RECT{stat_row.right - bs.cx - LS(8), stat_row.top, stat_row.right,
                    stat_row.bottom},
            bill_w.c_str(), theme::TypeSize::Mini, false, bill_c, 0, 0, nullptr, alpha);
    } else {
        // 周期额度账户：每窗口一环（环心窗口标签）+ 大号剩余% + 余量时长
        const int cells = (int)c.rows.size() > 2 ? 2 : (int)c.rows.size();
        const int cell_w = (stats.right - stats.left) / cells;
        for (int i = 0; i < cells; ++i) {
            const DetailWindowRow& rw = c.rows[i];
            RECT cell{stats.left + i * cell_w, stats.top, stats.left + (i + 1) * cell_w,
                      stats.bottom};
            const int rd = LS(44);
            const int rcx = cell.left + LS(12) + rd / 2;
            const int rcy = (cell.top + cell.bottom) / 2;
            // 深度枯竭（剩余 ≤5%）升级为红色 + 「触底」章
            const bool critical = rw.tone == DetailTone::Warn && rw.ring_percent >= 0 &&
                                  rw.ring_percent <= 5.0f;
            const DetailTone vis_tone =
                critical ? DetailTone::Bad
                         : (rw.tone == DetailTone::Neutral ? DetailTone::Ok : rw.tone);
            COLORREF arc = ToneColor(vis_tone);
            theme::DrawRingPx(m, (float)rcx, (float)rcy, (float)rd, LSf(3.5f),
                              rw.ring_percent, theme::kSGridLine, arc, alpha);
            // 环心窗口标签（5h/7d，纯 ASCII 才用等宽）
            Txt(m, RC(rcx - rd / 2, rcy - LS(9), rd, LS(18)), rw.label.c_str(),
                theme::TypeSize::Mini, false, arc, 1, 1,
                IsAsciiText(rw.label) ? theme::kFontMono : nullptr, alpha);
            const int vx = cell.left + LS(12) + rd + LS(14);
            std::wstring val = rw.value_text;
            std::wstring num = val, pct;
            if (!val.empty() && val.back() == L'%') {
                num = val.substr(0, val.size() - 1);
                pct = L"%";
            }
            SIZE ns = Measure(m, num.c_str(), theme::TypeSize::Num, true, theme::kSOnSurf);
            RECT nr{vx, rcy - LS(18), vx + ns.cx + LS(4), rcy + LS(10)};
            Txt(m, nr, num.c_str(), theme::TypeSize::Num, true, theme::kSOnSurf, 0, 1,
                nullptr, alpha);
            if (!pct.empty()) {
                Txt(m, RECT{nr.right, rcy - LS(16), nr.right + LS(18), rcy + LS(10)},
                    pct.c_str(), theme::TypeSize::Cap, false, theme::kSOnSurfVar, 0, 1,
                    nullptr, alpha);
            }
            if (critical) {
                RECT low_chip = RC(nr.right + LS(24), rcy - LS(13), LS(40), LS(20));
                FillRr(m, low_chip, 4, Blend(theme::kSError, theme::kSInset, 51));
                Txt(m, low_chip, L"触底", theme::TypeSize::Mini, true, theme::kSError, 1, 1);
            }
            COLORREF reset_c = critical ? theme::kSError : theme::kSOutline;
            std::wstring reset_t = RemainText(rw.reset_text);
            if (critical) reset_t = L"仅剩 " + reset_t.substr(2); // "余 X" → "仅剩 X"
            Txt(m, RECT{vx, rcy + LS(12), cell.right - LS(10), rcy + LS(30)},
                reset_t.c_str(), theme::TypeSize::Mini, false, reset_c, 0, 0, nullptr,
                alpha);
        }
    }

    // ---- 趋势图表带：分隔线 + 标题行（左跨度/右状态）+ 内嵌图表面板 ----
    if (has_trend) {
        HLinePx(m, card.left + p, card.right - p, r.trend.top - LS(10), theme::kSBorderSoft);
        std::wstring span = L"额度历史";
        if (!c.trend_span.empty()) span += L" · " + c.trend_span;
        Txt(m, RECT{r.trend.left, r.trend.top, r.trend.right, r.trend.top + LS(18)},
            span.c_str(), theme::TypeSize::Cap, false, theme::kSOnSurfVar, 0, 0, nullptr,
            alpha);
        std::wstring txt = !c.spend_text.empty() ? c.spend_text : c.eta_text;
        if (!txt.empty()) {
            COLORREF tc = c.trend_is_percent ? theme::kSSecondary : theme::kSOutline;
            int tw = Measure(m, txt.c_str(), theme::TypeSize::Mini, false, tc).cx;
            Txt(m, RECT{r.trend.right - tw, r.trend.top + LS(1), r.trend.right,
                        r.trend.top + LS(1) + LS(16)},
                txt.c_str(), theme::TypeSize::Mini, false, tc, 0, 0, nullptr, alpha);
        }
        RECT plot{r.trend.left, r.trend.top + LS(26), r.trend.right, r.trend.bottom};
        DrawTrendChart(m, plot, c.spark, c.trend_is_percent,
                       c.trend_is_percent ? theme::kSSecondary : theme::kSPrimary, 235);
    }

    // ---- 页脚操作栏：分隔线 + 打开控制台（左）+ 设置 ▾（右） ----
    HLinePx(m, card.left + p, card.right - p, r.settings_btn.top - LS(14),
            theme::kSBorderSoft);
    if (c.console_url) {
        RECT lr{card.left + p, r.settings_btn.top, r.settings_btn.left - LS(12),
                r.settings_btn.bottom};
        DrawLink(m, lr, L"打开控制台 →", theme::TypeSize::Cap, false, c.console_url);
    }
    DrawBtn(m, r.settings_btn, expanded ? L"收起 ▴" : L"设置 ▾", IcTune, StyleSoft(),
            base + kAccSettings, theme::TypeSize::Cap);

    // ---- 展开设置区（别名 / Key 或 codex 状态 / 安全说明 / 删除·保存） ----
    if (expanded) {
        // 别名行
        Txt(m, RC(card.left + p, r.ex_alias.top + LS(8), LS(48), LS(18)), L"别名",
            theme::TypeSize::Cap, false, theme::kSOnSurfVar, 0, 0);
        FillRr(m, r.ex_alias, 8, theme::kSSurfLow2);
        StrokeRr(m, r.ex_alias, 8, theme::kSBorderSoft, 180, 1.0f);
        // Key 行 / codex 免密行
        if (c.uses_api_key) {
            Txt(m, RC(card.left + p, r.ex_key.top + LS(9), LS(52), LS(18)), L"API Key",
                theme::TypeSize::Cap, false, theme::kSOnSurfVar, 0, 0);
            FillRr(m, r.ex_key, 8, theme::kSSurfLow2);
            StrokeRr(m, r.ex_key, 8, theme::kSBorderSoft, 180, 1.0f);
            Icon(m, IcLock, RC(r.ex_key.left + LS(12),
                               (r.ex_key.top + r.ex_key.bottom) / 2 - LS(9), LS(18), LS(18)),
                 theme::kSOutline, 255);
            DrawIconBtn(m, r.ex_key_eye, IcEye, // 掩码/明文共用眼睛图标（区分图标待 UI 拍板）
                        theme::kSOnSurfVar, base + kAccKeyEye, theme::kSSurfHigh,
                        theme::kSOnSurf, 16);
            DrawIconBtn(m, r.ex_key_copy, IcCopy, theme::kSOnSurfVar, base + kAccKeyCopy,
                        theme::kSSurfHigh, theme::kSOnSurf, 16);
            DrawIconBtn(m, r.ex_key_clear, IcClear, theme::kSOnSurfVar, base + kAccKeyClear,
                        theme::kSSurfHigh, theme::kSError, 16);
        } else {
            // 无 Key 类凭证行（codex = ~/.codex/auth.json；antigravity = 本机 hub）。状态框与
            // 「重新读取」按钮同在卡内:按钮右对齐内边距,状态文案用紧凑档（框内仅剩结论宽度）
            Txt(m, RC(card.left + p, r.ex_key.top + LS(9), LS(52), LS(18)), L"凭证",
                theme::TypeSize::Cap, false, theme::kSOnSurfVar, 0, 0);
            FillRr(m, r.ex_key, 8, theme::kSSurfLow2, 120);
            StrokeRr(m, r.ex_key, 8, theme::kSBorderSoft, 180, 1.0f);
            const bool ready = st.cfg.type == AccountType::Antigravity
                                   ? app->AntigravityHubReady()
                                   : app->CodexAuthReady();
            Txt(m, RECT{r.ex_key.left + LS(12), r.ex_key.top, r.ex_key.right - LS(8),
                        r.ex_key.bottom},
                LocalCredentialStatus(st.cfg.type, ready, true), theme::TypeSize::Cap, false,
                ready ? theme::kSSecondary : theme::kSTertiary, 0, 1);
            DrawBtn(m, r.ex_codex_sync, L"重新读取", IcRestart, StyleGhost(),
                    base + kAccCodexSync);
        }
        // 低额预警阈值行（比例窗口类卡片；点击弹档位菜单，即选即存）
        if (ProviderAlertCapable(st.cfg.type)) {
            if (r.ex_alert.right > r.ex_alert.left) {
                Txt(m, RC(card.left + p, r.ex_alert.top + LS(6), LS(92), LS(18)),
                    L"低额预警阈值", theme::TypeSize::Cap, false, theme::kSOnSurfVar, 0, 0);
                std::wstring lab;
                if (st.cfg.alert_pct == 0) {
                    int g = std::min(std::max(app->Cfg().notify.kimi_threshold_pct, 1), 99);
                    lab = L"继承全局（" + std::to_wstring(g) + L"%）";
                } else if (st.cfg.alert_pct == 1) {
                    lab = L"已关闭";
                } else {
                    lab = L"剩余 ≤ " + std::to_wstring(st.cfg.alert_pct) + L"%";
                }
                DrawBtn(m, r.ex_alert, lab.c_str(), IcBell, StyleGhost(), base + kAccAlert);
            }
        }
        // 安全说明行（紧跟展开区最后一行控件；预警行存在时随其下移，避免叠压）
        {
            const bool has_alert_row = ProviderAlertCapable(st.cfg.type) &&
                                      r.ex_alert.right > r.ex_alert.left;
            const int note_y =
                (has_alert_row ? r.ex_alert.bottom : r.ex_key.bottom) + LS(8);
            Txt(m, RECT{card.left + p + LS(2), note_y, card.right - p * 2, note_y + LS(18)},
                c.uses_api_key ? L"密钥经 DPAPI 加密存储在本机，不上传云端。"
                               : LocalCredentialNote(st.cfg.type),
                theme::TypeSize::Mini, false, theme::kSOutline, 0, 0);
        }
        // 按钮：删除卡片（红）/ 保存修改
        RECT del_bg = r.ex_delete;
        const bool dh = Hot(base + kAccDelete), dd = Down(base + kAccDelete);
        FillRr(m, del_bg, 8, Blend(theme::kSErrCont, theme::kSSurfLow, dd ? 90 : (dh ? 70 : 45)),
               dd || dh ? 255 : 220);
        StrokeRr(m, del_bg, 8, Blend(theme::kSError, theme::kSSurfLow, 90), 160, 1.0f);
        Txt(m, del_bg, L"删除卡片", theme::TypeSize::Cap, false, theme::kSError, 1, 1);
        AddHit(del_bg, base + kAccDelete);
        DrawBtn(m, r.ex_save, L"保存修改", IcCheck, StylePrimary(), base + kAccSave,
                theme::TypeSize::Cap, true);
        // 展开区输入框由 Relayout 摆放原生 EDIT（与自绘底同一矩形来源）
    }

    // ⓘ 悬停气泡：口径说明 + 最后成功时间（元信息带不再长显时间，最后一步覆盖卡内元素）
    std::wstring tip = c.note;
    if (!c.last_update.empty()) tip = tip.empty() ? c.last_update : tip + L"\n" + c.last_update;
    if (!tip.empty() && Hot(base + kAccNote))
        DrawNoteBubble(m, card, POINT{r.note.right, r.note.bottom}, tip);
}

void PaintQuota(HDC m, App* app, const RECT& content, HWND h) {
    g_cards = app->DetailCards(true); // 与 vis_idx 同序（accounts 顺序 × card_visible 过滤）
    g_qlayout = ComputeQuota(app, content);
    QuotaLayout& L = g_qlayout; // 非 const：卡片绘制回填 ⓘ 命中锚点
    const int x0 = content.left;
    const int cw = content.right - content.left;
    const int y0 = content.top;

    // 页头（固定，不滚动）：标题 + 就绪 chip + 刷新间隔 chip | 右侧统计 + 刷新 + 添加
    Txt(m, RC(x0, y0 + LS(2), LS(220), LS(30)), L"额度详情", theme::TypeSize::Num, true,
        theme::kSOnSurf, 0, 0);
    int total = 0, ready = 0;
    for (int vi : L.vis_idx) {
        const auto& st = app->Accounts()[vi];
        if (!st.cfg.enabled) continue;
        ++total;
        if (st.snap.status == FetchStatus::Ok) ++ready;
    }
    wchar_t ready_t[40];
    if (total > 0) swprintf_s(ready_t, L"%d/%d 服务已就绪", ready, total);
    else wcscpy_s(ready_t, L"尚未添加账户");
    {
        // 就绪 chip：绿系胶囊（绿点 + 淡绿底 + 绿描边；无账户时灰化）
        COLORREF cc = total > 0 ? theme::kSSecondary : theme::kSOutline;
        SIZE rs = Measure(m, ready_t, theme::TypeSize::Cap, false, cc);
        RECT rchip = RC(x0 + LS(108), y0 + LS(5), rs.cx + LS(36), LS(24));
        FillRr(m, rchip, 12, Blend(cc, theme::kSbg, 30));
        StrokeRr(m, rchip, 12, Blend(cc, theme::kSbg, 115), 160, 1.0f);
        Dot(m, rchip.left + LS(12), (rchip.top + rchip.bottom) / 2, 6, cc);
        Txt(m, RECT{rchip.left + LS(20), rchip.top, rchip.right - LS(6), rchip.bottom},
            ready_t, theme::TypeSize::Cap, false, cc, 0, 1);
        // 刷新间隔 chip：蓝点 + 中性胶囊（暂停时转琥珀）
        const auto& q = app->Cfg().query;
        std::wstring itxt =
            q.pause
                ? L"查询已暂停"
                : (q.power_save ? L"节能刷新 · 900s"
                                : (L"实时刷新间隔: " + std::to_wstring(q.interval_sec) + L"s"));
        COLORREF ic = q.pause ? theme::kSTertiary : theme::kSOnSurfVar;
        COLORREF idc = q.pause ? theme::kSTertiary : theme::kSPrimary;
        int ix = rchip.right + LS(10);
        SIZE is2 = Measure(m, itxt.c_str(), theme::TypeSize::Cap, false, ic);
        if (ix + is2.cx + LS(36) < L.stats_block.left - LS(20)) { // 防与右侧统计挤碰
            RECT ichip = RC(ix, y0 + LS(5), is2.cx + LS(36), LS(24));
            FillRr(m, ichip, 12, theme::kSBorderSoft);
            StrokeRr(m, ichip, 12, theme::kSCardBorder, 160, 1.0f);
            Dot(m, ichip.left + LS(12), (ichip.top + ichip.bottom) / 2, 6, idc);
            Txt(m, RECT{ichip.left + LS(20), ichip.top, ichip.right - LS(6), ichip.bottom},
                itxt.c_str(), theme::TypeSize::Cap, false, ic, 0, 1);
        }
    }
    // 右上统计块：总计可用额度 / N 项服务正常计费中（矩形来自布局单一来源；
    // 有隐藏 chip 时布局已让出，不得再按 refresh_btn 推导——会与 chip 重叠被盖住）
    {
        int ok_n = 0;
        for (int vi : L.vis_idx) {
            const auto& st = app->Accounts()[vi];
            if (st.cfg.enabled && st.snap.status == FetchStatus::Ok) ++ok_n;
        }
        wchar_t stat_t[64];
        if (total == 0) wcscpy_s(stat_t, L"尚未添加服务");
        else if (ok_n == total) swprintf_s(stat_t, L"%d 项服务正常计费中", total);
        else swprintf_s(stat_t, L"%d/%d 项服务正常计费中", ok_n, total);
        const int rn_right = L.stats_block.right;
        Txt(m, RECT{rn_right - LS(190), y0 + LS(1), rn_right, y0 + LS(15)}, L"总计可用额度",
            theme::TypeSize::Mini, false, theme::kSOutline, 2, 0);
        Txt(m, RECT{rn_right - LS(190), y0 + LS(16), rn_right, y0 + LS(34)}, stat_t,
            theme::TypeSize::Cap, true, theme::kSOnSurf, 2, 0);
    }
    // 右上：已隐藏 chip + 刷新 + 添加卡片（刷新只在本页页头，不再常驻标题栏）
    if (L.has_hidden_chip) {
        wchar_t ht[24];
        swprintf_s(ht, L"已隐藏 %d", (int)L.hidden_idx.size());
        DrawBtn(m, L.hidden_chip, ht, IcEye, StyleGhost(), kWHiddenChip);
    }
    DrawIconBtn(m, L.refresh_btn, IcSync, theme::kSOnSurfVar, kWRefresh, theme::kSSurfHigh2,
                theme::kSOnSurf, 16);
    StrokeRr(m, L.refresh_btn, 10, theme::kSCardBorder, 210, 1.0f);
    DrawBtn(m, L.add_btn, L"添加卡片", IcPlus, StylePrimary(), kWAddCard, theme::TypeSize::Cap,
            true, 10);

    // 空态：一个账户都没有
    if (L.empty_state) {
        RECT box = RC(x0, y0 + LS(96), cw, LS(240));
        FillRr(m, box, 16, theme::kSSurf);
        StrokeRr(m, box, 16, theme::kSCardBorder, 200, 1.0f);
        Icon(m, IcGrid, RC(x0 + cw / 2 - LS(18), box.top + LS(28), LS(36), LS(36)),
             theme::kSOutline, 220);
        Txt(m, RC(x0, box.top + LS(76), cw, LS(24)), L"还没有账户卡片", theme::TypeSize::Head,
            true, theme::kSOnSurf, 1, 0);
        Txt(m, RC(x0, box.top + LS(106), cw, LS(18)),
            L"点击右上角「添加卡片」，选择 Kimi Code / Codex / GLM / DeepSeek / Antigravity 开始监控",
            theme::TypeSize::Cap, false, theme::kSOutline, 1, 0);
        RECT cta = RC(x0 + cw / 2 - LS(70), box.top + LS(150), LS(140), LS(36));
        DrawBtn(m, cta, L"添加卡片", IcPlus, StylePrimary(), kWAddCard, theme::TypeSize::Cap,
                true, 10);
        return;
    }

    // ---- 滚动视口裁剪（2026-09-19 实测：下滚后卡片盖住标题栏/最小化按钮）----
    // 添加面板/隐藏区/卡片墙随 g_q.scroll 整体上移，越过固定页头的部分必须裁掉
    // （显示页 PaintDisplay 同规）。视口顶=页头高（kQuotaHeadDip，与 quota_layout 的
    // after_head 同源）；命中区经 g_hit_clip 同步裁剪，滚到页头/标题栏底下的卡片
    // 不留下不可见却可点的幽灵命中带；原生 EDIT 不受 DC 裁剪，由 Relayout 同规隐藏。
    // 绘制裁剪再外扩 2 DIP：卡片缘恰好与视口相切（右列卡右缘=content.right、首行
    // 顶=视口顶），描边以路径为中心，不外扩会削掉描边外半（右列卡右边框显示不全，
    // 实测）；外扩量远小于页头下 14 DIP 空隙，不会碰到页头内容。命中裁剪保持精确。
    const RECT scroll_vp{content.left, content.top + LS(kQuotaHeadDip), content.right,
                         content.bottom};
    const RECT clip_vp{scroll_vp.left - LS(2), scroll_vp.top - LS(2),
                       scroll_vp.right + LS(2), scroll_vp.bottom + LS(2)};
    SaveDC(m);
    IntersectClipRect(m, clip_vp.left, clip_vp.top, clip_vp.right, clip_vp.bottom);
    g_hit_clip = scroll_vp;

    // ---- 添加面板 ----
    if (g_q.add_open) {
        FillRr(m, L.add_panel, 16, theme::kSSurf);
        StrokeRr(m, L.add_panel, 16, theme::kSCardBorder, 200, 1.0f);
        Txt(m, RC(L.add_panel.left + LS(20), L.add_panel.top + LS(12), LS(220), LS(24)),
            L"新建账户卡片", theme::TypeSize::Head, true, theme::kSOnSurf, 0, 0);
        Txt(m, RC(L.add_panel.left + LS(140), L.add_panel.top + LS(17), LS(400), LS(16)),
            L"选择类型 → 输入账户信息与别名", theme::TypeSize::Cap, false, theme::kSOutline, 0,
            0);
        // 类型 chips（无 Key 类凭证源全机唯一:已有同类型卡片则置灰）
        for (int i = 0; i < ui::kAddTypeCount; ++i) {
            const RECT& rc = L.ap_type[i];
            const bool taken =
                ProviderSingleCard(kAddTypes[i]) && app->Cfg().HasType(kAddTypes[i]);
            const bool sel = g_q.add_type == i && !taken;
            FillRr(m, rc, 8, taken ? theme::kSSurfLow2
                                   : (sel ? theme::kSPrimCont : theme::kSSurfHigh));
            if (!sel && !taken)
                StrokeRr(m, rc, 8, theme::kSCardBorder, 170, 1.0f);
            wchar_t lab[32];
            if (taken)
                swprintf_s(lab, L"%s 已存在", AddTypeLabel(i)); // 无全角括号:五枚 chip 变窄后放不下
            else
                wcscpy_s(lab, AddTypeLabel(i));
            Txt(m, rc, lab, theme::TypeSize::Cap, sel,
                taken ? theme::kSOutline
                      : (sel ? theme::kSOnPrimCont : theme::kSOnSurfVar),
                1, 1);
            if (!taken) AddHit(rc, kWAddType0 + i);
        }
        // 别名行
        Txt(m, RC(L.add_panel.left + LS(20), L.ap_alias.top + LS(8), LS(52), LS(18)),
            L"别名", theme::TypeSize::Body, false, theme::kSOnSurfVar, 0, 0);
        FillRr(m, L.ap_alias, 8, theme::kSSurfLow2);
        // Key 行 / 无 Key 类免填行（凭证由本机凭证源提供）
        const AccountType add_type = kAddTypes[g_q.add_type];
        if (ProviderUsesApiKey(add_type)) {
            Txt(m, RC(L.add_panel.left + LS(20), L.ap_key.top + LS(9), LS(52), LS(18)),
                L"API Key", theme::TypeSize::Body, false, theme::kSOnSurfVar, 0, 0);
            FillRr(m, L.ap_key, 8, theme::kSSurfLow2);
            Icon(m, IcLock, RC(L.ap_key.left + LS(12),
                               (L.ap_key.top + L.ap_key.bottom) / 2 - LS(9), LS(18), LS(18)),
                 theme::kSOutline, 255);
            DrawIconBtn(m, L.ap_key_eye, IcEye, theme::kSOnSurfVar, kWAddKeyEye,
                        theme::kSSurfHigh, theme::kSOnSurf, 16);
            DrawIconBtn(m, L.ap_key_copy, IcCopy, theme::kSOnSurfVar, kWAddKeyCopy,
                        theme::kSSurfHigh, theme::kSOnSurf, 16);
            DrawIconBtn(m, L.ap_key_clear, IcClear, theme::kSOnSurfVar, kWAddKeyClear,
                        theme::kSSurfHigh, theme::kSError, 16);
        } else {
            Txt(m, RC(L.add_panel.left + LS(20), L.ap_key.top + LS(9), LS(52), LS(18)),
                L"凭证", theme::TypeSize::Body, false, theme::kSOnSurfVar, 0, 0);
            FillRr(m, L.ap_key, 8, theme::kSSurfLow2, 120);
            const bool rd = add_type == AccountType::Antigravity ? app->AntigravityHubReady()
                                                                 : app->CodexAuthReady();
            Txt(m, RECT{L.ap_key.left + LS(12), L.ap_key.top, L.ap_key.right - LS(8),
                        L.ap_key.bottom},
                LocalCredentialStatus(add_type, rd, false), theme::TypeSize::Body, false,
                rd ? theme::kSSecondary : theme::kSTertiary, 0, 1);
        }
        // 错误行
        if (!g_q.add_error.empty())
            Txt(m, L.ap_error, g_q.add_error.c_str(), theme::TypeSize::Body, false,
                theme::kSError, 0, 0);
        // 底部：控制台链接 + 取消/创建
        wchar_t cl[96];
        swprintf_s(cl, L"控制台：%hs →", ProviderConsoleUrl(add_type));
        DrawLink(m, L.ap_console, cl, theme::TypeSize::Body, false,
                 ProviderConsoleUrl(add_type));
        DrawBtn(m, L.ap_cancel, L"取消", IcNone, StyleGhost(), kWAddCancel);
        DrawBtn(m, L.ap_create, L"创建卡片", IcCheck, StylePrimary(), kWAddCreate);
    }

    // ---- 已隐藏区 ----
    if (g_q.hidden_open && !L.hidden_idx.empty()) {
        FillRr(m, L.hidden_box, 12, Blend(theme::kSSurfLow2, theme::kSbg, 128));
        const auto& accs = app->Accounts();
        for (size_t j = 0; j < L.hidden_idx.size(); ++j) {
            const AccountState& st = accs[L.hidden_idx[j]];
            const RECT& row = L.hidden_rows[j];
            FillRr(m, row, 8, theme::kSSurf);
            StrokeRr(m, row, 8, theme::kSBorderSoft, 180, 1.0f);
            RECT badge = RC(row.left + LS(10), row.top + LS(8), LS(24), LS(24));
            COLORREF bbg, bbr2, bfg;
            BrandLook(ProviderBadgeOf(st.cfg.type), bbg, bbr2, bfg);
            FillRr(m, badge, 6, bbg);
            wchar_t letter[2]{ProviderBadgeOf(st.cfg.type), 0};
            Txt(m, badge, letter, theme::TypeSize::Cap, true, bfg, 1, 1);
            std::wstring alias = Utf8ToWide(st.cfg.alias.empty()
                                                ? ProviderDefaultAlias(st.cfg.type)
                                                : st.cfg.alias);
            Txt(m, RC(badge.right + LS(10), row.top + LS(10), LS(300), LS(20)),
                alias.c_str(), theme::TypeSize::Body, true, theme::kSOnSurfVar, 0, 0);
            RECT unhide = RC(row.right - LS(84), row.top + LS(7), LS(70), LS(26));
            DrawBtn(m, unhide, L"显示", IcEye, StyleSoft(),
                    kWAccBase + (int)j * kAccStride + kAccUnhide);
        }
    }

    // ---- 卡片网格 ----
    for (size_t i = 0; i < L.vis_idx.size(); ++i) {
        PaintQuotaCard(m, app, h, L.cards[i], g_cards[i], L.cr[i], (int)i);
    }

    // ---- 拖拽排序可视化（F3）：源卡描边强调 + 目标插入线 ----
    if (g_drag_from >= 0 && g_drag_from < (int)L.vis_idx.size()) {
        StrokeRr(m, L.cards[g_drag_from], 12, theme::kSPrimary, 230, 1.6f);
        std::vector<int> others;
        for (int i2 = 0; i2 < (int)L.vis_idx.size(); ++i2)
            if (i2 != g_drag_from) others.push_back(i2);
        if (!others.empty() && g_drag_target >= 0) {
            int y;
            if (g_drag_target <= 0)
                y = L.cards[others.front()].top - LS(7);
            else if (g_drag_target >= (int)others.size())
                y = L.cards[others.back()].bottom + LS(7);
            else
                y = (L.cards[others[g_drag_target - 1]].bottom +
                     L.cards[others[g_drag_target]].top) / 2;
            y = std::max(y, (int)scroll_vp.top); // 钳在滚动视口内，插入线拖到顶仍可见
            RECT bar{content.left, y - LS(2), content.right, y + LS(2)};
            FillRr(m, bar, 2, theme::kSPrimary, 230);
            Dot(m, bar.left + LS(5), y, 8, theme::kSPrimary, 255);
        }
    }

    g_hit_clip = RECT{}; // 恢复无裁剪注册（页头/chrome 路径）
    RestoreDC(m, -1);    // 结束滚动视口裁剪
}
// =========================================================
// 页面绘制：显示设置（2026-09-15 重构为 Chrome 式单列纵向滚动列表）
// 用户反馈驱动：①左右双卡并排太紧凑 → 全宽单列、行距加大、分区滚动；
// ②嵌入任务栏置顶为第一选项；③模式/样式切换即调即存实时可见（不等保存）。
// 布局仍走 Compute* 单一来源产矩形，Paint*/Relayout 只消费；列表滚动 g_dscroll。
// =========================================================

} // namespace qb
