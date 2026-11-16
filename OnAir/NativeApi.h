#pragma once

#include <windows.h>

extern "C" {

__declspec(dllexport) BOOL __stdcall OnAirCheckForUpdatesOnce(HWND owner);
__declspec(dllexport) BOOL __stdcall OnAirShowOverlay();
__declspec(dllexport) BOOL __stdcall OnAirHideOverlay();
__declspec(dllexport) BOOL __stdcall OnAirSetFullRegion();
__declspec(dllexport) BOOL __stdcall OnAirGetRegion(RECT* region);
__declspec(dllexport) BOOL __stdcall OnAirSetRegion(const RECT* region);
__declspec(dllexport) BOOL __stdcall OnAirStartRecording(const wchar_t* outputPath, const RECT* region, wchar_t* errorBuffer, int errorBufferCount);
__declspec(dllexport) void __stdcall OnAirStopRecording();
__declspec(dllexport) BOOL __stdcall OnAirIsRecording();
__declspec(dllexport) long long __stdcall OnAirGetElapsedSeconds();
__declspec(dllexport) int __stdcall OnAirConsumeLastError(wchar_t* buffer, int bufferCount);

}
