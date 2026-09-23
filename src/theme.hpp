// theme.hpp — 设计 token + 现代深色主题原语。
// token（调色板/字号）是全部界面共享的单一来源：对话框、详情卡片、圆环、按钮同源；
// 绘制原语均为像素坐标（调用方负责 DPI 换算与双缓冲），GDI+ 会话在函数内部托管。
// 系统集成：暗色菜单（uxtheme）、"减少动态效果"跟随、Win11 圆角/深色标题栏。
// 全部基于系统 API，不引入第三方依赖；旧系统特性静默降级。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <vector>

namespace qb {
namespace theme {

// ---- 设计 token：层级色（F1 迭代 2026-09-15 起运行时双主题）----
// token 为 inline 运行时变量：引用处写法不变（theme::kSSurf），由 SetUiTheme 统一填充
// 深色（默认）或浅色一套。字号 TypeSize 仍是编译期档位护栏，不随主题变化。
inline COLORREF kWindowBg  = RGB(30, 31, 36);      // 窗口/对话框底
inline COLORREF kCardBg    = RGB(38, 40, 46);      // 卡片底
inline COLORREF kPanelBg   = RGB(24, 25, 29);      // 输入框/内容区底
inline COLORREF kBorder    = RGB(58, 62, 72);      // 边框/分隔线
inline COLORREF kText      = RGB(232, 234, 237);   // 主文字
inline COLORREF kSubText   = RGB(154, 160, 170);   // 次要文字
// ---- 设计 token：语义状态色（低额/耗尽/正常/信息） ----
inline COLORREF kAccent    = RGB(96, 141, 255);    // 强调蓝（链接/默认键/信息态）
inline COLORREF kOk        = RGB(64, 201, 180);    // 正常（蓝绿）
inline COLORREF kWarn      = RGB(255, 176, 32);    // 低额（琥珀）
inline COLORREF kBad       = RGB(255, 92, 92);     // 耗尽/凭证错误（红）
inline COLORREF kDanger    = kBad;                 // 旧名别名（危险操作）
// 品牌徽章色（K/D/C/G）已迁至 providers.hpp ProviderOps.brand_color（2026-09-18）：
// 品牌视觉身份是平台常量、不随主题变，归 providers 分派表单点；theme 只保留随主题变的 token
// ---- 设计 token：按钮（四态 + 默认键强调） ----
inline COLORREF kBtnNormal  = RGB(43, 46, 53);
inline COLORREF kBtnHover   = RGB(55, 59, 69);
inline COLORREF kBtnDown    = RGB(32, 35, 41);
inline COLORREF kBtnBorder  = RGB(58, 62, 72);
inline COLORREF kBtnTextOff = RGB(108, 112, 121);
inline COLORREF kBtnDefNormal = RGB(60, 96, 190);   // 默认键（强调蓝的实体变体）
inline COLORREF kBtnDefHover  = RGB(74, 112, 214);
inline COLORREF kBtnDefDown   = RGB(52, 84, 168);

// ---- 主题切换（F1：主窗口/对话框跟随系统深浅色）----
// 填充整套颜色 token 并递增主题代数；窗口按新 token 重绘即可，静态缓存的
// 画刷（输入框底色等）以 UiThemeGen() 判断重建。
void SetUiTheme(bool dark);
bool UiIsDark();     // 当前是否深色（进程默认深色）
int  UiThemeGen();   // 主题代数（每次 SetUiTheme +1；深色初值 0）
void ApplyUiThemeToWindow(HWND wnd); // 对已开窗口重应用 DWM 边框/标题（主题切换后调用）

// ---- 系统集成 ----
// 跟随系统"减少动态效果"（设置→辅助功能→视觉效果）；查询失败按允许动画处理。
bool AnimationsEnabled();
// uxtheme 暗色菜单（SetPreferredAppMode：深色主题 ForceDark，浅色主题 Default 回落）；
// 进程启动与主题切换时调用，失败静默。
void EnableDarkSystemChrome();

// 窗口外观：Win11 圆角 + 标题栏/边框/标题文字按当前主题 token（旧系统调用失败即忽略）
void ApplyWindowChrome(HWND wnd);

// 对话框统一主题（跟随深/浅）：深色时子控件走 DarkMode 主题；按钮一律自绘子类化
//（含键盘焦点环）。对话框创建后、显示前调用一次。
void ModernizeDialog(HWND dlg);

// 对话框公共消息（WM_ERASEBKGND/CTLCOLORSTATIC/CTLCOLOREDIT/CTLCOLORBTN，按当前主题取色）。
// 处理了返回 true 并给出 LRESULT；未处理返回 false（调用方走原逻辑）。
bool HandleDialogMessage(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* out);

// ---- 设计 token：字号阶梯（DIP；主窗口唯一允许的档位） ----
// 语义档而非裸字号：Txt/Measure/Ts 只收 TypeSize，裸字面量无法编译（编译期护栏）。
// 层级逻辑同 Fluent 2 type ramp：正文唯一档（强调用 bold），辅助文字下限 = 正文。
// 等宽 kFontMono 仅用于 API Key/路径/命令等"代码"内容；数值、时间戳一律 Segoe UI。
enum class TypeSize {
    Hero,  // 26：单一主数值（余额/偏移大数字）
    Num,   // 20：页面标题、卡内窗口数值
    Head,  // 15：卡片标题/分区标题（配 bold）
    Body,  // 12：正文、说明、控件文字（唯一正文档）
    Cap,   // 12：辅助文字下限（时间戳/角标/快捷键提示；= Body 值，语义别名）
    Mini,  // 10：章/角标/图表刻度（stitch 稿 9-11px 元信息档；仅短文案）
};
constexpr float TypeSizeDip(TypeSize s) {
    switch (s) {
        case TypeSize::Hero: return 26;
        case TypeSize::Num: return 20;
        case TypeSize::Head: return 15;
        case TypeSize::Mini: return 10;
        case TypeSize::Cap:
        case TypeSize::Body: return 12;
    }
    return 12;
}

// ---- 文本原语（像素坐标；GDI+ 抗锯齿，支持对齐/换行/省略） ----
struct TextStyle {
    float px = 12;            // 字号（设备像素，调用方按 DPI 换算）
    bool bold = false;
    COLORREF color = kText;
    int alpha = 255;
    int align = 0;            // 0=左 1=中 2=右
    int valign = 0;           // 0=上 1=中
    bool wrap = false;        // true=按矩形宽换行；false=单行
    bool ellipsis = true;     // 超宽以省略号截断（wrap=false 时水平方向生效）
    const wchar_t* face = nullptr; // nullptr=Segoe UI；等宽/数值文本传 kFontMono
};
// 等宽字体（stitch 设计稿 label-code/label-numeric 的 JetBrains Mono 系统近似；Win11 自带）
inline constexpr const wchar_t* kFontMono = L"Cascadia Mono";
void DrawTextPx(HDC hdc, const RECT& rc, const wchar_t* text, const TextStyle& ts);
// 测量单行文本（GenericTypographic；返回向上取整的像素尺寸）
SIZE MeasureTextPx(HDC hdc, const wchar_t* text, const TextStyle& ts);

// ---- 形状原语（像素坐标；调用方负责 DPI 与双缓冲） ----
void FillRoundRectPx(HDC hdc, const RECT& rc, int radius_px, COLORREF color, int alpha);
void StrokeRoundRectPx(HDC hdc, const RECT& rc, int radius_px, COLORREF color, int alpha,
                       float width_px);
void FillEllipsePx(HDC hdc, float cx, float cy, float diameter, COLORREF color, int alpha);
// 水平线性渐变（左→右），用于页眉底色
void FillGradientPx(HDC hdc, const RECT& rc, COLORREF left, COLORREF right);
// 状态环：sweep_percent<0 只画轨道；0 不画值弧；>=100 画整圆（避免弧缝）。
// 轨道透明度 = alpha 的 20%。
void DrawRingPx(HDC hdc, float cx, float cy, float diameter, float stroke,
                float sweep_percent, COLORREF track, COLORREF value, int alpha);
// 品牌徽章：圆角方块 + 居中单字母（圆环模式与详情卡片同源）
void DrawBadgePx(HDC hdc, const RECT& rc, int radius_px, COLORREF fill, int alpha,
                 wchar_t letter, float letter_px, bool letter_bold,
                 COLORREF letter_color, int letter_alpha);

// GDI+ 进程级初始化（引用计数幂等）。模块内直接 new Graphics 前调用一次。
void EnsureGdiplus();

// ---- stitch 设计稿调色板（stitch_/stitch_minimalist_app_interface；主窗口专用）----
// 2026-09-17 重构为 stitch_minimalist 稿的 Tailwind 色板：深色 = 稿件原值
//（app.bg/card/sidebar/accent/emerald），浅色 = 同阶亮色映射。
// kS* 为 inline 运行时变量：深色默认值在此，浅色由 SetUiTheme 填充。
inline COLORREF kSbg         = RGB(0x0B, 0x0F, 0x17); // app.bg（slate-950 蓝黑）
inline COLORREF kSSurfLow    = RGB(0x0E, 0x13, 0x1F); // sidebar/titlebar
inline COLORREF kSSurf       = RGB(0x14, 0x1B, 0x2B); // card
inline COLORREF kSSurfHigh   = RGB(0x18, 0x22, 0x36); // card-hover
inline COLORREF kSSurfHigh2  = RGB(0x20, 0x2B, 0x44); // 按下/更高一档悬浮面
inline COLORREF kSSurfLow2   = RGB(0x07, 0x0B, 0x12); // 输入框底/最低层
inline COLORREF kSOnSurf     = RGB(0xED, 0xF1, 0xF7); // on-surface（近 slate-100）
inline COLORREF kSOnSurfVar  = RGB(0x9A, 0xA6, 0xBC); // on-surface-variant（slate-400）
inline COLORREF kSOutline    = RGB(0x64, 0x74, 0x8B); // outline（slate-500，次要文字）
inline COLORREF kSOutlineVar = RGB(0x33, 0x3D, 0x52); // outline-variant（轴线/刻度须）
inline COLORREF kSPrimary    = RGB(0x60, 0xA5, 0xFA); // accent 文字/链接（blue-400）
inline COLORREF kSPrimCont   = RGB(0x25, 0x63, 0xEB); // 主按钮/开关底（blue-600）
inline COLORREF kSOnPrimCont = RGB(0xFF, 0xFF, 0xFF); // 其上文字（蓝底白字）
inline COLORREF kSPrimaryFix = RGB(0xD8, 0xE2, 0xFF); // primary-fixed（浅色保存键底，保留）
inline COLORREF kSPrimFixHover = RGB(0xC3, 0xD6, 0xFF); // primary-fixed 悬停
inline COLORREF kSPrimFixDown  = RGB(0x9F, 0xBC, 0xEF); // primary-fixed 按下
inline COLORREF kSSecondary  = RGB(0x34, 0xD3, 0x99); // 正常/在线/健康（emerald-400）
inline COLORREF kSSecCont    = RGB(0x05, 0x96, 0x69); // emerald-600
inline COLORREF kSTertiary   = RGB(0xFB, 0xBF, 0x24); // 低额/暂停（amber-400）
inline COLORREF kSTertCont   = RGB(0xB4, 0x53, 0x09); // amber-700
inline COLORREF kSError      = RGB(0xF8, 0x71, 0x71); // 耗尽/错误（red-400）
inline COLORREF kSErrCont    = RGB(0x7F, 0x1D, 0x1D); // red-900
// 卡片/面板结构色（stitch：border=white/8、border-subtle=white/4、bg-black/25 内嵌）
inline COLORREF kSCardBorder = RGB(0x27, 0x2E, 0x3E); // 卡片/侧栏/标题栏描边
inline COLORREF kSBorderSoft = RGB(0x1D, 0x24, 0x34); // 细分隔线（border-subtle）
inline COLORREF kSInset      = RGB(0x0C, 0x12, 0x20); // 卡内数值区/图表面板底
inline COLORREF kSGridLine   = RGB(0x1C, 0x22, 0x30); // 图表虚线网格/圆环轨道

} // namespace theme
} // namespace qb
