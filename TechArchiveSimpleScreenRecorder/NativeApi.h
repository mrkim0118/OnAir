#pragma once

#include <windows.h>

extern "C" {

__declspec(dllexport) BOOL __stdcall TasrCheckForUpdatesOnce(HWND owner);
__declspec(dllexport) BOOL __stdcall TasrShowOverlay();
__declspec(dllexport) BOOL __stdcall TasrHideOverlay();
__declspec(dllexport) BOOL __stdcall TasrSetFullRegion();
__declspec(dllexport) BOOL __stdcall TasrGetRegion(RECT* region);
__declspec(dllexport) BOOL __stdcall TasrSetRegion(const RECT* region);
__declspec(dllexport) BOOL __stdcall TasrStartRecording(const wchar_t* outputPath, const RECT* region, wchar_t* errorBuffer, int errorBufferCount);
__declspec(dllexport) void __stdcall TasrStopRecording();
__declspec(dllexport) BOOL __stdcall TasrIsRecording();
__declspec(dllexport) long long __stdcall TasrGetElapsedSeconds();
__declspec(dllexport) int __stdcall TasrConsumeLastError(wchar_t* buffer, int bufferCount);

}
