// tray_ui.hpp — 托盘图标、右键菜单与主窗口（额度卡片墙 + 显示设置，§3.3/§11）。
// 只呈现 UI 并把用户意图转发给 App；不发网络请求；不持有数据副本。
// 主窗口全自绘（自定义标题栏/侧栏），仅文本输入保留原生 EDIT 控件。
// 2026-09-14 起为两页面（额度详情/显示设置）：账户设置并入额度详情卡片
//（1 个 coding plan 账户 = 1 张卡片：开关/额度/控制台/状态 + 可展开的账户设置）。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

namespace qb {

class App;

// 主窗口页面（左侧导航切换；同一窗口，不再弹独立对话框）
enum { kPageQuota = 0, kPageDisplay, kPageCount };

class TrayUi {
public:
    bool Init(HINSTANCE hinst, HWND msg_wnd, App* app);
    void Shutdown();              // 移除托盘图标、销毁主窗口
    void RecreateIcon();          // TaskbarCreated（Explorer 重启）

    // 托盘回调消息（WM_APP+kMsgTrayNotify 的 w/lParam）
    void OnNotify(WORD wparam, LPARAM lparam);

    // 菜单勾选状态刷新（暂停/仅托盘等随配置变化）
    void RefreshMenuState();

    // 主窗口（额度卡片墙 + 显示设置页面）
    void ToggleDetail();          // 额度页开关（托盘左键）
    void ShowDetail();            // 打开/前置（保持当前页）
    void ShowMainAt(int page);    // 打开/前置并切换到指定页面
    void ShowQuotaAddPanel();     // 打开额度页并展开「添加卡片」面板（首次运行引导）
    void CloseDetail();           // 关闭（存在时）
    void OnDetailClosed();        // 窗口销毁回调（WM_NCDESTROY，窗口过程调用）
    HWND detail_hwnd() const { return detail_; }

    // 数据刷新后同步主窗口（标题 + 额度页重绘）
    void UpdateDetailText();
    // 主循环消息预翻译：主窗口及其子控件的 Tab/Esc/方向键导航
    bool PreTranslateMessage(MSG* m);

    void UpdateTip(const std::wstring& tip);      // 悬停提示（≤127 字符）
    void ShowBubble(const std::wstring& title, const std::wstring& text);
    // 动作回显（第十一轮）：主窗口开着→窗内短暂提示条；窗口关闭→托盘气泡兜底。
    // kind: 0=提示 1=成功 2=失败（kEcho*）。App 层动作（刷新汇总等）经此回显。
    void ShowEcho(int kind, const std::wstring& text);

    // ---- 窗口过程回调接口（DetailProc 使用） ----
    int page() const { return page_; }
    App* AppPtr() const { return app_; }
    void SetPage(int page);       // 切换页面（重建子控件）
    void RebuildPage();           // 销毁并按 page_ 重建页内控件
    void Relayout();              // 按当前客户区重排输入框等子控件
    bool OnCommand(int id, int code, HWND h); // WM_COMMAND 分发；返回=已处理

private:
    void CreatePageControls();    // 按 page_ 创建页内控件（各页输入框）

    HINSTANCE inst_ = nullptr;
    HWND msg_wnd_ = nullptr;
    App* app_ = nullptr;
    HWND detail_ = nullptr;
    bool icon_added_ = false;
    int page_ = kPageQuota;
};

} // namespace qb
