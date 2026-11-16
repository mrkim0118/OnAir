#pragma once

#include <windows.h>

#include <string>

namespace onair {

// Worker thread: returns the newer published version, or empty when up to date or offline.
std::wstring FindNewerVersion();
// UI thread: asks the user whether to install the given version.
bool ConfirmUpdate(HWND owner, const std::wstring& version);
// Worker thread: downloads and launches the installer. Returns an error message, or empty on success.
std::wstring DownloadAndStartInstaller(const std::wstring& version);
// UI thread.
void ShowUpdateError(HWND owner, const std::wstring& error);

}  // namespace onair
