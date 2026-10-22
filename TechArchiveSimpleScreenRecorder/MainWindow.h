#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

#include "ScreenRecorder.h"

namespace tasr {

class MainWindow {
public:
    int Run(HINSTANCE instance, int commandShow);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleOverlayMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool RegisterWindowClass();
    bool RegisterOverlayClass();
    void CreateControls();
    void LayoutControls();
    void InitializeDefaults();
    void StartUpdateThread();
    void RefreshState() const;
    void UpdateStatus() const;
    void BrowseForOutputPath();
    void ToggleOverlay();
    void EnsureOverlayWindow();
    void SyncOverlayToRegion() const;
    void SyncRegionFromOverlay();
    void UpdateRegionLabel() const;
    void ApplyFullRegion();
    bool ReadOptions(RecordingOptions& options, std::wstring& error);
    void StartRecording();
    void StopRecording();
    void DrawButton(const DRAWITEMSTRUCT* drawItem) const;

    std::wstring CurrentOutputPath() const;
    std::wstring BuildDefaultOutputPath() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND outputEdit_ = nullptr;
    HWND browseButton_ = nullptr;
    HWND regionLabel_ = nullptr;
    HWND areaButton_ = nullptr;
    HWND fullButton_ = nullptr;
    HWND recordButton_ = nullptr;
    HWND stopButton_ = nullptr;
    HWND statusLabel_ = nullptr;
    HFONT uiFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    HWND overlayWindow_ = nullptr;
    RECT selectedRegion_{};

    ScreenRecorder recorder_;
    std::thread updateThread_;
    std::atomic_bool shutdown_{false};
    std::wstring autoOutputPath_;
    std::wstring activeOutputPath_;
};

}  // namespace tasr
