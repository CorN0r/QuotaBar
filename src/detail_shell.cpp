// detail_shell.cpp — 主窗口壳（2026-09-19 拆分终局）：标题栏/侧栏/内容绘制、
// DetailProc（含悬停状态机与滚轮/拖拽）、HandleAction 命令分发、TrayUi 页面管理
// （SetPage/RebuildPage/Relayout/CreatePageControls/OnCommand/PreTranslate）.
// 绘制矩形一律 ComputeQuota/ComputeDisp 单一来源产出，壳只消费。
#include "tray_ui.hpp"
#include "tray_ui_internal.hpp"
#include "app.hpp"
#include "providers.hpp"
#include "log.hpp"
#include "util.hpp"
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <stdio.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace qb {

using ui::QuotaCardR;   // Relayout 消费 g_qlayout.cards
using ui::QuotaLayout;  // Relayout 局部 Lq
using ui::DispLayout;   // Relayout/HandleAction 消费 ComputeDisp 产出

const wchar_t* PageTitle(int page) {
    switch (page) {
        case kPageQuota: return L"额度详情";
        case kPageDisplay: return L"显示设置";
    }
    return L"";
}

// 获取说明（类型相关；添加面板与卡片展开区共用）
void ShowHelpForType(HWND h, AccountType t) {
    const wchar_t* msg;
    const wchar_t* title;
    switch (t) {
        case AccountType::Deepseek:
            msg = L"登录 platform.deepseek.com，进入「API Keys」页面生成全新 Key。"
                  L"QuotaBar 将通过只读鉴权查询 /user/balance 接口实时展示当前账户可用额度。";
            title = L"DeepSeek 获取说明";
            break;
        case AccountType::Glm:
            msg = L"前往 bigmodel.cn 开放平台，在右上角个人中心进入「API Keys」页面。"
                  L"复制主应用或专属 Coding Plan 的 API Key 并贴入此处。";
            title = L"智谱 GLM 获取说明";
            break;
        case AccountType::Codex:
            msg = L"Codex 免填 Key：在终端运行 codex auth login 完成 ChatGPT 登录，"
                  L"QuotaBar 自动只读 ~/.codex/auth.json（令牌由 Codex CLI 维护刷新）。";
            title = L"Codex 免密直连说明";
            break;
        default:
            msg = L"前往 Moonshot 开放平台控制台（kimi.com/code），进入「API 密钥」管理中心，"
                  L"点击创建新的 Secret Key。请确保该密钥具有 Coding Plan 额度扣除权限。";
            title = L"Kimi Code 获取说明";
            break;
    }
    MessageBoxW(h, msg, title, MB_OK | MB_ICONINFORMATION);
}

// 单张账户卡片（含展开设置区）。vis_i = 可见列表序（动态 id 基准）。
// 布局类型与计算核心已抽出 disp_layout.cpp（纯函数，selftest 覆盖）；
// 此处 using 保持 tray_ui 内引用不变（Compute* 单一来源契约延续）。
using ui::DispLayout;
int g_dscroll = 0; // 显示设置页滚动偏移（px；ComputeDisp 钳制后统一应用）

// =========================================================
// 窗口壳：标题栏 + 侧栏（stitch_minimalist：h-11 标题栏 / w-56 侧栏）
// =========================================================
void PaintTitleBar(HDC m, App* app, int client_w) {
    RECT full{0, 0, client_w, LS(kTitleDip)};
    SetDCBrushColor(m, theme::kSSurfLow);
    FillRect(m, &full, (HBRUSH)GetStockObject(DC_BRUSH));
    HLinePx(m, 0, client_w, LS(kTitleDip), theme::kSCardBorder);
    // 品牌：环形渐变 logo + 等宽字标；状态签仅异常时出现（暂停中）——正常态保持安静
    RECT logo = RC(LS(16), LS(10), LS(24), LS(24));
    DrawLogo(m, logo);
    Txt(m, RECT{logo.right + LS(10), 0, logo.right + LS(150), LS(kTitleDip)}, L"QuotaBar",
        theme::TypeSize::Cap, true, theme::kSOnSurf, 0, 1, theme::kFontMono);
    bool paused = app->Cfg().query.pause;
    if (paused) {
        const wchar_t* live = L"已暂停查询";
        COLORREF live_c = theme::kSTertiary;
        SIZE ls2 = Measure(m, live, theme::TypeSize::Mini, true, live_c);
        RECT lpill = RC(logo.right + LS(150), LS(14), ls2.cx + LS(30), LS(22));
        FillRr(m, lpill, 11, Blend(live_c, theme::kSSurfLow, 40));
        StrokeRr(m, lpill, 11, Blend(live_c, theme::kSSurfLow, 120), 150, 1.0f);
        Dot(m, lpill.left + LS(10), (lpill.top + lpill.bottom) / 2, 6, live_c);
        Txt(m, RECT{lpill.left + LS(16), lpill.top, lpill.right - LS(6), lpill.bottom}, live,
            theme::TypeSize::Mini, true, live_c, 0, 1);
    }
    // 右侧：最小化/最大化/关闭（32×28；关闭悬停红底白叉）
    int right = client_w - LS(8);
    DrawIconBtn(m, RC(right - LS(32), LS(8), LS(32), LS(28)), IcClose, theme::kSOnSurfVar,
                kWClose, RGB(0xE1, 0x1D, 0x48), RGB(255, 255, 255), 14);
    right -= LS(34);
    DrawIconBtn(m, RC(right - LS(32), LS(8), LS(32), LS(28)),
                g_maximized ? IcRestore : IcMax, theme::kSOnSurfVar, kWMax, theme::kSSurfHigh,
                theme::kSOnSurf, 14);
    right -= LS(34);
    DrawIconBtn(m, RC(right - LS(32), LS(8), LS(32), LS(28)), IcMin, theme::kSOnSurfVar,
                kWMin, theme::kSSurfHigh, theme::kSOnSurf, 14);
}

void PaintSidebar(HDC m, App* app, int client_w, int client_h, int page) {
    const bool paused = app->Cfg().query.pause;
    const int nw = NavW(client_w);
    const bool rail = RailNav(client_w);
    RECT navr{0, LS(kTitleDip), nw, client_h};
    SetDCBrushColor(m, theme::kSSurfLow);
    FillRect(m, &navr, (HBRUSH)GetStockObject(DC_BRUSH));
    RECT vline{nw - 1, LS(kTitleDip), nw, client_h};
    SetDCBrushColor(m, theme::kSCardBorder);
    FillRect(m, &vline, (HBRUSH)GetStockObject(DC_BRUSH));
    struct NavDef { int icon; const wchar_t* t; };
    const NavDef nav[2] = {{IcDonut, L"额度详情"}, {IcTune, L"显示设置"}};
    int y = LS(kTitleDip) + LS(16);
    for (int i = 0; i < 2; ++i) {
        RECT ir = rail ? RC((nw - LS(40)) / 2, y, LS(40), LS(40))
                       : RC(LS(12), y, nw - LS(24), LS(40));
        int id = kWNav0 + i;
        if (i == page) {
            // 激活项：蓝-600 低透明底 + 蓝描边 + 蓝字（stitch：blue-600/15 + border/25）
            FillRr(m, ir, 10, Blend(theme::kSPrimCont, theme::kSSurfLow, 40));
            StrokeRr(m, ir, 10, Blend(theme::kSPrimary, theme::kSSurfLow, 90), 150, 1.0f);
        } else if (Hot(id) && Down(id)) {
            FillRr(m, ir, 10, theme::kSSurfHigh2);
        } else if (Hot(id)) {
            FillRr(m, ir, 10, theme::kSSurf);
        }
        COLORREF fg = i == page ? theme::kSPrimary : theme::kSOnSurfVar;
        Icon(m, nav[i].icon,
             rail ? RC(ir.left + ((ir.right - ir.left) - LS(16)) / 2,
                       (ir.top + ir.bottom) / 2 - LS(8), LS(16), LS(16))
                  : RC(ir.left + LS(14), (ir.top + ir.bottom) / 2 - LS(8), LS(16), LS(16)),
             fg, 255);
        if (!rail)
            Txt(m, RECT{ir.left + LS(40), ir.top, ir.right - LS(10), ir.bottom}, nav[i].t,
                theme::TypeSize::Cap, i == page, fg, 0, 1);
        AddHit(ir, id);
        y += LS(40) + LS(6);
    }
    // 侧栏页脚：版本号（左）+ 在线/暂停状态（右）；窄栏（图标条）不放页脚
    if (!rail) {
        const int foot_h = LS(44);
        HLinePx(m, LS(12), nw - LS(12), client_h - foot_h, theme::kSBorderSoft);
        Txt(m, RC(LS(16), client_h - foot_h + LS(14), LS(90), LS(18)),
            AppVersionText().c_str(), theme::TypeSize::Mini, false, theme::kSOutline, 0, 0,
            theme::kFontMono);
        COLORREF sc = paused ? theme::kSTertiary : theme::kSSecondary;
        const wchar_t* sctx = paused ? L"已暂停" : L"在线";
        SIZE ss = Measure(m, sctx, theme::TypeSize::Mini, false, sc);
        int tx = nw - LS(16) - ss.cx;
        Dot(m, tx - LS(9), client_h - foot_h + LS(14) + LS(7), 6, sc);
        Txt(m, RECT{tx, client_h - foot_h + LS(14), nw - LS(12),
                    client_h - foot_h + LS(14) + LS(18)},
            sctx, theme::TypeSize::Mini, false, sc, 0, 0);
    }
}

// 回显提示条：窗口底部居中的短暂胶囊（UiEcho 设置内容，kTimerEcho 驱动过期淡出）。
// 壳层最后叠加绘制、不注册命中——瞬时提示不得劫持其下控件的点击。
void PaintEchoPill(HDC m, int client_w, int client_h) {
    if (g_echo.text.empty()) return;
    const int64_t remain = g_echo.until - NowMono();
    if (remain <= 0) return;
    const int alpha = remain < 300 ? (int)(255 * remain / 300) : 255; // 尾段 300ms 淡出
    const COLORREF accent = g_echo.kind == kEchoOk    ? theme::kSSecondary
                            : g_echo.kind == kEchoErr ? theme::kSError
                                                      : theme::kSPrimary;
    SIZE ts = Measure(m, g_echo.text.c_str(), theme::TypeSize::Cap, false, theme::kSOnSurf);
    const int pw = ts.cx + LS(46), ph = LS(34);
    RECT pr{};
    pr.left = (client_w - pw) / 2;
    pr.right = pr.left + pw;
    pr.bottom = client_h - LS(62); // 显示页页脚条(52 DIP)上方留 10 DIP；额度页悬于底部
    pr.top = pr.bottom - ph;
    FillRr(m, pr, 17, Blend(theme::kSSurfHigh2, theme::kSbg, 242), alpha);
    StrokeRr(m, pr, 17, Blend(accent, theme::kSbg, 170), alpha, 1.0f);
    Dot(m, pr.left + LS(17), (pr.top + pr.bottom) / 2, 7, accent, alpha);
    Txt(m, RECT{pr.left + LS(29), pr.top, pr.right - LS(12), pr.bottom},
        g_echo.text.c_str(), theme::TypeSize::Cap, false, theme::kSOnSurf, 0, 1, nullptr,
        alpha);
}

void PaintDetail(HWND h, HDC hdc, const TrayUi* self) {
    RECT rc{};
    GetClientRect(h, &rc);
    // 页面内容区（paint 与 Relayout 共用同一矩形，保证输入框与自绘底对齐）
    RECT content{NavW(rc.right) + LS(28), LS(kTitleDip) + LS(24), rc.right - LS(28),
                 rc.bottom - LS(24)};
    // 双缓冲：整帧绘入内存位图一次上屏（分步直绘会被 DWM 采样到半成品帧）
    static HDC mem = nullptr;
    static HBITMAP bmp = nullptr, bmp_old = nullptr;
    static int buf_w = 0, buf_h = 0;
    if (!mem || buf_w != rc.right || buf_h != rc.bottom) {
        if (bmp) {
            SelectObject(mem, bmp_old);
            DeleteObject(bmp);
            bmp = nullptr;
        }
        if (mem) {
            DeleteDC(mem);
            mem = nullptr;
        }
        HDC screen = GetDC(h);
        mem = CreateCompatibleDC(screen);
        bmp = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
        ReleaseDC(h, screen);
        bmp_old = (HBITMAP)SelectObject(mem, bmp);
        buf_w = rc.right;
        buf_h = rc.bottom;
    }
    RECT full{0, 0, buf_w, buf_h};
    SetDCBrushColor(mem, theme::kSbg);
    FillRect(mem, &full, (HBRUSH)GetStockObject(DC_BRUSH));

    App* app = (App*)GetWindowLongPtrW(GetWindow(h, GW_OWNER), GWLP_USERDATA);
    g_hits.clear();
    g_hit_clip = RECT{}; // 防御：页面绘制异常早退时残留裁剪会吞掉后续帧的命中注册
    g_links.clear();
    g_maximized = IsZoomed(h) != FALSE;
    const int page = self ? self->page() : kPageQuota;
    if (!app) {
        BitBlt(hdc, 0, 0, buf_w, buf_h, mem, 0, 0, SRCCOPY);
        return;
    }
    // 命中区域随页设置：内容区恒记录；额度页=滚动视口可点（页头带豁免，页头按钮
    // 在视口上方）；显示页=列表视口+页脚
    g_hit_content = content;
    if (page == kPageQuota) {
        // 与 PaintQuota 的滚动视口同一矩形：滚到页头/标题栏底下的卡片不可点
        g_hit_vp = RECT{content.left, content.top + LS(kQuotaHeadDip), content.right,
                        content.bottom};
        // 固定页头（隐藏 chip/刷新/添加卡片）在视口上方 → 头带豁免区保持可点
        g_hit_extra = RECT{content.left, content.top, content.right,
                           content.top + LS(kQuotaHeadDip)};
    } else {
        g_hit_vp = RECT{content.left, content.top + LS(52), content.right,
                        content.bottom - LS(28)};
        g_hit_extra = RECT{content.left, content.bottom - LS(28), content.right,
                           content.bottom};
    }
    // 显示页页脚条背景（页面按钮画在其上）
    if (page == kPageDisplay) {
        RECT bar{NavW(buf_w), buf_h - LS(52), buf_w, buf_h};
        SetDCBrushColor(mem, Blend(theme::kSSurfLow, theme::kSbg, 235));
        FillRect(mem, &bar, (HBRUSH)GetStockObject(DC_BRUSH));
        HLinePx(mem, bar.left, bar.right, bar.top, theme::kSCardBorder);
    }
    PaintTitleBar(mem, app, buf_w);
    PaintSidebar(mem, app, buf_w, buf_h, page);

    switch (page) {
        case kPageQuota: PaintQuota(mem, app, content, h); break;
        case kPageDisplay: PaintDisplay(mem, app, content, h); break;
        default: break;
    }
    PaintEchoPill(mem, buf_w, buf_h); // 动作回显最上层叠加（瞬时，不注册命中）
    BitBlt(hdc, 0, 0, buf_w, buf_h, mem, 0, 0, SRCCOPY);
}

// =========================================================
// 交互
// =========================================================
int SliderValueFrom(const RECT& rc, int vmin, int vmax, int x) {
    return ui::SliderValueFrom(rc, vmin, vmax, x, g_layout_dpi);
}

// 拖拽排序目标位（F3）：按其余卡片的中线判定插入槽（同行内再按水平中线分左右）。
// 入参 pt 与 QuotaLayout 矩形同为客户区物理像素（ComputeQuota 全程 LS 换算），无需再转 DIP。
// 判定核心在 ui_layout.cpp（纯函数，selftest 覆盖）。
int ComputeDragTarget(POINT pt, int from_vis) {
    return ui::ComputeDragTarget(pt, from_vis, g_qlayout.cards);
}

void ApplySlotLive(TrayUi* self, int v) {
    App* app = self->AppPtr();
    if (!app) return;
    g_draft.slot = std::min(std::max(v, -200), 200);
    app->SetSlotOffsetDip(g_draft.slot); // 实时预览
}
void CommitSlot(TrayUi* self) {
    App* app = self->AppPtr();
    if (!app) return;
    // 草稿同步:否则「保存设置」提交进页快照的旧偏移,把实时调整拽回去(实测左跳)
    g_draft.cfg.display.slot_offset_x_dip = g_draft.slot;
    if (!app->CommitSlotOffsetDip(g_draft.slot)) // 显式命令落盘(不再经 CfgRef 直写)
        UiEcho(self->detail_hwnd(), kEchoErr, L"位置已应用，但写入配置失败");
}
void NudgeSlot(TrayUi* self, int delta) {
    ApplySlotLive(self, g_draft.slot + delta);
    CommitSlot(self);
    if (HWND d = self->detail_hwnd()) InvalidateRect(d, nullptr, FALSE);
}

// ---- 显示页滑杆取值域与实时应用（2026-09-21 浮条位置/吸附滑杆加入） ----
bool IsSlideId(int id) {
    return id == kWIntSlider || id == kWPosSlider || id == kWPosXSlider ||
           id == kWPosYSlider || id == kWBarSnapXSlider;
}
void SlideRange(int id, int& vmin, int& vmax) {
    vmin = 0;
    vmax = 100;
    switch (id) {
        case kWIntSlider: vmin = 60; vmax = 3600; break;
        case kWPosSlider: vmin = -200; vmax = 200; break;
        case kWPosXSlider: vmin = 0; vmax = 2000; break;    // 浮条自由位置 X
        case kWPosYSlider: vmin = 0; vmax = 1200; break;    // 浮条自由位置 Y
        case kWBarSnapXSlider: vmin = -200; vmax = 200; break; // 吸附水平偏移
        default: break;
    }
}
// 拖动中：草稿与运行值同步（不落盘），实时预览
void ApplySlideLive(TrayUi* self, int id, int v) {
    App* app = self->AppPtr();
    if (!app) return;
    const DisplayConfig& c = g_draft.cfg.display;
    switch (id) {
        case kWPosSlider: ApplySlotLive(self, v); break;
        case kWPosXSlider:
            g_draft.cfg.display.free_pos_x_dip = v;
            app->SetFreePosLive(v, c.free_pos_y_dip);
            break;
        case kWPosYSlider:
            g_draft.cfg.display.free_pos_y_dip = v;
            app->SetFreePosLive(c.free_pos_x_dip, v);
            break;
        case kWBarSnapXSlider:
            g_draft.cfg.display.bar_snap_x_dip = v;
            app->SetSnapLive(c.bar_snap, v);
            break;
        default: break;
    }
}
// 抬起：落盘（即拖即存；嵌入偏移走 CommitSlot 原路径）
void CommitSlide(TrayUi* self, int id) {
    App* app = self->AppPtr();
    if (!app) return;
    const DisplayConfig& c = g_draft.cfg.display;
    bool ok = true;
    switch (id) {
        case kWPosSlider: CommitSlot(self); return;
        case kWPosXSlider:
        case kWPosYSlider: ok = app->CommitFreePos(c.free_pos_x_dip, c.free_pos_y_dip); break;
        case kWBarSnapXSlider: ok = app->CommitSnap(c.bar_snap, c.bar_snap_x_dip); break;
        default: return;
    }
    if (!ok) UiEcho(self->detail_hwnd(), kEchoErr, L"位置已应用，但写入配置失败");
}

void OpenUtf8Url(HWND h, const char* url) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, url, -1, nullptr, 0);
    std::wstring wurl(wlen > 0 ? (size_t)wlen - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl.data(), wlen);
    ShellExecuteW(h, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// 剪贴板文本复制（卡片 Key 复制等共用）。返回 false=打开/写入失败
//（剪贴板被其他程序占用、内存不足等），调用方据此回显，不再静默。
bool CopyTextToClipboard(HWND h, const std::wstring& t) {
    if (!OpenClipboard(h)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
        if (g) {
            void* dst = GlobalLock(g);
            if (dst) {
                memcpy(dst, t.c_str(), (t.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(g);
                // 写入成功才移交系统所有权；失败自释放，不泄漏不交未初始化块
                if (SetClipboardData(CF_UNICODETEXT, g)) ok = true;
                else GlobalFree(g);
            } else {
                GlobalFree(g); // Lock 失败：不得把未初始化块交给剪贴板
            }
        }
    }
    CloseClipboard();
    return ok;
}

// 新建面板 Key 输入框掩码切换
void SetEditMask(HWND edit, bool masked) {
    if (!edit) return;
    SendMessageW(edit, EM_SETPASSWORDCHAR, masked ? (WPARAM)L'●' : 0, 0);
    InvalidateRect(edit, nullptr, TRUE);
}

void HandleAction(TrayUi* self, HWND h, int id) {
    App* app = self->AppPtr();
    if (!app) return;
    if (id >= kWNav0 && id <= kWNav1) {
        self->SetPage(id - kWNav0);
        return;
    }
    // 注意必须有上界：kWLink0 之后的 id 都是页面控件，误入此分支会被整体吞掉
    // （实测：无上界版本让所有页面按钮静默失效）
    if (id >= kWLink0 && id - kWLink0 < (int)g_links.size()) {
        PageLink link = g_links[id - kWLink0];
        if (link.url) OpenUtf8Url(h, link.url);
        return;
    }
    switch (id) {
        case kWRefresh: app->CmdRefreshAll(); return;
        case kWMin: ShowWindow(h, SW_MINIMIZE); return;
        case kWMax:
            ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
            return;
        case kWClose: self->CloseDetail(); return;
        case kWAddCard:
            g_q.add_open = !g_q.add_open;
            g_q.add_error.clear();
            g_q.add_mask = true;
            self->RebuildPage(); // 面板输入框随展开/收起创建/销毁
            return;
        case kWHiddenChip:
            g_q.hidden_open = !g_q.hidden_open;
            InvalidateRect(h, nullptr, FALSE);
            return;
        case kWAddCancel:
            g_q.add_open = false;
            g_q.add_error.clear();
            self->RebuildPage();
            return;
        case kWAddCreate: {
            const AccountType t = kAddTypes[g_q.add_type];
            if (t == AccountType::Codex && app->Cfg().HasType(AccountType::Codex)) {
                g_q.add_error = L"Codex 仅支持一张卡片（auth.json 唯一）";
                InvalidateRect(h, nullptr, FALSE);
                return;
            }
            std::wstring alias = EditTextW(GetDlgItem(h, kEditAddAlias));
            std::string key = EditTextUtf8(GetDlgItem(h, kEditAddKey));
            if (t != AccountType::Codex && Trim(key).empty()) {
                g_q.add_error = L"请输入 API Key（「获取说明」有获取方式）";
                InvalidateRect(h, nullptr, FALSE);
                return;
            }
            if (!app->AddAccount(t, alias, key)) {
                g_q.add_error = L"创建失败，请重试";
                InvalidateRect(h, nullptr, FALSE);
                return;
            }
            g_q.add_open = false;
            g_q.add_error.clear();
            ResetQuotaUiScroll();
            self->RebuildPage();
            return;
        }
        default: break;
    }
    if (id >= kWAddType0 && id <= kWAddType3) {
        g_q.add_type = id - kWAddType0;
        g_q.add_error.clear();
        self->RebuildPage(); // codex 类型无 Key 输入框 → 重建子控件
        return;
    }
    // 添加面板 Key 行按钮
    if (id == kWAddKeyEye) {
        g_q.add_mask = !g_q.add_mask;
        SetEditMask(GetDlgItem(h, kEditAddKey), g_q.add_mask);
        InvalidateRect(h, nullptr, FALSE);
        return;
    }
    if (id == kWAddKeyCopy) {
        std::wstring t = EditTextW(GetDlgItem(h, kEditAddKey));
        if (t.empty()) {
            MessageBoxW(h, L"没有可复制的密钥", L"QuotaBar", MB_OK | MB_ICONWARNING);
            return;
        }
        // 复制结果回显：剪贴板被占用等失败此前完全静默
        if (CopyTextToClipboard(h, t))
            UiEcho(h, kEchoOk, L"已复制到剪贴板");
        else
            UiEcho(h, kEchoErr, L"复制失败：剪贴板被其他程序占用");
        return;
    }
    if (id == kWAddKeyClear) {
        SetWindowTextW(GetDlgItem(h, kEditAddKey), L"");
        return;
    }
    // 账户卡片动态控件
    if (id >= kWAccBase && id < kWAccBase + kAccMaxCount * kAccStride) {
        const int rel = id - kWAccBase;
        const int idx = rel / kAccStride;
        const int sub = rel % kAccStride;
        // 已隐藏行「显示」按钮（idx = 隐藏列表序，子 id 与卡片控件错开）
        if (sub == kAccUnhide) {
            if (idx >= 0 && idx < (int)g_qlayout.hidden_idx.size()) {
                const auto& accs = app->Accounts();
                int ai = g_qlayout.hidden_idx[idx];
                // 布局产物可能先于本次绘制过期（删除账户后失效矩形未消费即来了
                // 命令）：下标必须对当前账户向量复验，否则越界读 UB
                if (ai >= 0 && ai < (int)accs.size()) {
                    if (!app->SetAccountVisible(accs[ai].cfg.id, true))
                        UiEcho(h, kEchoErr, L"已显示，但写入配置失败");
                }
                InvalidateRect(h, nullptr, FALSE);
            }
            return;
        }
        if (idx < 0 || idx >= (int)g_qlayout.vis_idx.size()) return;
        const auto& accs = app->Accounts();
        int ai = g_qlayout.vis_idx[idx];
        if (ai < 0 || ai >= (int)accs.size()) return; // 同上：布局滞后防御
        const AccountState& st = accs[ai];
        switch (sub) {
            case kAccEnable:
                // 启用开关即落盘：off → 停止查询 + 常驻条/圆环隐藏 + 卡片置灰
                if (!app->SetAccountEnabled(st.cfg.id, !st.cfg.enabled))
                    UiEcho(h, kEchoErr, L"已切换，但写入配置失败");
                InvalidateRect(h, nullptr, FALSE);
                return;
            case kAccEye:
                // 显隐开关：隐藏卡片，页头「已隐藏 (N)」区可找回
                if (!app->SetAccountVisible(st.cfg.id, false))
                    UiEcho(h, kEchoErr, L"已隐藏，但写入配置失败");
                InvalidateRect(h, nullptr, FALSE);
                return;
            case kAccSettings:
                g_q.SetExpanded(st.cfg.id, !g_q.IsExpanded(st.cfg.id));
                self->RebuildPage(); // 展开区输入框随展开创建
                return;
            case kAccSave: {
                std::wstring alias = EditTextW(GetDlgItem(h, kEditAccAlias0 + idx));
                std::string key = EditTextUtf8(GetDlgItem(h, kEditAccKey0 + idx));
                // 清空 Key=删除密钥（cred_rev 隔离、旧密钥不可恢复），与「删除卡片」
                // 同级危险操作：先确认。旧实现静默直接清掉，用户无感知丢失。
                if (st.cfg.type != AccountType::Codex && Trim(key).empty() &&
                    !app->GetAccountKey(st.cfg.id).empty()) {
                    std::wstring disp =
                        alias.empty()
                            ? Utf8ToWide(ProviderDefaultAlias(st.cfg.type))
                            : alias;
                    std::wstring msg = L"检测到 Key 输入框已清空。保存将删除「" + disp +
                                       L"」的密钥，旧密钥不可恢复。确定继续吗？";
                    if (MessageBoxW(h, msg.c_str(), L"QuotaBar — 删除密钥",
                                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                        return;
                }
                auto rc2 = app->UpdateAccount(st.cfg.id, alias, key);
                if (rc2 == App::AccountCommit::IoError) {
                    MessageBoxW(h, L"保存失败：磁盘或 DPAPI 加密写入出错，请重试。",
                                L"QuotaBar", MB_OK | MB_ICONERROR);
                } else if (rc2 == App::AccountCommit::NoChange) {
                    UiEcho(h, kEchoInfo, L"没有改动"); // 旧实现点击毫无反应
                } else {
                    self->RebuildPage(); // 回填规范化后的别名/掩码值
                    UiEcho(h, kEchoOk, L"已保存");
                }
                return;
            }
            case kAccDelete: {
                std::wstring alias = Utf8ToWide(
                    st.cfg.alias.empty() ? ProviderDefaultAlias(st.cfg.type) : st.cfg.alias);
                std::wstring msg =
                    L"将删除卡片「" + alias + L"」及其密钥与本地缓存数据。确定继续吗？";
                int r = MessageBoxW(h, msg.c_str(), L"QuotaBar — 删除卡片",
                                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                if (r == IDYES) {
                    g_q.SetExpanded(st.cfg.id, false);
                    app->RemoveAccount(st.cfg.id);
                    ResetQuotaUiScroll();
                    self->RebuildPage();
                }
                return;
            }
            case kAccKeyEye: {
                const bool masked = g_q.IsMasked(st.cfg.id);
                g_q.SetMask(st.cfg.id, !masked);
                SetEditMask(GetDlgItem(h, kEditAccKey0 + idx), !masked);
                InvalidateRect(h, nullptr, FALSE);
                return;
            }
            case kAccKeyCopy: {
                std::wstring t = EditTextW(GetDlgItem(h, kEditAccKey0 + idx));
                if (t.empty()) {
                    MessageBoxW(h, L"没有可复制的密钥", L"QuotaBar", MB_OK | MB_ICONWARNING);
                    return;
                }
                if (CopyTextToClipboard(h, t))
                    UiEcho(h, kEchoOk, L"已复制到剪贴板");
                else
                    UiEcho(h, kEchoErr, L"复制失败：剪贴板被其他程序占用");
                return;
            }
            case kAccKeyClear:
                SetWindowTextW(GetDlgItem(h, kEditAccKey0 + idx), L"");
                return;
            case kAccHelp:
                ShowHelpForType(h, st.cfg.type);
                return;
            case kAccCodexSync:
                app->CmdRefreshAll(); // codex 令牌每次请求即时读 auth.json
                return;
            case kAccAlert: {
                // 低额预警阈值档位菜单（即选即存；深色/浅色随系统主题）
                enum { kAlInherit = 200, kAlOff = 201, kAl50 = 202, kAl30, kAl20, kAl10,
                       kAl5 };
                HMENU m = CreatePopupMenu();
                int g = std::min(std::max(app->Cfg().notify.kimi_threshold_pct, 1), 99);
                wchar_t inh[40];
                swprintf_s(inh, L"继承全局（%d%%）", g);
                AppendMenuW(m, MF_STRING | (st.cfg.alert_pct == 0 ? MF_CHECKED : 0),
                            kAlInherit, inh);
                AppendMenuW(m, MF_STRING | (st.cfg.alert_pct == 1 ? MF_CHECKED : 0), kAlOff,
                            L"关闭预警");
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                struct AlertLevel { int v; int cmd; };
                const AlertLevel levels[5] = {{50, kAl50}, {30, kAl30}, {20, kAl20},
                                              {10, kAl10}, {5, kAl5}};
                for (const auto& lv : levels) {
                    wchar_t t[32];
                    swprintf_s(t, L"剩余 ≤ %d%%", lv.v);
                    AppendMenuW(m,
                                MF_STRING | (st.cfg.alert_pct == lv.v ? MF_CHECKED : 0),
                                lv.cmd, t);
                }
                SetForegroundWindow(h);
                POINT mp;
                GetCursorPos(&mp);
                int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                              mp.x, mp.y, 0, h, nullptr);
                DestroyMenu(m);
                PostMessageW(h, WM_NULL, 0, 0);
                int v = -1;
                switch (cmd) {
                    case kAlInherit: v = 0; break;
                    case kAlOff: v = 1; break;
                    case kAl50: v = 50; break;
                    case kAl30: v = 30; break;
                    case kAl20: v = 20; break;
                    case kAl10: v = 10; break;
                    case kAl5: v = 5; break;
                    default: return;
                }
                if (!app->SetAccountAlertPct(st.cfg.id, v))
                    UiEcho(h, kEchoErr, L"已选择，但写入配置失败");
                InvalidateRect(h, nullptr, FALSE);
                return;
            }
            default:
                return;
        }
    }
    if (id >= kWMode0 && id <= kWMode3) {
        // 与绘制共用 kModes 顺序表（嵌入任务栏置顶）；即调即存实时刷新常驻条
        Log::WriteF(LogLevel::Info, "ui", "mode click id=%d -> %d", id - kWMode0,
                    (int)kModes[id - kWMode0].mode);
        g_draft.cfg.display.mode = kModes[id - kWMode0].mode;
        if (!app->ApplyDisplayLive(g_draft.cfg.display))
            UiEcho(h, kEchoErr, L"已应用，但写入配置失败");
        if (self->detail_hwnd()) self->Relayout(); // 阈值行/位置区随模式增减
        InvalidateRect(h, nullptr, FALSE);
        return;
    }
    if (id >= kWColor0 && id <= kWColor2) {
        g_draft.cfg.display.text_color_mode = id - kWColor0;
        if (!app->ApplyDisplayLive(g_draft.cfg.display))
            UiEcho(h, kEchoErr, L"已应用，但写入配置失败");
        InvalidateRect(h, nullptr, FALSE);
        return;
    }
    if (id >= kWTheme0 && id <= kWTheme2) {
        // 主窗口主题（0=跟随系统 1=深色 2=浅色）：即选即换并落盘
        g_draft.cfg.ui_theme = id - kWTheme0;
        if (!app->SetUiThemeMode(g_draft.cfg.ui_theme))
            UiEcho(h, kEchoErr, L"已应用，但写入配置失败");
        InvalidateRect(h, nullptr, FALSE);
        return;
    }
    switch (id) {
        case kWLayout2:
            g_draft.cfg.display.two_lines = true;
            if (!app->ApplyDisplayLive(g_draft.cfg.display))
                UiEcho(h, kEchoErr, L"已应用，但写入配置失败");
            InvalidateRect(h, nullptr, FALSE);
            return;
        case kWLayout1:
            g_draft.cfg.display.two_lines = false;
            if (!app->ApplyDisplayLive(g_draft.cfg.display))
                UiEcho(h, kEchoErr, L"已应用，但写入配置失败");
            InvalidateRect(h, nullptr, FALSE);
            return;
        case kWFontDec:
            if (g_draft.cfg.display.font_size_pt > 7) --g_draft.cfg.display.font_size_pt;
            if (!app->ApplyDisplayLive(g_draft.cfg.display))
                UiEcho(h, kEchoErr, L"已应用，但写入配置失败");
            InvalidateRect(h, nullptr, FALSE);
            return;
        case kWFontInc:
            if (g_draft.cfg.display.font_size_pt < 18) ++g_draft.cfg.display.font_size_pt;
            if (!app->ApplyDisplayLive(g_draft.cfg.display))
                UiEcho(h, kEchoErr, L"已应用，但写入配置失败");
            InvalidateRect(h, nullptr, FALSE);
            return;
        case kWEco: {
            g_draft.cfg.query.power_save = !g_draft.cfg.query.power_save;
            HWND e = GetDlgItem(h, kInterval);
            if (e) {
                SetWindowTextW(e, std::to_wstring(g_draft.cfg.query.power_save
                                                      ? 900
                                                      : g_draft.cfg.query.interval_sec)
                                       .c_str());
                EnableWindow(e, !g_draft.cfg.query.power_save);
            }
            InvalidateRect(h, nullptr, FALSE);
            return;
        }
        case kWNotify:
            g_draft.cfg.notify.enabled = !g_draft.cfg.notify.enabled;
            self->Relayout(); // 阈值行/输入框随开关展开或收起
            InvalidateRect(h, nullptr, FALSE);
            return;
        case kWAutoStart:
            g_draft.cfg.query.autostart = !g_draft.cfg.query.autostart;
            InvalidateRect(h, nullptr, FALSE);
            return;
        case kWClearData: {
            int r = MessageBoxW(h, L"将删除 %LOCALAPPDATA%\\QuotaBar 下的配置、DPAPI 密钥、"
                                    L"缓存与日志。开机自启动保持当前状态。确定继续吗？",
                                L"QuotaBar — 清除本地数据",
                                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (r == IDYES) {
                app->CmdClearLocalData();
                ResetQuotaUiScroll();
                self->RebuildPage();
                UiEcho(h, kEchoOk, L"已清除本地数据，账户已复位为默认卡片");
            }
            return;
        }
        case kWDiscardCfg:
            self->RebuildPage(); // 草稿重置为当前配置，EDIT 随重建灌回
            return;
        case kWSaveCfg: {
            AppConfig cfg = g_draft.cfg;
            // 位置微调即调即存:保存时以实时值为准(草稿里的 slot 是进页快照,直接
            // 提交会把实时调整过的偏移拽回旧值,实测保存后左跳)
            cfg.display.slot_offset_x_dip = g_draft.slot;
            int iv = GetEditInt(GetDlgItem(h, kInterval), cfg.query.interval_sec);
            cfg.query.interval_sec = std::min(std::max(iv, 60), 3600);
            int kt = GetEditInt(GetDlgItem(h, kKimiThr), cfg.notify.kimi_threshold_pct);
            cfg.notify.kimi_threshold_pct = std::min(std::max(kt, 1), 99);
            double dt = GetEditDouble(GetDlgItem(h, kDsThr), cfg.notify.deepseek_threshold_cny);
            cfg.notify.deepseek_threshold_cny = std::min(std::max(dt, 0.5), 99999.0);
            const bool wanted_auto = cfg.query.autostart;
            const bool ok = app->CommitConfig(cfg);
            g_draft.cfg = app->Cfg(); // 提交后回拍运行值（CommitConfig 内含字段校正）
            // 钳制值回写输入框：所见即所存（旧实现框里显示"5"实际已存 60，
            // 直到下次页面重建才变，用户无从察觉）
            HWND e = GetDlgItem(h, kInterval);
            if (e) SetWindowTextW(e, std::to_wstring(g_draft.cfg.query.interval_sec).c_str());
            e = GetDlgItem(h, kKimiThr);
            if (e)
                SetWindowTextW(e, std::to_wstring(g_draft.cfg.notify.kimi_threshold_pct).c_str());
            e = GetDlgItem(h, kDsThr);
            if (e) {
                wchar_t vb[32];
                swprintf_s(vb, L"%.2f", g_draft.cfg.notify.deepseek_threshold_cny);
                SetWindowTextW(e, vb);
            }
            // 保存键变形反馈：成功「已保存」/ 失败「保存失败」（修"保存无回显"主诉求）
            UiSaveFeedback(h, ok);
            if (g_draft.cfg.query.autostart != wanted_auto)
                UiEcho(h, kEchoErr, L"「开机自启动」写入失败，该项未生效");
            InvalidateRect(h, nullptr, FALSE);
            return;
        }
        case kWPosZero:
            ApplySlotLive(self, 0);
            CommitSlot(self);
            InvalidateRect(h, nullptr, FALSE);
            return;
        // 浮条吸附开关（即调即存；Y 行/滑杆语义随吸附显隐切换）
        case kWBarSnap:
            g_draft.cfg.display.bar_snap = !g_draft.cfg.display.bar_snap;
            if (!app->ApplyDisplayLive(g_draft.cfg.display))
                UiEcho(h, kEchoErr, L"已应用，但写入配置失败");
            self->Relayout();
            InvalidateRect(h, nullptr, FALSE);
            return;
        // 外观开关组（即调即存，实时重绘常驻条/嵌入条）
        case kWLowColor:
            g_draft.cfg.display.low_quota_color = !g_draft.cfg.display.low_quota_color;
            if (!app->ApplyDisplayLive(g_draft.cfg.display))
                UiEcho(h, kEchoErr, L"已应用，但写入配置失败");
            InvalidateRect(h, nullptr, FALSE);
            return;
        case kWBrandName:
            g_draft.cfg.display.brand_color_names = !g_draft.cfg.display.brand_color_names;
            if (!app->ApplyDisplayLive(g_draft.cfg.display))
                UiEcho(h, kEchoErr, L"已应用，但写入配置失败");
            InvalidateRect(h, nullptr, FALSE);
            return;
        case kWEmbNames:
            g_draft.cfg.display.embed_show_names = !g_draft.cfg.display.embed_show_names;
            if (!app->ApplyDisplayLive(g_draft.cfg.display))
                UiEcho(h, kEchoErr, L"已应用，但写入配置失败");
            InvalidateRect(h, nullptr, FALSE);
            return;
        case kWEmbIcons:
            g_draft.cfg.display.embed_brand_icons = !g_draft.cfg.display.embed_brand_icons;
            if (!app->ApplyDisplayLive(g_draft.cfg.display))
                UiEcho(h, kEchoErr, L"已应用，但写入配置失败");
            InvalidateRect(h, nullptr, FALSE);
            return;
        case kWEmbWindows:
            g_draft.cfg.display.embed_show_windows = !g_draft.cfg.display.embed_show_windows;
            if (!app->ApplyDisplayLive(g_draft.cfg.display))
                UiEcho(h, kEchoErr, L"已应用，但写入配置失败");
            InvalidateRect(h, nullptr, FALSE);
            return;
        default: return;
    }
}

// 输入框底色画刷：按主题代数重建（F1；旧静态画刷在切换主题后会残留旧色）。
// gen 必须随 cache 成对存储：函数级 static gen 服务两个缓存时，主题切换后先到的
// 调用方把 gen 盖到最新值，后到的缓存永不重建（禁用框底色残留旧主题，实测）。
HBRUSH ThemeInputBrush(HBRUSH& cache, int& cache_gen) {
    if (!cache || cache_gen != theme::UiThemeGen()) {
        if (cache) DeleteObject(cache);
        cache = CreateSolidBrush(theme::kSSurfLow2);
        cache_gen = theme::UiThemeGen();
    }
    return cache;
}

LRESULT CALLBACK DetailProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    TrayUi* self = (TrayUi*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_ERASEBKGND: {
            HDC hdc = (HDC)w;
            RECT rc{};
            GetClientRect(h, &rc);
            SetDCBrushColor(hdc, theme::kSbg);
            FillRect(hdc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(h, &ps);
            PaintDetail(h, hdc, self);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_PRINT:
            if (l & PRF_CLIENT) PaintDetail(h, (HDC)w, self);
            break;
        case WM_SIZE:
            // g_layout_dpi 是全局量：跨显示器拖动/贴靠后若与窗口实际 DPI 脱钩
            // （DPICHANGED 未送达的路径），布局会按错误倍率绘制——此处按窗口
            // 实际 DPI 纠偏，输入框字体一并重建
            if (GetDpiForWindow(h) != (UINT)g_layout_dpi) {
                Log::WriteF(LogLevel::Info, "ui", "size dpi heal: g=%d win=%u size=%dx%d",
                            g_layout_dpi, GetDpiForWindow(h), LOWORD(l), HIWORD(l));
                SetLayoutDpi(h);
                ApplyEditFonts(h);
            }
            if (self) self->Relayout();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            SetLayoutDpi(h);
            ApplyEditFonts(h);
            LPRECT r = (LPRECT)l;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self) self->RebuildPage();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)l;
            int minw = LS(kWinMinWDip), minh = LS(kWinMinHDip);
            // Win11 分屏贴靠把窗口压到半屏宽：若最小宽超过工作区一半，窗口会被
            // 最小宽顶过中缝分屏线（实测右半屏多出一块）。把最小宽钳到目标显示器
            // 工作区一半以内、最小高钳到工作区高以内，保证贴靠严格对齐分屏线；
            // 页面布局自带窄窗退化分支（紧凑账户页/预览卡收起）。
            HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                minw = std::min(minw, (int)((mi.rcWork.right - mi.rcWork.left) / 2));
                minh = std::min(minh, (int)(mi.rcWork.bottom - mi.rcWork.top));
            }
            mmi->ptMinTrackSize.x = minw;
            mmi->ptMinTrackSize.y = minh;
            return 0;
        }
        case WM_NCCALCSIZE:
            // 客户区=整窗（去掉标准标题栏）；最大化时按系统边框内缩避免内容出屏
            if (!w) break;
            if (IsZoomed(h)) {
                RECT* r = (RECT*)l;
                UINT dpi = GetDpiForWindow(h);
                if (!dpi) dpi = 96;
                int f = GetSystemMetricsForDpi(SM_CXFRAME, dpi) +
                        GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                r->left += f;
                r->top += f;
                r->right -= f;
                r->bottom -= f;
            }
            return 0;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            RECT wr{};
            GetWindowRect(h, &wr);
            int x = pt.x - wr.left, y = pt.y - wr.top;
            int wpx = wr.right - wr.left, hpx = wr.bottom - wr.top;
            if (!IsZoomed(h)) {
                int b = LS(6);
                bool lft = x < b, rgt = x >= wpx - b, top = y < b, bot = y >= hpx - b;
                if (top && lft) return HTTOPLEFT;
                if (top && rgt) return HTTOPRIGHT;
                if (bot && lft) return HTBOTTOMLEFT;
                if (bot && rgt) return HTBOTTOMRIGHT;
                if (lft) return HTLEFT;
                if (rgt) return HTRIGHT;
                if (top) return HTTOP;
                if (bot) return HTBOTTOM;
            }
            if (y < LS(kTitleDip)) {
                // HitAt 用客户区坐标：WM_NCHITTEST 的 pt 是屏幕坐标，必须先转换，
                // 否则整个标题栏被判成 HTCAPTION，标题栏按钮全部不可点（实测）
                POINT cpt = pt;
                ScreenToClient(h, &cpt);
                if (HitAt(cpt) < 0) return HTCAPTION;
            }
            break;
        }
        case WM_NCACTIVATE:
            return DefWindowProcW(h, m, w, (LPARAM)-1);
        case WM_NCLBUTTONDBLCLK:
            if (w == HTCAPTION) {
                ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            g_cursor_pt = pt; // 卡片整体 hover 提亮按光标落点判定（与命中 id 解耦）
            // 卡级 hover 重绘：光标从空隙直接进/出卡片body不改变命中 id（-1→-1），
            // 只靠下面的 hover 变化触发重绘会让提亮迟迟不出现（实测 Codex 卡高亮
            // 不及时）——跨卡进出必须单独判定并失效整帧。g_qlayout 仅额度页有效
            if (self->page() == kPageQuota) {
                int card_at = -1;
                for (size_t ci = 0; ci < g_qlayout.cards.size(); ++ci)
                    if (PtInRect(&g_qlayout.cards[ci], pt)) { card_at = (int)ci; break; }
                if (card_at != g_card_hover) {
                    g_card_hover = card_at;
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
            if (g_slide_drag) {
                RECT rc = RectOfHit(g_slide_drag);
                int vmin = 0, vmax = 100;
                SlideRange(g_slide_drag, vmin, vmax);
                int v = SliderValueFrom(rc, vmin, vmax, pt.x);
                if (g_slide_drag == kWIntSlider) {
                    g_draft.cfg.query.interval_sec = v;
                    HWND e = GetDlgItem(h, kInterval);
                    if (e) SetWindowTextW(e, std::to_wstring(v).c_str());
                } else {
                    ApplySlideLive(self, g_slide_drag, v);
                }
                InvalidateRect(h, nullptr, FALSE);
            } else if (g_drag_from >= 0) {
                // 拖拽中：更新目标插入位
                g_drag_target = ComputeDragTarget(pt, g_drag_from);
                InvalidateRect(h, nullptr, FALSE);
            } else {
                int hv = HitAt(pt);
                if (hv != g_hover) {
                    g_hover = hv;
                    InvalidateRect(h, nullptr, FALSE);
                }
                SetCursor(LoadCursorW(nullptr, hv >= kWLink0 ? IDC_HAND : IDC_ARROW));
                // 拖拽起拖判定：按住卡片头部移动超过 6px 才进入拖拽（与点击区分）
                if (g_drag_press_vis >= 0 && g_pressed >= kWAccBase) {
                    int dx = pt.x - g_drag_start.x, dy = pt.y - g_drag_start.y;
                    if (abs(dx) > LS(6) || abs(dy) > LS(6)) {
                        g_drag_from = g_drag_press_vis;
                        g_drag_target = ComputeDragTarget(pt, g_drag_from);
                        g_drag_press_vis = -1;
                        g_hover = -1;
                        InvalidateRect(h, nullptr, FALSE);
                    }
                }
            }
            if (!g_tracked) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                if (TrackMouseEvent(&tme)) g_tracked = true;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracked = false;
            g_cursor_pt = POINT{-1, -1};
            if (g_hover != -1 || g_card_hover != -1) {
                g_hover = -1;
                g_card_hover = -1;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            if (!self) break;
            const int delta = GET_WHEEL_DELTA_WPARAM(w);
            Log::WriteF(LogLevel::Info, "ui", "wheel page=%d delta=%d", self->page(), delta);
            if (self->page() == kPageQuota) {
                g_q.scroll += delta > 0 ? -LS(64) : LS(64); // 上滚到顶/下滚到底由 Compute 钳制
            } else if (self->page() == kPageDisplay) {
                g_dscroll += delta > 0 ? -LS(64) : LS(64); // 同口径钳制在 ComputeDisp
            } else {
                break;
            }
            self->Relayout();                            // 原生输入框随滚动重摆
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            int id = HitAt(pt);
            if (id > kWNone) {
                if (IsSlideId(id)) {
                    g_slide_drag = id;
                    RECT src = RectOfHit(id);
                    int vmin = 0, vmax = 100;
                    SlideRange(id, vmin, vmax);
                    int v = SliderValueFrom(src, vmin, vmax, pt.x);
                    if (id == kWIntSlider) {
                        g_draft.cfg.query.interval_sec = v;
                        HWND e = GetDlgItem(h, kInterval);
                        if (e) SetWindowTextW(e, std::to_wstring(v).c_str());
                    } else {
                        ApplySlideLive(self, id, v);
                    }
                } else {
                    g_pressed = id;
                    // 卡片头部按下：记录待拖拽的可见序（移动超阈值才真正起拖）
                    if (id >= kWAccBase &&
                        (id - kWAccBase) % kAccStride == kAccDrag) {
                        int vi = (id - kWAccBase) / kAccStride;
                        if (vi >= 0 && vi < (int)g_qlayout.vis_idx.size()) {
                            g_drag_press_vis = vi;
                            g_drag_start = pt;
                        }
                    }
                }
                SetCapture(h);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            // 先取拖动 id 再 ReleaseCapture:ReleaseCapture 会同步派发
            // WM_CAPTURECHANGED(把 g_slide_drag 清零),之后判断永远落空,
            // 位置滑杆抬起不落盘(实测:实时预览生效但配置未写,保存时被旧值覆盖)
            const int drag = g_slide_drag;
            g_slide_drag = 0;
            // 拖拽排序同理：先快照源位再释放捕获（WM_CAPTURECHANGED 会清 g_drag_from）
            const int card_drag_from = g_drag_from;
            if (GetCapture() == h) ReleaseCapture();
            if (drag == kWPosSlider) CommitSlot(self); // 嵌入偏移滑杆抬起落盘
            else if (drag) CommitSlide(self, drag);    // 浮条位置/吸附滑杆抬起落盘
            if (drag) {
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            // 拖拽排序落位（F3）
            if (card_drag_from >= 0) {
                int from = card_drag_from;
                int slot = ComputeDragTarget(pt, from);
                g_drag_from = -1;
                g_drag_target = -1;
                g_drag_press_vis = -1;
                Log::WriteF(LogLevel::Info, "ui", "card drop from=%d slot=%d", from, slot);
                if (slot >= 0 && slot != from && self && self->AppPtr() &&
                    from < (int)g_qlayout.vis_idx.size()) {
                    const auto& accs = self->AppPtr()->Accounts();
                    int ai = g_qlayout.vis_idx[from];
                    if (ai >= 0 && ai < (int)accs.size()) {
                        if (!self->AppPtr()->MoveAccountOrder(accs[ai].cfg.id, slot))
                            UiEcho(h, kEchoErr,
                                   L"已排序，但写入失败（密钥对位未更新，请重试）");
                        self->RebuildPage(); // 可见序变化 → 动态 id/输入框全部重建
                        return 0;
                    }
                }
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            g_drag_press_vis = -1;
            g_pressed = -1;
            // up 的激活只按命中测试、不依赖 down 状态（shot_pages.ps1 等自动化仅
            // Post WM_LBUTTONUP 也要能切页/触发；拖离取消仅影响按压视觉）
            int id = HitAt(pt);
            if (id > kWNone) {
                HandleAction(self, h, id);
                return 0;
            }
            InvalidateRect(h, nullptr, FALSE);
            break;
        }
        case WM_CAPTURECHANGED:
            g_pressed = -1;
            g_slide_drag = 0;
            g_drag_press_vis = -1;
            g_drag_from = -1;
            g_drag_target = -1;
            InvalidateRect(h, nullptr, FALSE);
            break;
        case WM_CTLCOLOREDIT: {
            // 输入框：等宽主题底；数值分色（画刷随主题代数重建，F1）
            HDC hdc = (HDC)w;
            int id = GetDlgCtrlID((HWND)l);
            COLORREF fg = theme::kSOnSurf;
            if (id == kInterval) fg = theme::kSPrimary;
            else if (id == kKimiThr || id == kDsThr) fg = theme::kSTertiary;
            SetTextColor(hdc, fg);
            SetBkColor(hdc, theme::kSSurfLow2);
            static HBRUSH input_br = nullptr;
            static int input_br_gen = -1;
            return (LRESULT)ThemeInputBrush(input_br, input_br_gen);
        }
        case WM_CTLCOLORSTATIC: {
            // 禁用 EDIT 与公共深色（对话框原语兜底）
            HDC hdc = (HDC)w;
            int id = GetDlgCtrlID((HWND)l);
            if (id == kInterval || id == kKimiThr || id == kDsThr || IsQuotaEditId(id)) {
                SetTextColor(hdc, theme::kSOutline);
                SetBkColor(hdc, theme::kSSurfLow2);
                static HBRUSH input_br2 = nullptr;
                static int input_br2_gen = -1;
                return (LRESULT)ThemeInputBrush(input_br2, input_br2_gen);
            }
            LRESULT out = 0;
            if (theme::HandleDialogMessage(h, m, w, l, &out)) return out;
            break;
        }
        case WM_SYSCOMMAND:
            if ((w & 0xFFF0) == SC_CLOSE) {
                if (self) self->CloseDetail();
                return 0;
            }
            break;
        case WM_CLOSE:
            // 任务栏右键「关闭窗口」等外部 WM_CLOSE：与标题栏 X 同径（淡出+清理）
            if (self) self->CloseDetail();
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(w), code = HIWORD(w);
            // EN_* 只可能来自本窗口的子控件；校验归属，防其它来源（对话框转发等）
            // 携带同值 id 误触发输入框联动重绘
            HWND ctl = (HWND)l;
            bool from_own = ctl && GetAncestor(ctl, GA_ROOT) == h;
            if (from_own &&
                (id == kInterval || id == kKimiThr || id == kDsThr || IsQuotaEditId(id)) &&
                (code == EN_SETFOCUS || code == EN_KILLFOCUS || code == EN_CHANGE)) {
                InvalidateRect(h, nullptr, FALSE); // placeholder/值联动重绘
                return 0;
            }
            if (self && self->OnCommand(id, code, h)) return 0;
            break;
        }
        case WM_NCDESTROY:
            if (self) self->OnDetailClosed();
            return 0;
        case WM_TIMER:
            // 回显过期驱动（kTimerEcho，200ms 粒度）：到期清态重绘，全部过期后停表
            if (w == kTimerEcho) {
                const int64_t now = NowMono();
                bool live = false;
                if (!g_echo.text.empty()) {
                    if (g_echo.until > now)
                        live = true;
                    else {
                        g_echo.text.clear();
                        InvalidateRect(h, nullptr, FALSE);
                    }
                }
                if (g_save_fb.kind != 0) {
                    if (g_save_fb.until > now)
                        live = true;
                    else {
                        g_save_fb.kind = 0;
                        InvalidateRect(h, nullptr, FALSE);
                    }
                }
                if (!live) KillTimer(h, kTimerEcho);
                return 0;
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(h, m, w, l);
}


// =========================================================
// 主窗口：页面管理
// =========================================================
// 有未保存草稿时先确认（切页/关窗都会经 RebuildPage 重置草稿——旧实现静默丢弃
// 用户改动，是「反馈缺失」排查清单中数据丢失风险最高的一项）。
// 返回 false=用户选择留下，调用方中止动作。
bool ConfirmDiscardDraft(HWND h, App* app, bool closing) {
    if (!h || !app) return true;
    if (!DisplayDraftDirty(h, app->Cfg())) return true;
    int r = MessageBoxW(h, closing
                                ? L"显示设置有未保存的更改，关闭将放弃这些更改。继续吗？"
                                : L"显示设置有未保存的更改，切换将放弃这些更改。继续吗？",
                        L"QuotaBar", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    return r == IDYES;
}

void TrayUi::ToggleDetail() {
    // 托盘左键=「切到前台 ⇄ 关闭」。旧实现只看 IsWindowVisible：窗口被其他窗口
    // 遮挡或最小化时它仍算"可见"，第一次点击走的是 CloseDetail（把窗口销毁），
    // 用户看到的是"点了没反应"，第二次点击才真正打开（实测反馈的 bug）。
    if (!detail_) {
        ShowDetail();
        return;
    }
    if (!IsWindowVisible(detail_) || IsIconic(detail_)) {
        ShowDetail();
        return;
    }
    if (GetForegroundWindow() == detail_) {
        CloseDetail(); // 确实在前台 → 才是"关闭"切换
    } else {
        // 遮挡在后台：只带到前台。不要用 SW_RESTORE——最大化/贴靠的窗口会被
        // 缩回普通尺寸，用户感知是"窗口自己变小了"
        ShowWindow(detail_, SW_SHOW);
        SetForegroundWindow(detail_);
    }
}

void TrayUi::ShowDetail() {
    const bool created = detail_ == nullptr;
    if (!detail_) {
        // 自定义框架：保留 caption/thickframe 位（最小化/最大化/缩放行为依赖），
        // WM_NCCALCSIZE 去掉原生子标题栏。WS_CLIPCHILDREN：子控件只剩不透明背景的
        // EDIT，裁剪避免父整帧重绘覆盖输入框（旧约束针对透明 GROUPBOX/STATIC，已不存在）。
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                      WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        if (!EnsureClass(kDetailClass, DetailProc, inst_)) return;
        SetLayoutDpi(msg_wnd_);
        detail_ = CreateWindowExW(0, kDetailClass, L"QuotaBar", style,
                                  CW_USEDEFAULT, CW_USEDEFAULT, LS(kWinWDip), LS(kWinHDip),
                                  msg_wnd_, nullptr, inst_, nullptr);
        if (!detail_) return;
        SetWindowLongPtrW(detail_, GWLP_USERDATA, (LONG_PTR)this);
        // 未显示窗口的 GetDpiForWindow 可能返回 0（实测），此时按监视器有效 DPI
        // 兜底，否则窗口按 96 建尺寸、绘制按监视器倍率，内容成倍挤进小窗
        {
            UINT dpi = GetDpiForWindow(detail_);
            UINT dpx = 0, dpy = 0;
            if (!dpi) {
                HMONITOR mon0 = MonitorFromWindow(detail_, MONITOR_DEFAULTTONEAREST);
                if (SUCCEEDED(GetDpiForMonitor(mon0, MDT_EFFECTIVE_DPI, &dpx, &dpy)) && dpx)
                    dpi = dpx;
            }
            if (dpi) g_layout_dpi = (int)dpi;
        }
        RebuildPage();
        RECT wr{0, 0, LS(kWinWDip), LS(kWinHDip)};
        AdjustWindowRectEx(&wr, style, FALSE, 0);
        HMONITOR mon = MonitorFromWindow(msg_wnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
        // 工作区保护：超高缩放下窗口不超出工作区（页面布局按实际客户区自适应）
        ww = std::min(ww, (int)(mi.rcWork.right - mi.rcWork.left) - LS(16));
        wh = std::min(wh, (int)(mi.rcWork.bottom - mi.rcWork.top) - LS(16));
        SetWindowPos(detail_, nullptr,
                     mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2,
                     mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2,
                     ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    UpdateDetailText();
    const bool was_visible = IsWindowVisible(detail_) != FALSE;
    const bool iconic = IsIconic(detail_) != FALSE;
    bool animated = false;
    if (iconic) {
        // 最小化态 IsWindowVisible 仍为真且 was_visible=1 会跳过淡入，但
        // SW_SHOW 不还原最小化窗口（实测"托盘点不回来"，用户连点无响应），
        // 必须 SW_RESTORE（会按此前状态还原：普通尺寸或最大化）
        ShowWindow(detail_, SW_RESTORE);
    } else {
        animated = !was_visible && theme::AnimationsEnabled() &&
                   AnimateWindow(detail_, 140, AW_BLEND);
        if (!animated) ShowWindow(detail_, SW_SHOW);
    }
    // 显示后补一次重绘：被遮挡窗口的首帧可能被 DWM 丢弃（实测空窗）
    InvalidateRect(detail_, nullptr, FALSE);
    SetForegroundWindow(detail_);
    Log::WriteF(LogLevel::Info, "ui", "show detail: created=%d was_vis=%d animated=%d iconic=%d",
                created ? 1 : 0, was_visible ? 1 : 0, animated ? 1 : 0, iconic ? 1 : 0);
}

void TrayUi::ShowQuotaAddPanel() {
    // 首次运行引导：打开额度页并展开「添加卡片」面板
    page_ = kPageQuota;
    g_q.add_open = true;
    if (detail_) RebuildPage();
    ShowDetail();
}

void TrayUi::ShowMainAt(int page) {
    if (page < 0 || page >= kPageCount) page = kPageQuota;
    if (detail_) {
        SetPage(page);
    } else {
        page_ = page;
    }
    ShowDetail();
}

void TrayUi::SetPage(int page) {
    if (page < 0 || page >= kPageCount) return;
    if (!detail_ || page == page_) return;
    // 离开显示页前确认脏草稿（RebuildPage 会重置草稿=静默丢弃改动）
    if (page_ == kPageDisplay && !ConfirmDiscardDraft(detail_, app_, false)) return;
    page_ = page;
    RebuildPage();
}

void TrayUi::RebuildPage() {
    if (!detail_) return;
    g_pressed = -1;
    g_hover = -1;
    g_card_hover = -1;
    g_slide_drag = 0;
    g_drag_press_vis = -1;
    g_drag_from = -1;
    g_drag_target = -1;
    g_links.clear();
    DestroyChildren(detail_);
    // 页面草稿重置（EDIT 文本随后灌入）；账户状态（展开/掩码/滚动）保留会话语义
    g_draft.cfg = app_->Cfg();
    g_draft.slot = app_->Cfg().display.slot_offset_x_dip;
    CreatePageControls();
    ApplyEditFonts(detail_);
    theme::ModernizeDialog(detail_);
    Relayout();
    UpdateDetailText();
    InvalidateRect(detail_, nullptr, TRUE);
}

void TrayUi::CloseDetail() {
    if (!detail_) return;
    // 关窗丢弃显示页脏草稿前确认（标题栏 X / Esc / WM_CLOSE / 托盘 toggle 同径；
    // 退出进程走 Shutdown 的 DestroyWindow，不打扰）
    if (!ConfirmDiscardDraft(detail_, app_, true)) return;
    Log::Write(LogLevel::Info, "ui", "close detail");
    if (theme::AnimationsEnabled() && IsWindowVisible(detail_))
        AnimateWindow(detail_, 140, AW_BLEND | AW_HIDE);
    DestroyWindow(detail_);
}

void TrayUi::Relayout() {
    if (!detail_) return;
    RECT rc{};
    GetClientRect(detail_, &rc);
    // 与 PaintDetail 相同的内容区矩形（输入框必须与自绘输入底对齐）
    RECT content{NavW(rc.right) + LS(28), LS(kTitleDip) + LS(24), rc.right - LS(28),
                 rc.bottom - LS(24)};
    if (page_ == kPageQuota) {
        // 与 PaintQuota 同一布局来源：输入框贴自绘输入底（含滚动偏移）
        QuotaLayout Lq = ComputeQuota(app_, content);
        // 原生 EDIT 不受 DC 裁剪：滚出滚动视口（页头之下）的输入框隐藏，与
        // PaintQuota 的视口裁剪同规，否则输入框悬浮在页头/标题栏上
        const RECT qvp{content.left, content.top + LS(kQuotaHeadDip), content.right,
                       content.bottom};
        auto place_edit = [&](HWND e, const RECT& r) {
            if (!e) return;
            if (r.top < qvp.top || r.bottom > qvp.bottom) {
                ShowWindow(e, SW_HIDE);
                return;
            }
            MoveWindow(e, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
            ShowWindow(e, SW_SHOW);
        };
        // 添加面板
        const bool add_key_visible =
            g_q.add_open && kAddTypes[g_q.add_type] != AccountType::Codex;
        HWND aa = GetDlgItem(detail_, kEditAddAlias);
        if (aa) {
            if (g_q.add_open) {
                place_edit(aa, RECT{Lq.ap_alias.left + LS(10), Lq.ap_alias.top + LS(4),
                                    Lq.ap_alias.right - LS(10),
                                    Lq.ap_alias.bottom - LS(4)});
            } else {
                ShowWindow(aa, SW_HIDE);
            }
        }
        HWND ak = GetDlgItem(detail_, kEditAddKey);
        if (ak) {
            if (add_key_visible) {
                place_edit(ak, RECT{Lq.ap_key.left + LS(38), Lq.ap_key.top + LS(5),
                                    Lq.ap_key.right - LS(102), Lq.ap_key.bottom - LS(5)});
            } else {
                ShowWindow(ak, SW_HIDE);
            }
        }
        // 展开卡的别名/Key 输入框（vi = 可见列表序，与 EDIT id/布局序一致）
        int vi = 0;
        const auto& accs = app_->Accounts();
        for (size_t i = 0; i < accs.size() && vi < (int)Lq.vis_idx.size(); ++i) {
            if ((int)i != Lq.vis_idx[vi]) continue; // 只推进可见序
            const AccountState& st = accs[i];
            if (g_q.IsExpanded(st.cfg.id)) {
                const QuotaCardR& r = Lq.cr[vi];
                HWND ae = GetDlgItem(detail_, kEditAccAlias0 + vi);
                if (ae) {
                    place_edit(ae, RECT{r.ex_alias.left + LS(10), r.ex_alias.top + LS(4),
                                        r.ex_alias.right - LS(10),
                                        r.ex_alias.bottom - LS(4)});
                }
                HWND ke = GetDlgItem(detail_, kEditAccKey0 + vi);
                if (ke) {
                    place_edit(ke, RECT{r.ex_key.left + LS(38), r.ex_key.top + LS(5),
                                        r.ex_key.right - LS(102), r.ex_key.bottom - LS(5)});
                }
            }
            ++vi;
        }
    } else if (page_ == kPageDisplay) {
        // 原生 EDIT 不受 DC 裁剪约束：滚出滚动视口的输入框必须隐藏，
        // 否则会悬浮在页头/页脚动作条之上（实测"900"输入框盖住页脚条）
        const RECT vp{content.left, content.top + LS(52), content.right,
                      content.bottom - LS(28)};
        auto place_edit = [&](HWND e, const RECT& r) {
            if (!e) return;
            if (r.top < vp.top || r.bottom > vp.bottom) {
                ShowWindow(e, SW_HIDE); // 已滚出视口（部分越界即隐藏）
                return;
            }
            MoveWindow(e, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
            ShowWindow(e, SW_SHOW);
        };
        DispLayout L = ComputeDisp(content);
        place_edit(GetDlgItem(detail_, kInterval), L.int_edit);
        // 阈值输入框跟随「低额度预警」开关显隐（渐进披露；隐藏即禁用输入）
        HWND kt = GetDlgItem(detail_, kKimiThr);
        HWND dt = GetDlgItem(detail_, kDsThr);
        if (L.thr_visible) {
            EnableWindow(kt, TRUE);
            place_edit(kt, L.thr_k_edit);
            EnableWindow(dt, TRUE);
            place_edit(dt, L.thr_d_edit);
        } else {
            ShowWindow(kt, SW_HIDE);
            ShowWindow(dt, SW_HIDE);
        }
    }
}

void TrayUi::OnDetailClosed() {
    detail_ = nullptr;
    g_hover = -1;
    g_pressed = -1;
    g_card_hover = -1;
    g_slide_drag = 0;
    g_drag_press_vis = -1;
    g_drag_from = -1;
    g_drag_target = -1;
    g_tracked = false;
    g_links.clear();
    g_echo.text.clear(); // 回显状态不跨窗口生命周期（定时器随窗口销毁自动清理）
    g_save_fb.kind = 0;
}

void TrayUi::UpdateDetailText() {
    if (!detail_ || !app_) return;
    std::wstring title = std::wstring(L"QuotaBar — ") + PageTitle(page_);
    SetWindowTextW(detail_, title.c_str());
    InvalidateRect(detail_, nullptr, FALSE);
}

bool TrayUi::PreTranslateMessage(MSG* m) {
    if (!detail_ || !m || !m->hwnd) return false;
    if (m->hwnd != detail_ && !IsChild(detail_, m->hwnd)) return false;
    // 位置微调快捷键：Alt+←/→ 微移 1px，Shift+←/→ 步进 4px（设计稿「高频快捷键协同」）
    // Alt 是系统键：按住 Alt 的方向键走 WM_SYSKEYDOWN，只判 WM_KEYDOWN 永远收不到
    //（用户实测"Alt+左右不微调"的根因）；Shift 组合才是普通 WM_KEYDOWN。
    if ((m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) &&
        page_ == kPageDisplay && (m->wParam == VK_LEFT || m->wParam == VK_RIGHT)) {
        const bool alt = GetKeyState(VK_MENU) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        // 焦点在输入框时不抢 Shift+方向键（文本选择优先）；Alt 组合与输入无关恒拦截
        HWND focus = GetFocus();
        wchar_t cls[16]{};
        bool focus_edit = focus && IsChild(detail_, focus) &&
                          GetClassNameW(focus, cls, 16) > 0 && wcscmp(cls, L"Edit") == 0;
        if (alt || (shift && !focus_edit)) {
            // 快捷键只服务嵌入条偏移（浮条位置走滑杆/Ctrl+拖动；其他模式不吞方向键）
            if (app_->Cfg().display.mode != DisplayMode::Embed) return false;
            int d = (m->wParam == VK_RIGHT ? 1 : -1) * (shift ? 4 : 1);
            NudgeSlot(this, d);
            return true; // 吞掉：不进 IsDialogMessage/DefWindowProc（防触发系统菜单模式）
        }
    }
    return IsDialogMessageW(detail_, m) != FALSE;
}

// =========================================================
// 页面创建
// =========================================================
void TrayUi::CreatePageControls() {
    auto mk_edit = [&](int id, bool password, int limit, bool number = false) {
        HWND e = CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                                     (password ? ES_PASSWORD : 0) | (number ? ES_NUMBER : 0),
                                 0, 0, 10, 10, detail_, (HMENU)(INT_PTR)id, nullptr, nullptr);
        SendMessageW(e, EM_SETLIMITTEXT, limit, 0);
        return e;
    };
    switch (page_) {
        case kPageQuota: {
            if (g_q.add_open) {
                mk_edit(kEditAddAlias, false, 64);
                if (kAddTypes[g_q.add_type] != AccountType::Codex)
                    mk_edit(kEditAddKey, g_q.add_mask, 4096);
            }
            // 展开卡的别名/Key 输入框（vi = 可见列表序）
            int vi = 0;
            for (const auto& st : app_->Accounts()) {
                if (!st.cfg.card_visible) continue;
                if (g_q.IsExpanded(st.cfg.id)) {
                    std::wstring alias = Utf8ToWide(
                        st.cfg.alias.empty() ? ProviderDefaultAlias(st.cfg.type)
                                             : st.cfg.alias);
                    SetWindowTextW(mk_edit(kEditAccAlias0 + vi, false, 64), alias.c_str());
                    if (st.cfg.type != AccountType::Codex) {
                        HWND ke = mk_edit(kEditAccKey0 + vi, g_q.IsMasked(st.cfg.id), 4096);
                        SetWindowTextW(ke, Utf8ToWide(app_->GetAccountKey(st.cfg.id)).c_str());
                    }
                }
                ++vi;
            }
            break;
        }
        case kPageDisplay: {
            HWND iv = mk_edit(kInterval, false, 8, true);
            SetWindowTextW(iv, std::to_wstring(g_draft.cfg.query.power_save
                                                   ? 900
                                                   : g_draft.cfg.query.interval_sec)
                                   .c_str());
            if (g_draft.cfg.query.power_save) EnableWindow(iv, FALSE);
            HWND kt = mk_edit(kKimiThr, false, 4, true);
            char abuf[32];
            sprintf_s(abuf, "%d", g_draft.cfg.notify.kimi_threshold_pct);
            SetWindowTextW(kt, Utf8ToWide(abuf).c_str());
            if (!g_draft.cfg.notify.enabled) EnableWindow(kt, FALSE);
            HWND dt = mk_edit(kDsThr, false, 16);
            sprintf_s(abuf, "%.2f", g_draft.cfg.notify.deepseek_threshold_cny);
            SetWindowTextW(dt, Utf8ToWide(abuf).c_str());
            if (!g_draft.cfg.notify.enabled) EnableWindow(dt, FALSE);
            break;
        }
        default: break;
    }
}

bool TrayUi::OnCommand(int id, int code, HWND h) {
    (void)code; // 仅 IDOK/IDCANCEL 到达此处（自绘控件走 HitAt/HandleAction）
    switch (id) {
        case IDOK:
            // Enter：显示页保存（额度页无整页保存语义——账户变更即时提交）
            if (page_ == kPageDisplay) HandleAction(this, h, kWSaveCfg);
            return true;
        case IDCANCEL:
            // Esc：关闭窗口（IsDialogMessage 分发）
            CloseDetail();
            return true;
        default:
            return false;
    }
}


} // namespace qb