// tray_ui_internal.hpp — tray_ui 系内部共享契约（2026-09-18 拆分立，2026-09-19 终局更新）。
// 消费方：ui_draw.cpp / detail_shell.cpp / tray_area.cpp / quota_page.cpp / display_page.cpp。
// 所有权：共享全局与控件原语的【定义】在 ui_draw.cpp；窗口壳与命令分发在 detail_shell.cpp；
// 托盘段在 tray_area.cpp；页面绘制在其各自 TU；默认参数只在此头声明（定义处不再重复）。
// 托盘图标资源/ id 常量（kTrayIconId/kIconResId）定义在 tray_area.cpp。
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include "config.hpp"
#include "state.hpp"       // DetailCard/DisplayMode 等
#include "theme.hpp"
#include "ui_icons.hpp"    // Ic/Icon/DrawLogo
#include "ui_layout.hpp"   // ui::HitW
#include "quota_layout.hpp"
#include "disp_layout.hpp"

namespace qb {

class App;
class TrayUi;

// ---- 布局常量（DIP；编译期护栏：与 theme.hpp TypeSize 同源审校） ----
constexpr int kRowH = 40;      // 标签+说明+控件 标准行高
constexpr int kRowGap = 8;     // 行距
constexpr int kSecGap = 16;    // 分区间距
constexpr int kTitleDip = 44;  // 自定义标题栏高（stitch：h-11）
constexpr int kNavWDip = 224;  // 侧栏宽（w-56）
constexpr int kQuotaHeadDip = 52; // 额度页固定页头高（quota_layout after_head 同源）；
                                  // 滚动内容视口顶=content.top+此值（绘制/命中/EDIT 三处同规）
constexpr int kWinWDip = 1000, kWinHDip = 780;
constexpr int kWinMinWDip = 960, kWinMinHDip = 760;

// 主窗口类名（tray_area 注册/创建，DetailProc 在 detail_shell）
constexpr wchar_t kDetailClass[] = L"QuotaBarDetailWnd";
LRESULT CALLBACK DetailProc(HWND h, UINT m, WPARAM w, LPARAM l);

// ---- DPI 缩放（Per-Monitor V2；进程不被系统自动缩放） ----
extern int g_layout_dpi;
inline int LS(int v) { return v * g_layout_dpi / 96; }
inline float LSf(float v) { return v * g_layout_dpi / 96.0f; }
void SetLayoutDpi(HWND wnd); // 内部会同步 SetIconsDpi

// ---- 设计 token 辅助（stitch 调色板见 theme.hpp 的 kS*） ----
COLORREF Blend(COLORREF top, COLORREF base, int alpha);
theme::TextStyle Ts(theme::TypeSize size, bool bold, COLORREF color, int alpha = 255,
                    int align = 0, int valign = 0, bool wrap = false,
                    const wchar_t* face = nullptr);
SIZE Measure(HDC hdc, const wchar_t* text, theme::TypeSize size, bool bold, COLORREF color,
             const wchar_t* face = nullptr);
void Txt(HDC hdc, const RECT& rc, const wchar_t* text, theme::TypeSize size, bool bold,
         COLORREF color, int align = 0, int valign = 1, const wchar_t* face = nullptr,
         int alpha = 255);
void FillRr(HDC hdc, const RECT& rc, int radius_dip, COLORREF color, int alpha = 255);
void StrokeRr(HDC hdc, const RECT& rc, int radius_dip, COLORREF color, int alpha,
              float w_dip = 1.0f);
void HLinePx(HDC hdc, int x1, int x2, int y, COLORREF color);
void Dot(HDC hdc, int cx, int cy, int d_dip, COLORREF color, int alpha = 255);
inline RECT RC(int x, int y, int w, int h) { return RECT{x, y, x + w, y + h}; }
void RowDown(int& y, int h, int gap = kRowGap); // 流式纵向布局（无返回值，y 原地累进）
inline bool RailNav(int client_w) { return client_w < LS(kWinWDip); }
inline int NavW(int client_w) { return RailNav(client_w) ? LS(56) : LS(kNavWDip); }

// ---- 交互 id（g_hits 注册；导航/按钮/滑杆统一） ----
enum {
    kWNone = 0,
    kWMin, kWMax, kWClose, kWRefresh,
    kWNav0, kWNav1,
    kWAddCard,        // 页头「添加卡片」按钮（展开/收起新建面板）
    kWHiddenChip,     // 页头「已隐藏 (N)」chip（展开/收起隐藏卡片区）
    // 添加面板
    kWAddType0, kWAddType1, kWAddType2, kWAddType3,
    kWAddCreate, kWAddCancel, kWAddKeyEye, kWAddKeyCopy, kWAddKeyClear,
    // 额度卡片页脚/行内链接（动态注册；分发按「id>=kWLink0 且 id-kWLink0<g_links.size()」
    // 判界，页面固定控件 id 必须落在 g_links 数量之外——账户卡片控件用 kWAccBase=2000
    // 远偏移）
    kWLink0,  // kWLink0+i：链接（打开控制台/说明页）
    // 显示页
    kWMode0, kWMode1, kWMode2, kWMode3,
    kWLayout2, kWLayout1, kWFontDec, kWFontInc,
    kWColor0, kWColor1, kWColor2,
    kWTheme0, kWTheme1, kWTheme2,
    kWLowColor, kWBrandName, kWEmbIcons, kWEmbNames, kWEmbWindows,
    kWEco, kWNotify, kWAutoStart, kWClearData,
    kWDiscardCfg, kWSaveCfg, kWIntSlider,
    // 显示页·位置微调区（嵌入偏移 / 浮条位置 / 吸附开关与吸附偏移）
    kWPosZero, kWPosSlider, kWBarSnap, kWPosXSlider, kWPosYSlider, kWBarSnapXSlider,
};

// 账户卡片动态控件 id：kWAccBase + 账户序 * kAccStride + 子 id。
// 大偏移避开 kWLink0+i 的链接区间与上方固定枚举。
constexpr int kWAccBase = 2000;
constexpr int kAccMaxCount = 100;
constexpr int kAccStride = 20;
constexpr int kAccEnable = 0;   // 启用开关（查询+常驻条）
constexpr int kAccEye = 1;      // 显隐开关（卡片是否显示）
constexpr int kAccSettings = 2; // 展开/收起设置区
constexpr int kAccSave = 3;     // 保存修改
constexpr int kAccDelete = 4;   // 删除卡片
constexpr int kAccKeyEye = 5;   // Key 明文切换
constexpr int kAccKeyCopy = 6;  // Key 复制
constexpr int kAccKeyClear = 7; // Key 清空
constexpr int kAccHelp = 8;     // 获取说明（弹窗）
constexpr int kAccCodexSync = 9;// codex 重新读取 auth.json
constexpr int kAccNote = 10;    // ⓘ 口径说明悬停命中区
constexpr int kAccAlert = 11;   // 低额预警阈值（弹出选择菜单；Kimi/GLM 卡）
constexpr int kAccUnhide = 12;  // 已隐藏区「显示」按钮（仅隐藏行使用，与卡片子 id 错开）
constexpr int kAccDrag = 13;    // 卡片头部拖拽排序命中区（F3；按下拖离阈值 6px 起拖）

// 原生 EDIT 控件 id（输入路径保留系统控件）
enum { kInterval = 1214, kKimiThr = 1215, kDsThr = 1216,
       kEditAddAlias = 1301, kEditAddKey = 1302,
       kEditAccAlias0 = 1400, kEditAccKey0 = 1500 }; // 账户序 i → +i（<100 个账户）
bool IsQuotaEditId(int id);

// ---- 命中系统（滚动列表项只在视口内可点；chrome 恒可点） ----
extern std::vector<ui::HitW> g_hits;
extern RECT g_hit_content, g_hit_vp, g_hit_extra;
// 命中注册裁剪（额度页滚动视口）：非空时 AddHit 把命中区裁到可见部分——滚到页头/
// 标题栏底下的卡片不再留下不可见却可点的幽灵命中带（与 DC 裁剪同矩形，见 PaintQuota）。
extern RECT g_hit_clip;
inline void AddHit(const RECT& rc, int id) {
    if (g_hit_clip.right > g_hit_clip.left) {
        RECT vis = ui::ClipHitRect(rc, g_hit_clip);
        if (vis.right <= vis.left || vis.bottom <= vis.top) return; // 完全滚出：不注册
        g_hits.push_back({vis, id});
        return;
    }
    g_hits.push_back({rc, id});
}
int HitAt(POINT pt);
RECT RectOfHit(int id);

// ---- 交互态（会话内） ----
extern int g_hover;         // 悬停控件 id
extern int g_pressed;       // 按下控件 id
extern POINT g_cursor_pt;   // 最近鼠标客户区坐标
extern int g_card_hover;    // 光标所在卡片可见序（-1=不在卡上；卡级 hover 提亮重绘判定）
extern int g_slide_drag;    // 正在拖动的滑杆 id（0=无）
extern bool g_tracked;
extern bool g_maximized;
inline bool Hot(int id) { return g_hover == id; }
inline bool Down(int id) { return g_pressed == id; }
// 卡片拖拽排序（F3）
extern int g_drag_press_vis, g_drag_from, g_drag_target;
extern POINT g_drag_start;

// ---- 页脚/行内链接（kWLink0+i ↔ g_links[i]） ----
struct PageLink {
    RECT rc;
    bool accounts;        // true=切到账户设置页；false=打开 url
    const char* url;
};
extern std::vector<PageLink> g_links;

// ---- 页面草稿与状态 ----
struct Draft {
    AppConfig cfg;
    int slot = 0;
};
extern Draft g_draft;

// ---- 动作回显（2026-09-19 第十一轮，用户反馈「保存设置无回显」举一反三） ----
// 单槽轻量回显：窗口底部居中的短暂提示条（复制结果/刷新节流/清除数据/落盘失败等）。
// 新消息覆盖旧消息；kTimerEcho 定时器驱动过期与淡出（绘制在 PaintEchoPill，壳层末尾
// 最上层叠加，不注册命中——2~3s 的瞬时提示不应劫持其下控件的点击）。
enum { kEchoInfo = 0, kEchoOk = 1, kEchoErr = 2 };
struct EchoState {
    std::wstring text;
    int kind = kEchoInfo;
    int64_t until = 0; // 单调钟到期时刻（NowMono 毫秒）
};
extern EchoState g_echo;
// 保存键反馈态：点击「保存设置」后按钮短暂变形成「已保存」/「保存失败」
struct SaveFbState {
    int kind = 0; // 0=常态 1=已保存 2=保存失败
    int64_t until = 0;
};
extern SaveFbState g_save_fb;
constexpr int kTimerEcho = 3; // 详情窗回显定时器 id（该窗口此前无任何定时器占用）
void UiEcho(HWND w, int kind, const wchar_t* text);    // 定义 ui_draw.cpp
void UiSaveFeedback(HWND w, bool ok);                  // 定义 ui_draw.cpp
void PaintEchoPill(HDC m, int client_w, int client_h); // 定义 detail_shell.cpp
// 显示页脏草稿判定：草稿字段 + 输入框实时文本 vs 已提交配置（定义 display_page.cpp）。
// 供页脚「未保存/放弃变更」态与切页/关窗确认共用。
bool DisplayDraftDirty(HWND h, const AppConfig& base);

struct QuotaUi {
    bool add_open = false;          // 「添加卡片」面板展开
    int add_type = 0;               // 0=Kimi 1=Codex 2=GLM 3=Deepseek（chips 序）
    bool add_mask = true;           // 面板 Key 输入掩码态
    std::wstring add_error;         // 面板内联错误
    bool hidden_open = false;       // 已隐藏卡片区展开
    std::vector<std::string> expanded; // 展开设置区的账户 id
    std::vector<std::pair<std::string, bool>> mask; // 每账户 Key 掩码态
    int scroll = 0;                 // 卡片区滚动偏移（px，0=顶部）
    bool IsExpanded(const std::string& id) const {
        return std::find(expanded.begin(), expanded.end(), id) != expanded.end();
    }
    void SetExpanded(const std::string& id, bool on) {
        auto it = std::find(expanded.begin(), expanded.end(), id);
        if (on && it == expanded.end()) expanded.push_back(id);
        if (!on && it != expanded.end()) expanded.erase(it);
    }
    bool IsMasked(const std::string& id) const {
        for (const auto& m : mask)
            if (m.first == id) return m.second;
        return true;
    }
    void SetMask(const std::string& id, bool masked) {
        for (auto& m : mask)
            if (m.first == id) { m.second = masked; return; }
        mask.push_back({id, masked});
    }
};
extern QuotaUi g_q;
inline void ResetQuotaUiScroll() { g_q.scroll = 0; }
extern int g_dscroll; // 显示设置页滚动偏移（ComputeDisp 钳制后统一应用）

// 最近一次布局与卡片数据（Paint 产出；点击处理/Relayout 按同坐标消费）
extern ui::QuotaLayout g_qlayout;
extern std::vector<DetailCard> g_cards;

// ---- 控件原语（定义在 ui_draw.cpp；Draw* 注册命中并消费 Hot/Down） ----
struct BtnStyle {
    COLORREF bg, bg_hover, bg_down, fg;
    COLORREF border = 0; // 0=无边框；非 0=细描边（stitch ghost 键）
};
BtnStyle StyleGhost();
BtnStyle StyleSoft();
BtnStyle StylePrimary();
inline BtnStyle StyleSave() { return StylePrimary(); }
void DrawBtn(HDC hdc, const RECT& rc, const wchar_t* label, int icon, const BtnStyle& st,
             int id, theme::TypeSize text_size = theme::TypeSize::Body, bool semibold = false,
             int radius_dip = 8, int icon_dip = 16, int pad_dip = 10);
void DrawIconBtn(HDC hdc, const RECT& rc, int icon, COLORREF fg, int id,
                 COLORREF hover_bg = theme::kSSurf, COLORREF hover_fg = theme::kSOnSurf,
                 int icon_dip = 18);
void DrawToggle(HDC hdc, const RECT& rc, bool on, COLORREF on_color, int id,
                bool enabled = true);
void DrawSlider(HDC hdc, const RECT& rc, int vmin, int vmax, int val, COLORREF accent, int id,
                bool enabled = true);
void DrawLink(HDC hdc, const RECT& rc, const wchar_t* text, theme::TypeSize size,
              bool accounts, const char* url);
void BrandLook(wchar_t badge, COLORREF& bg, COLORREF& border, COLORREF& fg);
const wchar_t* TypeTag(wchar_t badge);
std::wstring AppVersionText(); // 版本资源读取（ui_draw 定义；侧栏消费）
void SetEditMask(HWND edit, bool masked);
// 额度卡片文本辅助（定义在 ui_draw.cpp）
COLORREF ToneColor(DetailTone t);
std::wstring RemainText(const std::wstring& s);
bool IsAsciiText(const std::wstring& s);
void DrawNoteBubble(HDC m, const RECT& card, const POINT& anchor, const std::wstring& text);
// 返回 false=剪贴板打开/写入失败（被其他程序占用等），调用方负责回显
bool CopyTextToClipboard(HWND h, const std::wstring& t);
std::wstring EditTextW(HWND edit);
std::string EditTextUtf8(HWND edit);
int GetEditInt(HWND edit, int def);
double GetEditDouble(HWND edit, double def);
void DestroyChildren(HWND h);
bool EnsureClass(const wchar_t* cls, WNDPROC proc, HINSTANCE inst);
void RefreshEditFonts();
void ApplyEditFonts(HWND root);
extern HFONT g_edit_mono;

// 显示模式表（绘制与点击分发共用防漂移；定义在 display_page.cpp）。
// 2026-09-21 模式合并：贴靠并入浮条吸附，4 项。
struct ModeDef {
    DisplayMode mode;
    const wchar_t* t;
    const wchar_t* d;
    bool rec;
};
extern const ModeDef kModes[4];

// 越屏改动（浮条 Ctrl+拖动落位/嵌入槽实时微调）后同步页面草稿快照，防止
// 「保存设置」用进页旧快照把已落盘的位置拽回去。
void SyncDraftDisplay(const DisplayConfig& disp);

// 添加面板类型序（与 g_q.add_type 对应；定义在 quota_page.cpp）
extern const AccountType kAddTypes[4];
const wchar_t* AddTypeLabel(int i);
const wchar_t* AddTypeKeyPlaceholder(int i);

// ---- 页面绘制入口（定义在页面 TU） ----
void PaintQuota(HDC m, App* app, const RECT& content, HWND h);
void PaintDisplay(HDC m, App* app, const RECT& content, HWND h);

// ---- 布局入口（包装在页面 TU；纯计算在 quota_layout/disp_layout） ----
ui::QuotaLayout ComputeQuota(App* app, const RECT& content);
ui::DispLayout ComputeDisp(const RECT& content);
int SliderValueFrom(const RECT& rc, int vmin, int vmax, int x);
int ComputeDragTarget(POINT pt, int from_vis);
int HitAt_(POINT pt); // 兼容别名（勿用，见 HitAt）

} // namespace qb
