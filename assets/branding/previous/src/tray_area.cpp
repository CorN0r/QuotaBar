// tray_area.cpp — 托盘段（2026-09-19 拆分终局）：图标生命周期（初始化/重建/气泡/
// 提示）与右键菜单（含勾选态刷新、OnNotify 分发）。主窗口壳在 detail_shell.cpp。
#include "tray_ui.hpp"
#include "tray_ui_internal.hpp"
#include "app.hpp"
#include "util.hpp"
#include "log.hpp"
#include <shellapi.h>
#include <stdio.h>

#pragma comment(lib, "shell32.lib")

namespace qb {

constexpr UINT kTrayIconId = 1;
constexpr int kIconResId = 102; // 与 assets/quota_bar.rc 的 IDI_QUOTABAR 一致

// =========================================================
// 托盘
// =========================================================
bool TrayUi::Init(HINSTANCE hinst, HWND msg_wnd, App* app) {
    inst_ = hinst;
    msg_wnd_ = msg_wnd;
    app_ = app;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = msg_wnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kMsgTrayNotify;
    nid.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(kIconResId));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"QuotaBar");
    icon_added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (icon_added_) {
        NOTIFYICONDATAW ver{};
        ver.cbSize = sizeof(ver);
        ver.hWnd = msg_wnd_;
        ver.uID = kTrayIconId;
        ver.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &ver);
    }
    EnsureClass(kDetailClass, DetailProc, hinst);
    return icon_added_;
}

void TrayUi::Shutdown() {
    if (icon_added_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = msg_wnd_;
        nid.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        icon_added_ = false;
    }
    if (detail_) {
        DestroyWindow(detail_);
        detail_ = nullptr;
    }
}

void TrayUi::RecreateIcon() {
    if (icon_added_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = msg_wnd_;
        nid.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        icon_added_ = false;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = msg_wnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kMsgTrayNotify;
    nid.hIcon = LoadIconW(inst_, MAKEINTRESOURCEW(kIconResId));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"QuotaBar");
    icon_added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (icon_added_) {
        // 重加后必须重设 V4：NIM_ADD 回退 v0 回调语义（丢 WM_CONTEXTMENU/键盘通知）
        NOTIFYICONDATAW ver{};
        ver.cbSize = sizeof(ver);
        ver.hWnd = msg_wnd_;
        ver.uID = kTrayIconId;
        ver.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &ver);
    }
}

void TrayUi::UpdateTip(const std::wstring& tip) {
    if (!icon_added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = msg_wnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayUi::ShowBubble(const std::wstring& title, const std::wstring& text) {
    if (!icon_added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = msg_wnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// 动作回显路由：主窗口在前 → 窗内提示条（语境就近）；窗口关闭 → 托盘气泡兜底
//（托盘菜单「立即刷新」在窗口关闭时也有反馈，不再静默）
void TrayUi::ShowEcho(int kind, const std::wstring& text) {
    if (detail_) {
        UiEcho(detail_, kind, text.c_str());
        return;
    }
    ShowBubble(L"QuotaBar", text);
}

void TrayUi::RefreshMenuState() {
    // 菜单每次弹出按当前配置重建（含勾选），无需持久状态
}

void TrayUi::OnNotify(WORD /*wparam*/, LPARAM lparam) {
    switch (LOWORD(lparam)) {
        case WM_LBUTTONUP:
            Log::WriteF(LogLevel::Info, "ui", "tray lbuttonup -> toggle");
            ToggleDetail();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            const AppConfig& cfg = app_->Cfg();
            bool tray_only = cfg.display.mode == DisplayMode::TrayOnly;
            HMENU m = CreatePopupMenu();
            auto add = [&](UINT id, const wchar_t* t, UINT flags = MF_STRING) {
                AppendMenuW(m, flags, id, t);
            };
            enum { kCmdRefresh = 1, kCmdDetail, kCmdDisplay,
                   kCmdPause, kCmdTrayOnly, kCmdExit };
            add(kCmdRefresh, L"立即刷新(&R)");
            add(kCmdDetail, L"额度详情(&D)");    // 账户卡片（含账户设置）在此页
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            add(kCmdDisplay, L"显示设置(&S)");   // 位置微调已并入此页
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            add(kCmdPause, L"暂停查询(&P)", MF_STRING | (cfg.query.pause ? MF_CHECKED : 0));
            add(kCmdTrayOnly, L"仅托盘(&T)", MF_STRING | (tray_only ? MF_CHECKED : 0));
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            add(kCmdExit, L"退出(&X)");

            SetForegroundWindow(msg_wnd_); // 菜单跟踪需要前台
            POINT pt;
            GetCursorPos(&pt);
            int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                          pt.x, pt.y, 0, msg_wnd_, nullptr);
            DestroyMenu(m);
            PostMessageW(msg_wnd_, WM_NULL, 0, 0); // 使菜单立即收起
            switch (cmd) {
                case kCmdRefresh: app_->CmdRefreshAll(); break;
                case kCmdDetail: ShowMainAt(kPageQuota); break;
                case kCmdDisplay: ShowMainAt(kPageDisplay); break;
                case kCmdPause: app_->CmdTogglePause(); break;
                case kCmdTrayOnly: app_->CmdToggleTrayOnly(); break;
                case kCmdExit: app_->CmdExit(); break;
                default: break;
            }
            break;
        }
        default:
            break;
    }
}


} // namespace qb