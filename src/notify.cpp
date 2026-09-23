// notify.cpp — Toast 通知实现。C++/WinRT 头文件只在本翻译单元展开，避免污染其他模块；
// 全部失败路径静默（返回 false），行为兜底由 App 层的托盘气球承担。
#include "notify.hpp"
#include "log.hpp"
#include <shobjidl.h>
#include <propsys.h>
#include <shlobj.h>
#include <string>

// C++/WinRT：/W4 下个别模板可能产生告警，局部关闭（内部实现，不外泄）
#pragma warning(push)
#pragma warning(disable : 4100 4127 4244 4456 4458 4702 5054)
#include <winrt/base.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#pragma warning(pop)

#pragma comment(lib, "windowsapp.lib") // RoGetActivationFactory / WindowsCreateString

namespace qb {
namespace notify {

namespace {

// System.AppUserModel.ID 的 PROPERTYKEY（fmtid 9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3, pid 5；
// 手写常量避免引入 propsys.lib）
constexpr PROPERTYKEY kPkeyAppUserModelId = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

// Toast XML 文本转义（<>&'"；标题/别名可能含用户输入字符）
std::wstring XmlEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s) {
        switch (ch) {
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'&': out += L"&amp;"; break;
            case L'\'': out += L"&apos;"; break;
            case L'"': out += L"&quot;"; break;
            default: out += ch;
        }
    }
    return out;
}

std::wstring ShortcutPath() {
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) return {};
    std::wstring path = std::wstring(programs) + L"\\QuotaBar.lnk";
    CoTaskMemFree(programs);
    return path;
}

} // namespace

bool EnsureToastShortcut() {
    const std::wstring lnk = ShortcutPath();
    if (lnk.empty()) return false;
    if (GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES) return true;

    wchar_t exe[MAX_PATH + 2]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;

    IShellLinkW* sl = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(ShellLink), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&sl));
    if (FAILED(hr)) return false;
    bool ok = false;
    do {
        if (FAILED(sl->SetPath(exe))) break;
        IPropertyStore* ps = nullptr;
        if (FAILED(sl->QueryInterface(IID_PPV_ARGS(&ps)))) break;
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_LPWSTR;
        v.pwszVal = const_cast<PWSTR>(kAumid);
        HRESULT hr2 = ps->SetValue(kPkeyAppUserModelId, v);
        if (SUCCEEDED(hr2)) hr2 = ps->Commit();
        ps->Release();
        if (FAILED(hr2)) break;
        IPersistFile* pf = nullptr;
        if (FAILED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) break;
        hr2 = pf->Save(lnk.c_str(), TRUE);
        pf->Release();
        if (FAILED(hr2)) break;
        ok = true;
    } while (false);
    sl->Release();
    if (ok) Log::Write(LogLevel::Info, "notify", "已创建 Toast AUMID 捷径");
    return ok;
}

bool ShowToast(const wchar_t* aumid, const std::wstring& title, const std::wstring& body) {
    try {
        using namespace winrt;
        using namespace Windows::Data::Xml::Dom;
        using namespace Windows::UI::Notifications;
        std::wstring xml =
            L"<toast activationType=\"system\"><visual><binding "
            L"template=\"ToastGeneric\">";
        xml += L"<text>" + XmlEscape(title) + L"</text>";
        if (!body.empty()) xml += L"<text>" + XmlEscape(body) + L"</text>";
        xml += L"</binding></visual><audio silent=\"true\"/></toast>";
        XmlDocument doc;
        doc.LoadXml(winrt::hstring(xml));
        ToastNotification toast(doc);
        ToastNotificationManager::CreateToastNotifier(winrt::hstring(aumid)).Show(toast);
        return true;
    } catch (const winrt::hresult_error& e) {
        Log::WriteF(LogLevel::Info, "notify", "toast 失败 hresult=0x%08X（回落气球）",
                    (unsigned long)e.code());
        return false;
    } catch (...) {
        return false;
    }
}

} // namespace notify
} // namespace qb
