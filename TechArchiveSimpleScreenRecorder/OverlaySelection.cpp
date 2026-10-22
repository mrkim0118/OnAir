#include "OverlaySelection.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>

namespace tasr {
namespace {

constexpr wchar_t kOverlayClassName[] = L"TechArchiveSimpleScreenRecorderOverlayWindow";
constexpr int kOverlayEdge = 8;
constexpr int kOverlayBorderThickness = 6;
constexpr COLORREF kOverlayColor = RGB(230, 54, 54);

LONG ClampLong(LONG value, LONG minimum, LONG maximum) {
    return std::clamp<LONG>(value, minimum, maximum);
}

}  // namespace

RECT OverlaySelection::FullVirtualScreenRect() {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return RECT{ left, top, left + width, top + height };
}

RECT OverlaySelection::NormalizeRegion(RECT region) {
    const RECT bounds = FullVirtualScreenRect();
    const LONG left = ClampLong(region.left, bounds.left, bounds.right - 64);
    const LONG top = ClampLong(region.top, bounds.top, bounds.bottom - 64);
    const LONG right = ClampLong(region.right, left + 64, bounds.right);
    const LONG bottom = ClampLong(region.bottom, top + 64, bounds.bottom);
    return RECT{ left, top, right, bottom };
}

bool OverlaySelection::EnsureWindow() {
    if (hwnd_ != nullptr && IsWindow(hwnd_)) {
        return true;
    }

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kOverlayClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    windowClass.hbrBackground = nullptr;
    RegisterClassExW(&windowClass);

    if (region_.left == 0 && region_.top == 0 && region_.right == 0 && region_.bottom == 0) {
        SetFullRegion();
    }

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        kOverlayClassName,
        L"",
        WS_POPUP,
        region_.left,
        region_.top,
        region_.right - region_.left,
        region_.bottom - region_.top,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (hwnd_ == nullptr) {
        return false;
    }

    SetLayeredWindowAttributes(hwnd_, 0, 78, LWA_ALPHA);
    ApplyWindowShape();
    return true;
}

void OverlaySelection::ApplyWindowShape() const {
    if (hwnd_ == nullptr || !IsWindow(hwnd_)) {
        return;
    }

    RECT client = {};
    GetClientRect(hwnd_, &client);
    const LONG width = std::max<LONG>(0, client.right - client.left);
    const LONG height = std::max<LONG>(0, client.bottom - client.top);
    const LONG innerLeft = std::min<LONG>(width, kOverlayBorderThickness);
    const LONG innerTop = std::min<LONG>(height, kOverlayBorderThickness);
    const LONG innerRight = std::max<LONG>(innerLeft, width - kOverlayBorderThickness);
    const LONG innerBottom = std::max<LONG>(innerTop, height - kOverlayBorderThickness);

    HRGN outer = CreateRectRgn(0, 0, width, height);
    HRGN inner = CreateRectRgn(innerLeft, innerTop, innerRight, innerBottom);
    HRGN frame = CreateRectRgn(0, 0, 0, 0);
    CombineRgn(frame, outer, inner, RGN_DIFF);
    SetWindowRgn(hwnd_, frame, TRUE);
    DeleteObject(outer);
    DeleteObject(inner);
}

bool OverlaySelection::Show() {
    if (!EnsureWindow()) {
        return false;
    }

    region_ = NormalizeRegion(region_);
    SetWindowPos(
        hwnd_,
        HWND_TOPMOST,
        region_.left,
        region_.top,
        region_.right - region_.left,
        region_.bottom - region_.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    return true;
}

void OverlaySelection::Hide() {
    if (hwnd_ != nullptr && IsWindow(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void OverlaySelection::SetRegion(const RECT& region) {
    region_ = NormalizeRegion(region);
    if (hwnd_ != nullptr && IsWindow(hwnd_)) {
        SetWindowPos(
            hwnd_,
            HWND_TOPMOST,
            region_.left,
            region_.top,
            region_.right - region_.left,
            region_.bottom - region_.top,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
        ApplyWindowShape();
    }
}

RECT OverlaySelection::Region() const noexcept {
    return region_;
}

void OverlaySelection::SetFullRegion() {
    region_ = FullVirtualScreenRect();
    if (hwnd_ != nullptr && IsWindow(hwnd_)) {
        SetRegion(region_);
    }
}

LRESULT CALLBACK OverlaySelection::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    OverlaySelection* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OverlaySelection*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<OverlaySelection*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT OverlaySelection::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST: {
        POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT windowRect = {};
        GetWindowRect(hwnd, &windowRect);
        const int x = point.x - windowRect.left;
        const int y = point.y - windowRect.top;
        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top;

        const bool left = x < kOverlayEdge;
        const bool right = x >= width - kOverlayEdge;
        const bool top = y < kOverlayEdge;
        const bool bottom = y >= height - kOverlayEdge;

        if (top && left) {
            return HTTOPLEFT;
        }
        if (top && right) {
            return HTTOPRIGHT;
        }
        if (bottom && left) {
            return HTBOTTOMLEFT;
        }
        if (bottom && right) {
            return HTBOTTOMRIGHT;
        }
        if (left) {
            return HTLEFT;
        }
        if (right) {
            return HTRIGHT;
        }
        if (top) {
            return HTTOP;
        }
        if (bottom) {
            return HTBOTTOM;
        }
        return HTCAPTION;
    }

    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client = {};
        GetClientRect(hwnd, &client);
        HBRUSH fillBrush = CreateSolidBrush(kOverlayColor);
        FillRect(dc, &client, fillBrush);
        DeleteObject(fillBrush);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_MOVE:
    case WM_SIZE:
    case WM_EXITSIZEMOVE: {
        RECT rect = {};
        GetWindowRect(hwnd, &rect);
        region_ = NormalizeRegion(rect);
        ApplyWindowShape();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 64;
        info->ptMinTrackSize.y = 64;
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        hwnd_ = nullptr;
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace tasr
