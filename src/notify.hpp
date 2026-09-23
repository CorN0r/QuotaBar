// notify.hpp — 提醒通道（B 迭代 2026-09-15）：Win11 Toast 通知 + 托盘气球兜底。
// Toast 走 C++/WinRT（Windows.UI.Notifications），免打包应用需一个带 AUMID 属性的
// 开始菜单捷径做应用标识——首用时自动创建（幂等）。任何失败静默返回 false，
// 由调用方（App::Notify_）回落托盘气球；不弹系统对话框、不写日志之外的状态。
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
namespace notify {

// 应用的 AUMID（与开始菜单捷径的 System.AppUserModel.ID 一致）
inline constexpr const wchar_t* kAumid = L"QuotaBar.QuotaBar";

// 确保带 AUMID 的捷径存在（首用时创建；已存在直接成功）。需在已初始化 COM 的线程调用。
bool EnsureToastShortcut();

// 发送一条 Toast。成功返回 true；Focus Assist/通知关闭或任何 COM 失败 → false（走气球）。
bool ShowToast(const wchar_t* aumid, const std::wstring& title, const std::wstring& body);

} // namespace notify
} // namespace qb
