// Windows-only desktop integration (compiled only on Windows). Strings are UTF-8.
#pragma once
#include <string>
#include <vector>

// The user's music folders: the Music known folder (follows OneDrive / relocated
// folders) plus the plain %USERPROFILE%\Music and OneDrive\Music if they differ.
std::vector<std::string> win32MusicFolders();
// Opens a folder in Explorer without flashing a console window.
bool win32OpenFolder(const std::string& path);
// Gives the window the exe's icon (title bar, taskbar, Alt-Tab).
void win32SetWindowIcon(const void* hwnd);
// UTF-8 <-> UTF-16 for Win32 "W" APIs.
std::wstring win32Wide(const std::string& utf8);
std::string win32Utf8(const wchar_t* wide);
