#pragma once

#include <windows.h>

#include <string>
#include <thread>

#include "ScreenRecorder.h"

namespace onair {

class MainWindow {
public:
    int Run(HINSTANCE instance, int commandShow);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR refData);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleOverlayMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleSettingsMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool RegisterWindowClass();
    bool RegisterOverlayClass();
    bool RegisterSettingsClass();
    void ShowSettings();
    void CreateSettingsControls(HWND hwnd);
    void PaintSettings(HWND hwnd, HDC dc) const;
    void LoadSettings();
    void SaveSettings() const;
    void SelectChoice(int row, int index);
    void ApplyFormatToOutputPath();
    OutputFormat SelectedFormat() const;
    const wchar_t* FileExtension() const;
    void CreateControls();
    void ApplyDpi(UINT dpi, const RECT* suggested);
    void CreateFonts();
    void DeleteFonts();
    void LayoutControls();
    void InitializeDefaults();
    void StartUpdateThread();
    void RefreshState() const;
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
    void PaintWindow(HDC dc) const;
    void PaintOverlay(HWND hwnd, HDC dc) const;
    void DrawButton(const DRAWITEMSTRUCT* drawItem) const;
    RECT OverlayHandleRect(HWND hwnd) const;
    int Scale(int value) const;

    std::wstring CurrentOutputPath() const;
    std::wstring BuildDefaultOutputPath() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND outputEdit_ = nullptr;
    HWND browseButton_ = nullptr;
    HWND areaButton_ = nullptr;
    HWND fullButton_ = nullptr;
    HWND recordButton_ = nullptr;
    HWND settingsButton_ = nullptr;
    HWND settingsWindow_ = nullptr;
    HWND hoveredButton_ = nullptr;
    HWND overlayWindow_ = nullptr;
    HFONT textFont_ = nullptr;
    HFONT labelFont_ = nullptr;
    HFONT iconFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH surfaceBrush_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    RECT fieldRect_{};
    RECT statusRect_{};
    RECT regionRect_{};
    RECT selectedRegion_{};
    int settings_[4] = {};  // Selected choice index per settings row (see kSettingRows).

    ScreenRecorder recorder_;
    std::thread updateThread_;
    std::wstring autoOutputPath_;
    std::wstring activeOutputPath_;
};

}  // namespace onair
