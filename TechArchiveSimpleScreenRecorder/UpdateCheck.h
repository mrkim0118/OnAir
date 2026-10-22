#pragma once

#include <windows.h>

#include <atomic>

namespace tasr {

void CheckForUpdatesOnce(HWND owner, const std::atomic_bool& shutdownFlag);

}  // namespace tasr
