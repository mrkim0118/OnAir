#include "NativeApi.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

#include "OverlaySelection.h"
#include "ScreenRecorder.h"
#include "UpdateCheck.h"

namespace {

onair::ScreenRecorder g_recorder;
onair::OverlaySelection g_overlay;
std::mutex g_errorMutex;
std::wstring g_lastError;

void StoreLastError(const std::wstring& error) {
    std::lock_guard lock(g_errorMutex);
    g_lastError = error;
}

void ClearLastError() {
    std::lock_guard lock(g_errorMutex);
    g_lastError.clear();
}

std::wstring TakeLastError() {
    std::lock_guard lock(g_errorMutex);
    std::wstring error = g_lastError;
    g_lastError.clear();
    return error;
}

int CopyMessage(const std::wstring& value, wchar_t* buffer, int bufferCount) {
    if (buffer == nullptr || bufferCount <= 0) {
        return static_cast<int>(value.size());
    }

    const int maxChars = std::max(0, bufferCount - 1);
    const int copied = std::min(maxChars, static_cast<int>(value.size()));
    if (copied > 0) {
        memcpy(buffer, value.data(), static_cast<size_t>(copied) * sizeof(wchar_t));
    }
    buffer[copied] = L'\0';
    return copied;
}

RECT NormalizeForRecording(const RECT& input) {
    RECT value = input;
    if (value.right < value.left) {
        std::swap(value.left, value.right);
    }
    if (value.bottom < value.top) {
        std::swap(value.top, value.bottom);
    }

    const LONG width = value.right - value.left;
    const LONG height = value.bottom - value.top;
    if ((width % 2) != 0) {
        --value.right;
    }
    if ((height % 2) != 0) {
        --value.bottom;
    }
    return value;
}

}  // namespace

extern "C" {

BOOL __stdcall OnAirCheckForUpdatesOnce(HWND owner) {
    const std::wstring version = onair::FindNewerVersion();
    if (!version.empty() && onair::ConfirmUpdate(owner, version)) {
        const std::wstring error = onair::DownloadAndStartInstaller(version);
        if (!error.empty()) {
            onair::ShowUpdateError(owner, error);
        }
    }
    return TRUE;
}

BOOL __stdcall OnAirShowOverlay() {
    return g_overlay.Show() ? TRUE : FALSE;
}

BOOL __stdcall OnAirHideOverlay() {
    g_overlay.Hide();
    return TRUE;
}

BOOL __stdcall OnAirSetFullRegion() {
    g_overlay.SetFullRegion();
    return TRUE;
}

BOOL __stdcall OnAirGetRegion(RECT* region) {
    if (region == nullptr) {
        return FALSE;
    }

    *region = g_overlay.Region();
    return TRUE;
}

BOOL __stdcall OnAirSetRegion(const RECT* region) {
    if (region == nullptr) {
        return FALSE;
    }

    g_overlay.SetRegion(*region);
    return TRUE;
}

BOOL __stdcall OnAirStartRecording(const wchar_t* outputPath, const RECT* region, wchar_t* errorBuffer, int errorBufferCount) {
    if (outputPath == nullptr || region == nullptr) {
        CopyMessage(L"Output path and region are required.", errorBuffer, errorBufferCount);
        return FALSE;
    }

    ClearLastError();
    onair::RecordingOptions options = {};
    options.outputPath = outputPath;
    options.region = NormalizeForRecording(*region);
    const LONG width = options.region.right - options.region.left;
    const LONG height = options.region.bottom - options.region.top;
    options.frameRate = 15;
    options.bitRate = std::clamp<UINT32>(static_cast<UINT32>(width * height * 5), 4'000'000U, 16'000'000U);

    std::wstring error;
    if (!g_recorder.Start(options, error)) {
        CopyMessage(error, errorBuffer, errorBufferCount);
        return FALSE;
    }

    return TRUE;
}

void __stdcall OnAirStopRecording() {
    g_recorder.Stop();
}

BOOL __stdcall OnAirIsRecording() {
    return g_recorder.IsRecording() ? TRUE : FALSE;
}

long long __stdcall OnAirGetElapsedSeconds() {
    return static_cast<long long>(g_recorder.Elapsed().count());
}

int __stdcall OnAirConsumeLastError(wchar_t* buffer, int bufferCount) {
    std::wstring error = g_recorder.ConsumeLastError();
    if (error.empty()) {
        error = TakeLastError();
    }
    return CopyMessage(error, buffer, bufferCount);
}

}
