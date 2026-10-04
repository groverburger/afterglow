// Windows folder dialog (IFileOpenDialog); Linux via the desktop's own tools.
#include "folder_picker.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>

#include "win32_shell.h"

bool pickFolderDialog(const char* title, std::string* out) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool ok = false;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(win32Wide(title).c_str());
        if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    *out = win32Utf8(path);
                    ok = !out->empty();
                }
                CoTaskMemFree(path);
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

#else

#include <cstdio>

namespace {

bool runPicker(const std::string& cmd, std::string* out) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    std::string s;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) s += buf;
    int rc = pclose(p);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (rc != 0 || s.empty()) return false;
    *out = s;
    return true;
}

}  // namespace

bool pickFolderDialog(const char* title, std::string* out) {
    std::string t = title;
    return runPicker("zenity --file-selection --directory --title=\"" + t + "\" 2>/dev/null", out) ||
           runPicker("kdialog --getexistingdirectory ~ --title \"" + t + "\" 2>/dev/null", out);
}

#endif
