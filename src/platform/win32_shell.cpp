// Windows desktop integration: known folders, Explorer, window icon.
#include "win32_shell.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iterator>

namespace fs = std::filesystem;

std::wstring win32Wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), w.data(), n);
    return w;
}

std::string win32Utf8(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, s.data(), n, nullptr, nullptr);
    return s;
}

namespace {

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring envVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
    return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

}  // namespace

std::vector<std::string> win32MusicFolders() {
    std::vector<fs::path> candidates;
    if (auto m = knownFolder(FOLDERID_Music); !m.empty()) candidates.emplace_back(m);
    if (auto home = envVar(L"USERPROFILE"); !home.empty()) candidates.push_back(fs::path(home) / L"Music");
    if (auto od = envVar(L"OneDrive"); !od.empty()) candidates.push_back(fs::path(od) / L"Music");

    std::vector<std::string> out;
    std::vector<std::wstring> seen;
    for (auto& c : candidates) {
        std::error_code ec;
        if (!fs::is_directory(c, ec)) continue;
        fs::path canon = fs::weakly_canonical(c, ec);
        if (ec) canon = c;
        std::wstring key = canon.wstring();
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t ch) { return wchar_t(std::towlower(ch)); });
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        out.push_back(win32Utf8(canon.c_str()));
    }
    return out;
}

bool win32OpenFolder(const std::string& path) {
    std::wstring w = win32Wide(path);
    return INT_PTR(ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

void win32SetWindowIcon(const void* hwnd) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HWND w = HWND(const_cast<void*>(hwnd));
    if (!w) return;
    auto load = [&](int metricX, int metricY) {
        return HANDLE(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(metricX), GetSystemMetrics(metricY), 0));
    };
    if (HANDLE big = load(SM_CXICON, SM_CYICON)) SendMessageW(w, WM_SETICON, ICON_BIG, LPARAM(big));
    if (HANDLE small = load(SM_CXSMICON, SM_CYSMICON)) SendMessageW(w, WM_SETICON, ICON_SMALL, LPARAM(small));
}
