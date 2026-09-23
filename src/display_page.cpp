// display_page.cpp — 显示设置页绘制（模式表 + ComputeDisp 包装 + PaintDisplay）。
// 2026-09-18 自 tray_ui.cpp 迁出：布局来自 disp_layout（纯函数），
// 控件原语与共享草稿经 tray_ui_internal.hpp。
#include "tray_ui_internal.hpp"
#include "app.hpp"
#include <cmath>

namespace qb {

using ui::DispLayout;

// 显示模式 UI 顺序（嵌入任务栏=默认推荐置顶；绘制与点击分发共用此表防漂移）。
// 2026-09-21 模式合并：贴靠任务栏外侧并入「浮条」的吸附开关（旧配置自动迁移）。
const ModeDef kModes[4] = {
    {DisplayMode::Embed, L"嵌入任务栏", L"融合单行，最省空间", true},
    {DisplayMode::Free, L"浮条", L"桌面悬浮 · 可吸附任务栏 · Ctrl+拖动", false},
    {DisplayMode::TrayOnly, L"仅托盘图标", L"隐藏常驻条", false},
    {DisplayMode::Rings, L"圆环浮标", L"任务栏上方环形仪表", false},
};

// =========================================================
// 显示设置布局（2026-09-16 二次重构：macOS 系统设置式分组卡片）
// 设计规范（对齐用户反馈"参考苹果/谷歌设置页"）：
//   · 设置列限宽 640 居中；分区 = 小节标题 + 圆角卡片（kSSurf，r12）
//   · 卡内行高统一 48（模式选择行 54），行间细分隔线；卡左右内边距 20/16
//   · 右缘控件统一内边距 16：开关 44×24、三段选择 240×28、步进 28×28、按钮 30 高
//   · 标签一律 Body 居左、说明 Cap；偏移数值 Num（弃 Hero 巨字，避免与按钮失衡）
// =========================================================
// 薄包装：从 g_draft.cfg 快照输入，纯计算在 disp_layout.cpp；
// g_dscroll 经引用参数被钳制写回。
DispLayout ComputeDisp(const RECT& content) {
    ui::DispPageIn in;
    in.mode = g_draft.cfg.display.mode;
    in.notify_enabled = g_draft.cfg.notify.enabled;
    in.snap = g_draft.cfg.display.bar_snap;
    in.scroll = g_dscroll;
    return ui::ComputeDispLayout(in, content, g_layout_dpi, g_dscroll);
}

// 越屏改动后同步草稿快照（浮条拖放落位/嵌入槽微调不走草稿，页面开着时防旧快照回写）
void SyncDraftDisplay(const DisplayConfig& disp) { g_draft.cfg.display = disp; }

// 显示页脏草稿判定：草稿字段 + 输入框实时文本 vs 已提交配置。
// 输入框打字不写草稿（只在保存时读取），所以 EDIT 文本必须单独比对；节能态输入框
// 禁用且恒显 900（占位），区间比对跳过避免假脏。供页脚「未保存/放弃变更」态与
// 切页/关窗确认（ConfirmDiscardDraft）共用。
bool DisplayDraftDirty(HWND h, const AppConfig& base) {
    const AppConfig& d = g_draft.cfg;
    if (d.query.power_save != base.query.power_save) return true;
    if (d.notify.enabled != base.notify.enabled) return true;
    if (d.query.autostart != base.query.autostart) return true;
    if (d.query.interval_sec != base.query.interval_sec) return true;
    if (d.notify.kimi_threshold_pct != base.notify.kimi_threshold_pct) return true;
    if (fabs(d.notify.deepseek_threshold_cny - base.notify.deepseek_threshold_cny) > 0.005)
        return true;
    if (!d.query.power_save) {
        // 空框按未改处理（GetEditInt 回落 base），避免清空即误报脏
        if (GetEditInt(GetDlgItem(h, kInterval), base.query.interval_sec) !=
            base.query.interval_sec)
            return true;
        if (d.notify.enabled) {
            if (GetEditInt(GetDlgItem(h, kKimiThr), base.notify.kimi_threshold_pct) !=
                base.notify.kimi_threshold_pct)
                return true;
            double dt = GetEditDouble(GetDlgItem(h, kDsThr),
                                      base.notify.deepseek_threshold_cny);
            if (fabs(dt - base.notify.deepseek_threshold_cny) > 0.005) return true;
        }
    }
    return false;
}

void PaintDisplay(HDC m, App* app, const RECT& content, HWND h) {
    const DispLayout L = ComputeDisp(content);
    const AppConfig& d = g_draft.cfg;
    const int slot = g_draft.slot;

    // 页头（固定，不随列表滚动）
    int x0 = content.left, y0 = content.top;
    Txt(m, RC(x0, y0, LS(300), LS(28)), L"显示设置", theme::TypeSize::Num, true,
        theme::kSOnSurf, 0, 0);
    Txt(m, RC(x0, y0 + LS(28), LS(500), LS(16)), L"常驻显示 · 位置微调 · 刷新与提醒",
        theme::TypeSize::Cap, false, theme::kSOnSurfVar, 0, 0);

    // 视口裁剪：滚动列表不得越过固定页头/页脚（与 ComputeDisp 的 vp 一致）
    const RECT vp{content.left, content.top + LS(52), content.right,
                  content.bottom - LS(28)};
    SaveDC(m);
    IntersectClipRect(m, vp.left, vp.top, vp.right, vp.bottom);

    // ---- macOS 分组卡片绘制原语 ----
    auto SecTitle = [&](const RECT& rc, const wchar_t* t) {
        if (rc.bottom <= rc.top) return;
        Txt(m, rc, t, theme::TypeSize::Body, true, theme::kSOnSurfVar, 0, 1);
    };
    auto CardBg = [&](const RECT& card) {
        if (card.bottom <= card.top) return;
        FillRr(m, card, 12, theme::kSSurf);
    };
    // 行标签：统一 Body 居左（卡内左缩进 20，右留控件区）
    auto RowLabel = [&](const RECT& row, const wchar_t* t, bool dim = false) {
        Txt(m, RECT{row.left + LS(20), row.top, row.right - LS(16), row.bottom}, t,
            theme::TypeSize::Body, false, dim ? theme::kSOutline : theme::kSOnSurf, 0, 1);
    };
    // 行间细分隔线（左缩进 20 对齐文字；模式行传 48 对齐文字起点）
    auto Sep = [&](const RECT& prev_row, int left_inset = 20) {
        HLinePx(m, prev_row.left + LS(left_inset), prev_row.right - LS(16),
                prev_row.bottom, Blend(theme::kSOutlineVar, theme::kSSurf, 120));
    };
    // 分段选择（统一 240×28 胶囊组；文本居中，选中项主题胶囊）
    auto Seg = [&](const RECT& box, int cells, int sel, int id_base,
                   const wchar_t* const* names) {
        FillRr(m, box, 8, theme::kSSurfLow2);
        const int bw = (box.right - box.left - LS(4)) / cells;
        for (int i = 0; i < cells; ++i) {
            RECT cell{box.left + LS(2) + i * bw, box.top + LS(2),
                      box.left + LS(2) + (i + 1) * bw, box.bottom - LS(2)};
            if (i == cells - 1) cell.right = box.right - LS(2);
            const bool on = i == sel;
            if (on) FillRr(m, cell, 6, theme::kSPrimCont);
            Txt(m, cell, names[i], theme::TypeSize::Body, on,
                on ? theme::kSOnPrimCont : theme::kSOnSurfVar, 1, 1);
            AddHit(cell, id_base + i);
        }
    };

    // ---- 显示模式 ----
    SecTitle(L.sec[0], L"显示模式");
    CardBg(L.card[0]);
    for (int i = 0; i < 4; ++i) {
        const RECT& rc = L.mode[i];
        const bool sel = d.display.mode == kModes[i].mode;
        RECT hov{rc.left + LS(6), rc.top + LS(3), rc.right - LS(6), rc.bottom - LS(3)};
        if (sel) {
            FillRr(m, hov, 8, Blend(theme::kSPrimCont, theme::kSSurf, 40));
        } else if (Hot(kWMode0 + i)) {
            FillRr(m, hov, 8, theme::kSSurfLow2);
        }
        float rcy = (rc.top + rc.bottom) / 2.0f;
        theme::FillEllipsePx(m, (float)rc.left + LS(30), rcy, (float)LS(16),
                             sel ? theme::kSPrimary : theme::kSSurfHigh2, 255);
        if (sel)
            theme::FillEllipsePx(m, (float)rc.left + LS(30), rcy, (float)LS(6),
                                 theme::kSSurf, 255);
        COLORREF tc = sel ? theme::kSPrimary : theme::kSOnSurf;
        Txt(m, RECT{rc.left + LS(48), rc.top + LS(8), rc.right - LS(110), rc.top + LS(28)},
            kModes[i].t, theme::TypeSize::Body, sel, tc, 0, 0);
        Txt(m, RECT{rc.left + LS(48), rc.top + LS(28), rc.right - LS(110),
                    rc.top + LS(47)},
            kModes[i].d, theme::TypeSize::Cap, false, theme::kSOutline, 0, 0);
        if (kModes[i].rec) {
            const wchar_t* rec = L"推荐";
            SIZE rs2 = Measure(m, rec, theme::TypeSize::Cap, true, theme::kSPrimary);
            RECT rc2 = RC(rc.right - rs2.cx - LS(26), (int)rcy - LS(9), rs2.cx + LS(10),
                          LS(18));
            FillRr(m, rc2, 4, Blend(theme::kSPrimary, theme::kSSurf, 55));
            Txt(m, rc2, rec, theme::TypeSize::Cap, true, theme::kSPrimary, 1, 1);
        }
        AddHit(rc, kWMode0 + i);
        if (i < 3) Sep(rc, 48);
    }

    // ---- 外观 ----
    SecTitle(L.sec[1], L"外观");
    CardBg(L.card[1]);
    RowLabel(L.theme_label, L"主题");
    {
        static const wchar_t* const names[3] = {L"跟随系统", L"深色", L"浅色"};
        Seg(L.theme_seg, 3, d.ui_theme == 2 ? 2 : (d.ui_theme == 1 ? 1 : 0), kWTheme0,
            names);
    }
    Sep(L.theme_label);
    if (L.embed) {
        RowLabel(L.embed_note, L"嵌入模式固定单行渲染，排版与字号不适用", true);
        Sep(L.embed_note);
    } else {
        RowLabel(L.layout_label, L"排版布局");
        {
            RECT box{L.seg_d.left - LS(2), L.seg_d.top - LS(2), L.seg_s.right + LS(2),
                     L.seg_s.bottom + LS(2)};
            static const wchar_t* const names[2] = {L"双行堆叠", L"单行紧凑"};
            Seg(box, 2, d.display.two_lines ? 0 : 1, kWLayout2, names);
        }
        Sep(L.layout_label);
        RowLabel(L.font_label, L"字体字号");
        wchar_t fv[24];
        swprintf_s(fv, L"%d pt", d.display.font_size_pt);
        Txt(m, L.font_val, fv, theme::TypeSize::Body, false, theme::kSOnSurfVar, 1, 1);
        DrawIconBtn(m, L.font_dec, IcMinus, theme::kSOnSurf, kWFontDec, theme::kSSurfHigh,
                    theme::kSOnSurf, 16);
        DrawIconBtn(m, L.font_inc, IcPlus, theme::kSOnSurf, kWFontInc, theme::kSSurfHigh,
                    theme::kSOnSurf, 16);
        Sep(L.font_label);
    }
    RowLabel(L.color_label, L"字色自适应");
    {
        static const wchar_t* const names[3] = {L"自动感知", L"纯白高反差",
                                                L"深墨极简"};
        int sel_color = d.display.text_color_mode == 2 ? 2
                        : (d.display.text_color_mode == 1 ? 1 : 0);
        Seg(L.color_seg, 3, sel_color, kWColor0, names);
    }
    Sep(L.color_label);
    RowLabel(L.lowc_label, L"低额度变色 · 剩余不足变琥珀，耗尽变红");
    DrawToggle(m, L.lowc_toggle, d.display.low_quota_color, theme::kSTertCont, kWLowColor);
    Sep(L.lowc_label);
    RowLabel(L.brand_label, L"账户名品牌色 · K / C / D / G 平台色");
    DrawToggle(m, L.brand_toggle, d.display.brand_color_names, theme::kSPrimCont,
               kWBrandName);
    Sep(L.brand_label);
    RowLabel(L.embicon_label, L"原厂图标 · 嵌入条名称与徽章改用厂商图标");
    DrawToggle(m, L.embicon_toggle, d.display.embed_brand_icons, theme::kSPrimCont,
               kWEmbIcons);
    if (L.embed) {
        Sep(L.embicon_label);
        RowLabel(L.embname_label, L"嵌入条显示名称行 · 关闭后只显示数值");
        DrawToggle(m, L.embname_toggle, d.display.embed_show_names, theme::kSSecondary,
                   kWEmbNames);
        Sep(L.embname_label);
        RowLabel(L.embwin_label, L"嵌入条窗口标签 · 5h / 周");
        DrawToggle(m, L.embwin_toggle, d.display.embed_show_windows, theme::kSSecondary,
                   kWEmbWindows);
    }

    // ---- 位置微调 ----
    if (L.pos_visible) {
        SecTitle(L.sec[2], L"位置微调");
        CardBg(L.card[2]);
        if (L.embed) {
            RowLabel(L.big_val, L"当前水平偏移");
            wchar_t bigv[24];
            swprintf_s(bigv, L"%+d DIP", slot);
            SIZE bv = Measure(m, bigv, theme::TypeSize::Num, true, theme::kSPrimary);
            Txt(m, RECT{L.b_zero.left - LS(12) - bv.cx, L.big_val.top,
                        L.b_zero.left - LS(12), L.big_val.bottom},
                bigv, theme::TypeSize::Num, true, theme::kSPrimary, 2, 1);
            DrawBtn(m, L.b_zero, L"置零", IcNone, StyleGhost(), kWPosZero);
            Sep(L.big_val);
            DrawSlider(m, RECT{L.slider.left + LS(20), L.slider.top + LS(7),
                               L.slider.right - LS(16), L.slider.top + LS(33)},
                       -200, 200, slot, theme::kSPrimary, kWPosSlider);
            Icon(m, IcKeyboard,
                 RC(L.kbd_line.left, L.kbd_line.top + LS(4), LS(14), LS(14)),
                 theme::kSOutline, 255);
            Txt(m, RECT{L.kbd_line.left + LS(22), L.kbd_line.top, L.kbd_line.right,
                        L.kbd_line.bottom},
                L"Alt + ←/→ 微移 1px · Shift + ←/→ 步进 4px · 即调即存",
                theme::TypeSize::Cap, false, theme::kSOutline, 0, 1);
        } else {
            if (L.free) {
                RowLabel(L.snap_row, L"吸附任务栏 · 贴任务栏上缘右端");
                DrawToggle(m, L.snap_toggle, d.display.bar_snap, theme::kSPrimCont,
                           kWBarSnap);
                Sep(L.snap_row);
            }
            RowLabel(L.x_label,
                     (L.free && !d.display.bar_snap) ? L"水平位置" : L"吸附水平偏移");
            {
                wchar_t xv[24];
                if (d.display.bar_snap || L.rings)
                    swprintf_s(xv, L"%+d DIP", d.display.bar_snap_x_dip);
                else
                    swprintf_s(xv, L"%d DIP", d.display.free_pos_x_dip);
                Txt(m, L.x_val, xv, theme::TypeSize::Num, true, theme::kSPrimary, 2, 1);
            }
            Sep(L.x_label);
            if (d.display.bar_snap || L.rings) {
                DrawSlider(m, RECT{L.x_slider.left + LS(20), L.x_slider.top + LS(7),
                                   L.x_slider.right - LS(16), L.x_slider.top + LS(33)},
                           -200, 200, d.display.bar_snap_x_dip, theme::kSPrimary,
                           kWBarSnapXSlider);
            } else {
                DrawSlider(m, RECT{L.x_slider.left + LS(20), L.x_slider.top + LS(7),
                                   L.x_slider.right - LS(16), L.x_slider.top + LS(33)},
                           0, 2000, d.display.free_pos_x_dip, theme::kSPrimary,
                           kWPosXSlider);
                Sep(L.x_slider);
                RowLabel(L.y_label, L"垂直位置");
                {
                    wchar_t yv[24];
                    swprintf_s(yv, L"%d DIP", d.display.free_pos_y_dip);
                    Txt(m, L.y_val, yv, theme::TypeSize::Num, true, theme::kSPrimary, 2,
                        1);
                }
                Sep(L.y_label);
                DrawSlider(m, RECT{L.y_slider.left + LS(20), L.y_slider.top + LS(7),
                                   L.y_slider.right - LS(16), L.y_slider.top + LS(33)},
                           0, 1200, d.display.free_pos_y_dip, theme::kSPrimary,
                           kWPosYSlider);
            }
            Icon(m, IcKeyboard,
                 RC(L.pos_hint.left, L.pos_hint.top + LS(4), LS(14), LS(14)),
                 theme::kSOutline, 255);
            Txt(m, RECT{L.pos_hint.left + LS(22), L.pos_hint.top, L.pos_hint.right,
                        L.pos_hint.bottom},
                L"按住 Ctrl 悬停常驻条可直接拖动 · 松手即存，靠近任务栏自动吸附",
                theme::TypeSize::Cap, false, theme::kSOutline, 0, 1);
        }
    }

    // ---- 刷新与提醒 ----
    SecTitle(L.sec[3], L"刷新与提醒");
    CardBg(L.card[3]);
    RowLabel(L.int_label, L"刷新间隔");
    FillRr(m, L.int_edit, 6, theme::kSSurfLow2, d.query.power_save ? 120 : 255);
    Txt(m, RECT{L.int_edit.right + LS(6), L.int_edit.top, L.int_edit.right + LS(30),
                L.int_edit.bottom},
        L"秒", theme::TypeSize::Body, false, theme::kSOnSurfVar, 0, 1);
    Sep(L.int_label);
    DrawSlider(m, RECT{L.int_slider.left + LS(20), L.int_slider.top + LS(6),
                       L.int_slider.right - LS(16), L.int_slider.top + LS(32)},
               60, 3600, d.query.power_save ? 900 : d.query.interval_sec,
               theme::kSPrimary, kWIntSlider, !d.query.power_save);
    Sep(L.int_slider);
    RowLabel(L.row_eco, L"节能模式 · 空闲时固定 900 秒刷新");
    DrawToggle(m, L.eco_toggle, d.query.power_save, theme::kSSecondary, kWEco);
    Sep(L.row_eco);
    RowLabel(L.row_notify, L"低额度预警 · 剩余不足时提醒");
    DrawToggle(m, L.notify_toggle, d.notify.enabled, theme::kSTertCont, kWNotify);
    if (L.thr_visible) {
        Sep(L.row_notify);
        RowLabel(L.thr_k, L"Kimi / GLM 剩余 ≤");
        FillRr(m, L.thr_k_edit, 6, theme::kSSurfLow2);
        Txt(m, RECT{L.thr_k_edit.right + LS(6), L.thr_k_edit.top,
                    L.thr_k_edit.right + LS(30), L.thr_k_edit.bottom},
            L"%", theme::TypeSize::Body, false, theme::kSOutline, 0, 1);
        Sep(L.thr_k);
        RowLabel(L.thr_d, L"DeepSeek 余额 ≤");
        FillRr(m, L.thr_d_edit, 6, theme::kSSurfLow2);
        Txt(m, RECT{L.thr_d_edit.right + LS(6), L.thr_d_edit.top,
                    L.thr_d_edit.right + LS(30), L.thr_d_edit.bottom},
            L"¥", theme::TypeSize::Body, false, theme::kSOutline, 0, 1);
    }

    // ---- 启动与数据 ----
    SecTitle(L.sec[4], L"启动与数据");
    CardBg(L.card[4]);
    RowLabel(L.row_auto, L"开机自启动");
    DrawToggle(m, L.auto_toggle, d.query.autostart, theme::kSPrimCont, kWAutoStart);
    Sep(L.row_auto);
    RowLabel(L.row_clear, L"清除本机配置 / 密钥 / 缓存 / 历史");
    {
        const bool hov = Hot(kWClearData), dwn = Down(kWClearData);
        if (hov || dwn)
            FillRr(m, L.clear_btn, 8,
                   Blend(theme::kSErrCont, theme::kSSurfLow, dwn ? 90 : 60));
        else
            StrokeRr(m, L.clear_btn, 8, Blend(theme::kSErrCont, theme::kSSurfLow, 150),
                     200, 1.0f);
        Icon(m, IcTrash,
             RC(L.clear_btn.left + LS(10),
                (L.clear_btn.top + L.clear_btn.bottom) / 2 - LS(7), LS(14), LS(14)),
             theme::kSError, 255);
        Txt(m, RECT{L.clear_btn.left + LS(28), L.clear_btn.top,
                    L.clear_btn.right - LS(4), L.clear_btn.bottom},
            L"清除数据", theme::TypeSize::Body, false, theme::kSError, 1, 1);
        AddHit(L.clear_btn, kWClearData);
    }

    RestoreDC(m, -1); // 结束视口裁剪（页脚动作条固定不滚）

    // ---- 页脚动作条内容（背景条由壳层先画）：脏草稿指示 + 放弃变更 + 保存设置 ----
    {
        int fy2 = content.bottom - LS(20);
        int W = content.right + LS(28);
        const bool dirty = DisplayDraftDirty(h, app->Cfg());
        if (dirty) {
            // 脏草稿指示（琥珀点+文案）：改动未保存时可见，干净态不占视觉
            Dot(m, content.left + LS(5), fy2 + LS(18), 7, theme::kSTertiary);
            Txt(m, RC(content.left + LS(14), fy2, content.left + LS(154), LS(36)),
                L"有未保存的更改", theme::TypeSize::Cap, false, theme::kSTertiary, 0, 1);
        }
        // 保存键反馈态：点击后短暂变形成「已保存」/「保存失败」（修"保存无回显"）
        const wchar_t* slabel = L"保存设置";
        BtnStyle pst = StylePrimary();
        if (g_save_fb.kind == 1) {
            slabel = L"已保存";
            pst = {theme::kSSecCont, theme::kSSecCont, theme::kSSecCont,
                   theme::kSOnPrimCont, 0};
        } else if (g_save_fb.kind == 2) {
            slabel = L"保存失败";
            pst = {theme::kSErrCont, theme::kSErrCont, theme::kSErrCont, theme::kSError, 0};
        }
        SIZE sb = Measure(m, slabel, theme::TypeSize::Cap, true, theme::kSOnPrimCont);
        RECT bsave = RC(W - LS(24) - sb.cx - LS(52), fy2, sb.cx + LS(52), LS(36));
        DrawBtn(m, bsave, slabel, IcSave, pst, kWSaveCfg, theme::TypeSize::Cap, true, 10);
        RECT bdis = RC(bsave.left - LS(10) - LS(104), fy2, LS(104), LS(36));
        if (dirty) {
            DrawBtn(m, bdis, L"放弃变更", IcNone, StyleGhost(), kWDiscardCfg,
                    theme::TypeSize::Cap, false, 10);
        } else {
            // 干净态置灰占位且不注册命中：无改动可放弃，点了也不该有反应
            FillRr(m, bdis, 8, theme::kSSurf);
            StrokeRr(m, bdis, 8, theme::kSCardBorder, 120, 1.0f);
            Txt(m, bdis, L"放弃变更", theme::TypeSize::Cap, false, theme::kSOutline, 1, 1,
                nullptr, 120);
        }
    }
}


} // namespace qb
