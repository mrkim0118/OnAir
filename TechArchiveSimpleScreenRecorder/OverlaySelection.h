#pragma once

#include <windows.h>

namespace tasr {

class OverlaySelection {
public:
    bool Show();
    void Hide();
    void SetRegion(const RECT& region);
    RECT Region() const noexcept;
    void SetFullRegion();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    bool EnsureWindow();
    void ApplyWindowShape() const;
    static RECT FullVirtualScreenRect();
    static RECT NormalizeRegion(RECT region);

    HWND hwnd_ = nullptr;
    RECT region_{};
};

}  // namespace tasr
