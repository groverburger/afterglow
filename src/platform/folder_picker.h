// Native "choose a folder" dialog. Blocks until the user picks or cancels, so
// never call it while holding the engine mutex (audio would stall).
#pragma once
#include <string>

// Returns false when cancelled or when no dialog is available on this system.
bool pickFolderDialog(const char* title, std::string* out);
