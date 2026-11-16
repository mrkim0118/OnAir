#include "MainWindow.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>

// GDI+ headers use min/max, which NOMINMAX removes.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

#include "UpdateCheck.h"
#include "resource.h"

namespace onair {
namespace {

constexpr wchar_t kWindowClassName[] = L"OnAirWindow";
constexpr wchar_t kOverlayClassName[] = L"OnAirOverlayWindow";
constexpr wchar_t kSettingsClassName[] = L"OnAirSettingsWindow";
constexpr wchar_t kAppTitle[] = L"OnAir";

// Layout in 96-DPI units; scaled with MainWindow::Scale.
constexpr int kPadding = 16;
constexpr int kRowHeight = 36;
constexpr int kRowGap = 12;
constexpr int kGap = 8;
constexpr int kClientWidth = 640;
constexpr int kClientHeight = kPadding * 2 + kRowHeight * 2 + kRowGap;
constexpr int kStatusWidth = 128;
constexpr int kRegionWidth = 128;
constexpr int kAreaWidth = 92;
constexpr int kFullWidth = 92;
constexpr int kRecordWidth = 116;
constexpr int kRadius = 8;
constexpr int kOverlayEdge = 8;
constexpr int kOverlayBorder = 4;
constexpr int kOverlayHandleWidth = 140;
constexpr int kOverlayHandleHeight = 26;
constexpr BYTE kOverlayAlpha = 220;
constexpr int kSettingsWidth = 460;
constexpr int kSettingsPadding = 20;
constexpr int kSettingsLabelWidth = 140;
constexpr int kSettingsChoiceGap = 4;
constexpr int kSettingsDoneWidth = 96;

constexpr UINT_PTR kTimerStatus = 1001;
constexpr UINT kMsgUpdateAvailable = WM_APP + 1;
constexpr UINT kMsgUpdateFailed = WM_APP + 2;

constexpr COLORREF kBackground = RGB(24, 26, 31);
constexpr COLORREF kSurface = RGB(36, 39, 46);
constexpr COLORREF kSurfaceHover = RGB(46, 50, 59);
constexpr COLORREF kSurfacePressed = RGB(30, 33, 39);
constexpr COLORREF kBorder = RGB(54, 58, 68);
constexpr COLORREF kText = RGB(236, 238, 241);
constexpr COLORREF kMuted = RGB(150, 156, 168);
constexpr COLORREF kDisabledText = RGB(88, 93, 104);
constexpr COLORREF kWhite = RGB(255, 255, 255);
constexpr COLORREF kAccent = RGB(239, 68, 68);
constexpr COLORREF kAccentHover = RGB(248, 90, 90);
constexpr COLORREF kAccentPressed = RGB(210, 40, 40);
constexpr COLORREF kAccentActive = RGB(185, 28, 28);
constexpr COLORREF kAccentDim = RGB(96, 36, 40);
constexpr COLORREF kReady = RGB(34, 197, 94);

constexpr wchar_t kGlyphFolder[] = L"\xE838";
constexpr wchar_t kGlyphArea[] = L"\xE7A8";
constexpr wchar_t kGlyphFull[] = L"\xE740";
constexpr wchar_t kGlyphMove[] = L"\xE7C2";
constexpr wchar_t kGlyphSettings[] = L"\xE713";

enum ControlId : int {
    kControlOutput = 101,
    kControlBrowse = 102,
    kControlArea = 103,
    kControlFull = 104,
    kControlRecord = 105,
    kControlSettings = 106,
    kControlChoiceBase = 200,  // Settings choice buttons: kControlChoiceBase + row * 10 + index.
    kControlSettingsDone = 299,
};

struct SettingRow {
    const wchar_t* title;
    const wchar_t* key;
    std::initializer_list<const wchar_t*> labels;
    int defaultIndex;
};

// Defaults match the original fixed behaviour: MP4, ~0.3 bits/pixel, 15 fps, system audio on.
const SettingRow kSettingRows[] = {
    { L"Format", L"Format", { L"MP4", L"WMV" }, 0 },
    { L"Quality", L"Quality", { L"Low", L"Standard", L"High", L"Max" }, 2 },
    { L"Frame rate (fps)", L"FrameRate", { L"15", L"24", L"30", L"60" }, 0 },
    { L"System audio", L"Audio", { L"On", L"Off" }, 0 },
};
constexpr int kRowFormat = 0;
constexpr int kRowQuality = 1;
constexpr int kRowFrameRate = 2;
constexpr int kRowAudio = 3;
constexpr int kSettingRowCount = 4;
constexpr double kQualityBitsPerPixel[] = { 0.07, 0.15, 0.3, 0.6 };
constexpr UINT32 kFrameRates[] = { 15, 24, 30, 60 };
constexpr int kSettingsHeight = kSettingsPadding * 2 + kSettingRowCount * (kRowHeight + kRowGap) + kGap + kRowHeight;
constexpr DWORD kSettingsStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;

std::wstring ReadWindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring value(static_cast<size_t>(std::max(0, length)) + 1, L'\0');
    GetWindowTextW(window, value.data(), length + 1);
    value.resize(static_cast<size_t>(std::max(0, length)));
    return value;
}

std::wstring ExecutableDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD written = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (written == 0 || written >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(buffer).parent_path().wstring();
}

// Media Foundation picks the container from the extension, so it must always match the selected format.
std::wstring EnsureExtension(const std::wstring& path, const wchar_t* extension) {
    if (path.empty()) {
        return path;
    }
    std::filesystem::path parsed(path);
    parsed.replace_extension(extension);
    return parsed.wstring();
}

std::wstring SettingsFilePath() {
    PWSTR localAppData = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData)) && localAppData != nullptr) {
        base = localAppData;
    }
    CoTaskMemFree(localAppData);
    if (base.empty()) {
        return {};
    }
    return (std::filesystem::path(base) / L"OnAir" / L"settings.ini").wstring();
}

void ApplyDarkTitleBar(HWND hwnd, COLORREF caption) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));  // Windows 11 only; ignored elsewhere.
}

std::wstring FormatClock(std::chrono::seconds seconds) {
    const long long total = seconds.count();
    wchar_t buffer[32] = {};
    if (total >= 3600) {
        StringCchPrintfW(buffer, std::size(buffer), L"REC %lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
    } else {
        StringCchPrintfW(buffer, std::size(buffer), L"REC %02lld:%02lld", total / 60, total % 60);
    }
    return buffer;
}

std::wstring FormatRegionSize(const RECT& region) {
    wchar_t buffer[64] = {};
    StringCchPrintfW(buffer, std::size(buffer), L"%ld \x00D7 %ld", region.right - region.left, region.bottom - region.top);
    return buffer;
}

RECT FullVirtualScreenRect() {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return RECT{ left, top, left + width, top + height };
}

int EvenFloor(int value) {
    if (value < 2) {
        return 2;
    }
    return (value % 2 == 0) ? value : (value - 1);
}

RECT NormalizeRegion(RECT region) {
    const RECT bounds = FullVirtualScreenRect();
    const LONG left = std::clamp<LONG>(region.left, bounds.left, bounds.right - 64);
    const LONG top = std::clamp<LONG>(region.top, bounds.top, bounds.bottom - 64);
    const LONG right = std::clamp<LONG>(region.right, left + 64, bounds.right);
    const LONG bottom = std::clamp<LONG>(region.bottom, top + 64, bounds.bottom);
    return RECT{ left, top, right, bottom };
}

RECT CenteredDefaultRegion() {
    const RECT full = FullVirtualScreenRect();
    const LONG width = std::max<LONG>(640, (full.right - full.left) * 2 / 3);
    const LONG height = std::max<LONG>(360, (full.bottom - full.top) * 2 / 3);
    const int left = full.left + ((full.right - full.left) - width) / 2;
    const int top = full.top + ((full.bottom - full.top) - height) / 2;
    return NormalizeRegion(RECT{ left, top, left + width, top + height });
}

void PostString(HWND hwnd, UINT message, std::wstring value) {
    auto* payload = new std::wstring(std::move(value));
    if (!PostMessageW(hwnd, message, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

bool FontExists(const wchar_t* face) {
    LOGFONTW query = {};
    query.lfCharSet = DEFAULT_CHARSET;
    StringCchCopyW(query.lfFaceName, LF_FACESIZE, face);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(
        dc,
        &query,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) -> int {
            *reinterpret_cast<bool*>(found) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found),
        0);
    ReleaseDC(nullptr, dc);
    return found;
}

HFONT CreateUiFont(const wchar_t* face, int pixelHeight, int weight, UINT dpi) {
    return CreateFontW(
        -MulDiv(pixelHeight, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
        0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

SIZE MeasureText(HDC dc, HFONT font, const wchar_t* text) {
    SIZE size = {};
    const HGDIOBJ old = SelectObject(dc, font);
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
    SelectObject(dc, old);
    return size;
}

void DrawTextAt(HDC dc, HFONT font, const wchar_t* text, RECT rect, COLORREF color, UINT format = DT_LEFT) {
    const HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rect, format | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(dc, old);
}

// Draws an icon glyph followed by an optional label, centered as one group inside bounds.
void DrawIconLabel(HDC dc, const RECT& bounds, HFONT iconFont, HFONT labelFont, const wchar_t* icon, const wchar_t* label,
                   COLORREF iconColor, COLORREF textColor, int gap) {
    const SIZE iconSize = MeasureText(dc, iconFont, icon);
    const SIZE labelSize = (*label != L'\0') ? MeasureText(dc, labelFont, label) : SIZE{};
    const int total = iconSize.cx + (labelSize.cx > 0 ? gap + labelSize.cx : 0);
    const int x = bounds.left + ((bounds.right - bounds.left) - total) / 2;
    DrawTextAt(dc, iconFont, icon, RECT{ x, bounds.top, x + iconSize.cx, bounds.bottom }, iconColor);
    if (labelSize.cx > 0) {
        const int labelX = x + iconSize.cx + gap;
        DrawTextAt(dc, labelFont, label, RECT{ labelX, bounds.top, labelX + labelSize.cx, bounds.bottom }, textColor);
    }
}

Gdiplus::Color ToGdiplus(COLORREF color) {
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

// Anti-aliased rounded rectangle; pass CLR_NONE to skip the fill or the outline.
void FillRoundRect(Gdiplus::Graphics& graphics, const RECT& rect, float radius, COLORREF fill, COLORREF border, float stroke = 1.0f) {
    const float inset = (border != CLR_NONE) ? stroke / 2.0f : 0.0f;
    const float x = static_cast<float>(rect.left) + inset;
    const float y = static_cast<float>(rect.top) + inset;
    const float w = static_cast<float>(rect.right - rect.left) - inset * 2.0f;
    const float h = static_cast<float>(rect.bottom - rect.top) - inset * 2.0f;
    const float d = std::min({ radius * 2.0f, w, h });
    if (d <= 0.0f) {
        return;
    }

    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();

    if (fill != CLR_NONE) {
        Gdiplus::SolidBrush brush(ToGdiplus(fill));
        graphics.FillPath(&brush, &path);
    }
    if (border != CLR_NONE) {
        Gdiplus::Pen pen(ToGdiplus(border), stroke);
        graphics.DrawPath(&pen, &path);
    }
}

void PrepareGraphics(Gdiplus::Graphics& graphics) {
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
}

// Off-screen bitmap that is copied to the target DC on destruction (flicker-free painting).
class MemoryCanvas {
public:
    MemoryCanvas(HDC target, int width, int height)
        : target_(target),
          width_(width),
          height_(height),
          dc_(CreateCompatibleDC(target)),
          bitmap_(CreateCompatibleBitmap(target, width, height)),
          old_(SelectObject(dc_, bitmap_)) {}

    ~MemoryCanvas() {
        BitBlt(target_, 0, 0, width_, height_, dc_, 0, 0, SRCCOPY);
        SelectObject(dc_, old_);
        DeleteObject(bitmap_);
        DeleteDC(dc_);
    }

    MemoryCanvas(const MemoryCanvas&) = delete;
    MemoryCanvas& operator=(const MemoryCanvas&) = delete;

    HDC dc() const { return dc_; }

private:
    HDC target_;
    int width_;
    int height_;
    HDC dc_;
    HBITMAP bitmap_;
    HGDIOBJ old_;
};

class GdiplusSession {
public:
    GdiplusSession() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token_, &input, nullptr) != Gdiplus::Ok) {
            token_ = 0;
        }
    }
    ~GdiplusSession() {
        if (token_ != 0) {
            Gdiplus::GdiplusShutdown(token_);
        }
    }
    GdiplusSession(const GdiplusSession&) = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;

private:
    ULONG_PTR token_ = 0;
};

constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;

}  // namespace

int MainWindow::Run(HINSTANCE instance, int commandShow) {
    const GdiplusSession gdiplus;
    instance_ = instance;
    if (!RegisterWindowClass() || !RegisterOverlayClass() || !RegisterSettingsClass()) {
        return 0;
    }

    hwnd_ = CreateWindowExW(
        0,
        kWindowClassName,
        kAppTitle,
        kWindowStyle,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        kClientWidth,
        kClientHeight,
        nullptr,
        nullptr,
        instance_,
        this);

    if (hwnd_ == nullptr) {
        return 0;
    }

    ShowWindow(hwnd_, commandShow);
    UpdateWindow(hwnd_);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Gives Tab / Shift+Tab / Space keyboard navigation between the controls.
        const bool handled = (settingsWindow_ != nullptr && IsDialogMessageW(settingsWindow_, &message)) || IsDialogMessageW(hwnd_, &message);
        if (!handled) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    return static_cast<int>(message.wParam);
}

bool MainWindow::RegisterWindowClass() {
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 20, 20, LR_DEFAULTCOLOR));
    return RegisterClassExW(&windowClass) != 0;
}

bool MainWindow::RegisterOverlayClass() {
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = OverlayProc;
    windowClass.hInstance = instance_;
    windowClass.lpszClassName = kOverlayClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    windowClass.hbrBackground = nullptr;
    return RegisterClassExW(&windowClass) != 0;
}

bool MainWindow::RegisterSettingsClass() {
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = SettingsProc;
    windowClass.hInstance = instance_;
    windowClass.lpszClassName = kSettingsClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 20, 20, LR_DEFAULTCOLOR));
    return RegisterClassExW(&windowClass) != 0;
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(createStruct->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::OverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleOverlayMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::SettingsProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleSettingsMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::ButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData) {
    auto* self = reinterpret_cast<MainWindow*>(refData);
    switch (message) {
    case WM_MOUSEMOVE:
        if (self->hoveredButton_ != hwnd) {
            if (self->hoveredButton_ != nullptr) {
                InvalidateRect(self->hoveredButton_, nullptr, FALSE);
            }
            self->hoveredButton_ = hwnd;
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&track);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (self->hoveredButton_ == hwnd) {
            self->hoveredButton_ = nullptr;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_ERASEBKGND:
        return 1;  // DrawButton paints every pixel.
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, ButtonSubclassProc, 0);
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

int MainWindow::Scale(int value) const {
    return MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
}

void MainWindow::CreateControls() {
    const DWORD buttonStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;

    outputEdit_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kControlOutput), instance_, nullptr);
    browseButton_ = CreateWindowExW(0, WC_BUTTONW, L"Browse", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kControlBrowse), instance_, nullptr);
    areaButton_ = CreateWindowExW(0, WC_BUTTONW, L"Area", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kControlArea), instance_, nullptr);
    fullButton_ = CreateWindowExW(0, WC_BUTTONW, L"Full", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kControlFull), instance_, nullptr);
    recordButton_ = CreateWindowExW(0, WC_BUTTONW, L"Record", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kControlRecord), instance_, nullptr);
    settingsButton_ = CreateWindowExW(0, WC_BUTTONW, L"Settings", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kControlSettings), instance_, nullptr);

    for (HWND button : { browseButton_, areaButton_, fullButton_, recordButton_, settingsButton_ }) {
        SetWindowSubclass(button, ButtonSubclassProc, 0, reinterpret_cast<DWORD_PTR>(this));
    }

    backgroundBrush_ = CreateSolidBrush(kBackground);
    surfaceBrush_ = CreateSolidBrush(kSurface);

    SendMessageW(outputEdit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Save as .mp4"));
    ApplyDpi(GetDpiForWindow(hwnd_), nullptr);
    InitializeDefaults();
    RefreshState();
}

void MainWindow::CreateFonts() {
    DeleteFonts();
    textFont_ = CreateUiFont(L"Segoe UI", 14, FW_NORMAL, dpi_);
    labelFont_ = CreateUiFont(L"Segoe UI", 14, FW_SEMIBOLD, dpi_);
    iconFont_ = CreateUiFont(FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets", 14, FW_NORMAL, dpi_);
}

void MainWindow::DeleteFonts() {
    for (HFONT* font : { &textFont_, &labelFont_, &iconFont_ }) {
        if (*font != nullptr) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
}

void MainWindow::ApplyDpi(UINT dpi, const RECT* suggested) {
    dpi_ = dpi;
    CreateFonts();
    SendMessageW(outputEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont_), FALSE);

    RECT frame = { 0, 0, Scale(kClientWidth), Scale(kClientHeight) };
    AdjustWindowRectExForDpi(&frame, kWindowStyle, FALSE, 0, dpi_);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    if (suggested != nullptr) {
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    LayoutControls();
}

void MainWindow::LayoutControls() {
    if (hwnd_ == nullptr) {
        return;
    }

    const int width = Scale(kClientWidth);
    const int padding = Scale(kPadding);
    const int row = Scale(kRowHeight);
    const int gap = Scale(kGap);
    const int firstY = padding;
    const int secondY = padding + row + Scale(kRowGap);

    const int settingsX = width - padding - row;
    const int browseX = settingsX - gap - row;
    fieldRect_ = { padding, firstY, browseX - gap, firstY + row };

    TEXTMETRICW metrics = {};
    HDC dc = GetDC(hwnd_);
    const HGDIOBJ old = SelectObject(dc, textFont_);
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);

    const int inset = Scale(12);
    const int editHeight = metrics.tmHeight;
    MoveWindow(outputEdit_, fieldRect_.left + inset, firstY + (row - editHeight) / 2, (fieldRect_.right - fieldRect_.left) - inset * 2, editHeight, TRUE);
    MoveWindow(browseButton_, browseX, firstY, row, row, TRUE);
    MoveWindow(settingsButton_, settingsX, firstY, row, row, TRUE);

    statusRect_ = { padding, secondY, padding + Scale(kStatusWidth), secondY + row };
    regionRect_ = { statusRect_.right + gap, secondY, statusRect_.right + gap + Scale(kRegionWidth), secondY + row };

    int x = width - padding - Scale(kRecordWidth);
    MoveWindow(recordButton_, x, secondY, Scale(kRecordWidth), row, TRUE);
    x -= gap + Scale(kFullWidth);
    MoveWindow(fullButton_, x, secondY, Scale(kFullWidth), row, TRUE);
    x -= gap + Scale(kAreaWidth);
    MoveWindow(areaButton_, x, secondY, Scale(kAreaWidth), row, TRUE);

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::InitializeDefaults() {
    autoOutputPath_ = BuildDefaultOutputPath();
    selectedRegion_ = CenteredDefaultRegion();
    SetWindowTextW(outputEdit_, autoOutputPath_.c_str());
    UpdateRegionLabel();
}

void MainWindow::StartUpdateThread() {
    updateThread_ = std::thread([hwnd = hwnd_]() {
        std::wstring version = FindNewerVersion();
        if (!version.empty()) {
            PostString(hwnd, kMsgUpdateAvailable, std::move(version));
        }
    });
}

void MainWindow::RefreshState() const {
    const BOOL idle = recorder_.IsRecording() ? FALSE : TRUE;
    EnableWindow(outputEdit_, idle);
    EnableWindow(browseButton_, idle);
    EnableWindow(areaButton_, idle);
    EnableWindow(fullButton_, idle);
    EnableWindow(settingsButton_, idle);
    InvalidateRect(recordButton_, nullptr, FALSE);
    InvalidateRect(hwnd_, &statusRect_, FALSE);
}

void MainWindow::UpdateRegionLabel() const {
    InvalidateRect(hwnd_, &regionRect_, FALSE);
}

void MainWindow::BrowseForOutputPath() {
    wchar_t buffer[4096] = {};
    std::wstring current = CurrentOutputPath();
    if (!current.empty()) {
        StringCchCopyW(buffer, std::size(buffer), current.c_str());
    }

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    const bool wmv = SelectedFormat() == OutputFormat::Wmv;
    dialog.lpstrFilter = wmv ? L"WMV Video (*.wmv)\0*.wmv\0" : L"MP4 Video (*.mp4)\0*.mp4\0";
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = wmv ? L"wmv" : L"mp4";
    dialog.lpstrTitle = L"Save Recording";

    if (GetSaveFileNameW(&dialog) != FALSE) {
        SetWindowTextW(outputEdit_, EnsureExtension(buffer, FileExtension()).c_str());
    }
}

void MainWindow::EnsureOverlayWindow() {
    if (overlayWindow_ != nullptr && IsWindow(overlayWindow_)) {
        return;
    }

    overlayWindow_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        kOverlayClassName,
        L"",
        WS_POPUP,
        selectedRegion_.left,
        selectedRegion_.top,
        selectedRegion_.right - selectedRegion_.left,
        selectedRegion_.bottom - selectedRegion_.top,
        hwnd_,
        nullptr,
        instance_,
        this);

    if (overlayWindow_ != nullptr) {
        SetLayeredWindowAttributes(overlayWindow_, 0, kOverlayAlpha, LWA_ALPHA);
    }
}

void MainWindow::SyncOverlayToRegion() const {
    if (overlayWindow_ == nullptr || !IsWindow(overlayWindow_)) {
        return;
    }

    const RECT region = NormalizeRegion(selectedRegion_);
    SetWindowPos(
        overlayWindow_,
        HWND_TOPMOST,
        region.left,
        region.top,
        region.right - region.left,
        region.bottom - region.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void MainWindow::SyncRegionFromOverlay() {
    if (overlayWindow_ == nullptr || !IsWindow(overlayWindow_)) {
        return;
    }

    RECT rect = {};
    GetWindowRect(overlayWindow_, &rect);
    selectedRegion_ = NormalizeRegion(rect);
    UpdateRegionLabel();
}

void MainWindow::ToggleOverlay() {
    EnsureOverlayWindow();
    if (overlayWindow_ == nullptr) {
        return;
    }

    if (IsWindowVisible(overlayWindow_)) {
        ShowWindow(overlayWindow_, SW_HIDE);
    } else {
        SyncOverlayToRegion();
        ShowWindow(overlayWindow_, SW_SHOWNORMAL);
        SetForegroundWindow(overlayWindow_);
    }
    InvalidateRect(areaButton_, nullptr, FALSE);
}

void MainWindow::ApplyFullRegion() {
    selectedRegion_ = FullVirtualScreenRect();
    UpdateRegionLabel();
    SyncOverlayToRegion();
}

bool MainWindow::ReadOptions(RecordingOptions& options, std::wstring& error) {
    RECT region = NormalizeRegion(selectedRegion_);
    const int width = EvenFloor(region.right - region.left);
    const int height = EvenFloor(region.bottom - region.top);

    if (width <= 1 || height <= 1) {
        error = L"Recording area is too small.";
        return false;
    }

    region.right = region.left + width;
    region.bottom = region.top + height;
    selectedRegion_ = region;
    UpdateRegionLabel();

    std::wstring output = EnsureExtension(CurrentOutputPath(), FileExtension());
    if (output.empty()) {
        error = L"Output path is required.";
        return false;
    }

    options.outputPath = output;
    options.region = region;
    options.format = SelectedFormat();
    options.frameRate = kFrameRates[settings_[kRowFrameRate]];
    options.captureAudio = settings_[kRowAudio] == 0;
    const double bitRate = static_cast<double>(width) * height * options.frameRate * kQualityBitsPerPixel[settings_[kRowQuality]];
    options.bitRate = static_cast<UINT32>(std::clamp(bitRate, 1'000'000.0, 50'000'000.0));

    SetWindowTextW(outputEdit_, output.c_str());
    return true;
}

void MainWindow::StartRecording() {
    RecordingOptions options;
    std::wstring error;
    if (!ReadOptions(options, error)) {
        MessageBoxW(hwnd_, error.c_str(), kAppTitle, MB_OK | MB_ICONWARNING);
        return;
    }

    if (overlayWindow_ != nullptr && IsWindowVisible(overlayWindow_)) {
        ShowWindow(overlayWindow_, SW_HIDE);
    }
    if (settingsWindow_ != nullptr) {
        DestroyWindow(settingsWindow_);
    }

    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(options.outputPath), ec) && !ec) {
        const int response = MessageBoxW(hwnd_, L"Overwrite existing file?", kAppTitle, MB_YESNO | MB_ICONQUESTION);
        if (response != IDYES) {
            return;
        }
    }

    if (!recorder_.Start(options, error)) {
        MessageBoxW(hwnd_, error.c_str(), kAppTitle, MB_OK | MB_ICONERROR);
        return;
    }

    activeOutputPath_ = options.outputPath;
    SetTimer(hwnd_, kTimerStatus, 250, nullptr);
    RefreshState();
}

void MainWindow::StopRecording() {
    recorder_.Stop();
    KillTimer(hwnd_, kTimerStatus);
    RefreshState();

    const std::wstring error = recorder_.ConsumeLastError();
    if (!error.empty()) {
        MessageBoxW(hwnd_, error.c_str(), kAppTitle, MB_OK | MB_ICONERROR);
    }

    std::wstring current = CurrentOutputPath();
    if (!activeOutputPath_.empty() && current == activeOutputPath_ && current == autoOutputPath_) {
        autoOutputPath_ = BuildDefaultOutputPath();
        SetWindowTextW(outputEdit_, autoOutputPath_.c_str());
    }
    activeOutputPath_.clear();
}

void MainWindow::PaintWindow(HDC target) const {
    RECT client = {};
    GetClientRect(hwnd_, &client);
    MemoryCanvas canvas(target, client.right, client.bottom);
    HDC dc = canvas.dc();
    FillRect(dc, &client, backgroundBrush_);

    const bool recording = recorder_.IsRecording();
    const bool blinkOn = (GetTickCount64() / 500) % 2 == 0;
    const int dot = Scale(8);
    const int dotX = statusRect_.left + Scale(16);
    const int centerY = (statusRect_.top + statusRect_.bottom) / 2;
    const float stroke = static_cast<float>(dpi_) / USER_DEFAULT_SCREEN_DPI;
    {
        Gdiplus::Graphics graphics(dc);
        PrepareGraphics(graphics);
        const bool editFocused = GetFocus() == outputEdit_;
        FillRoundRect(graphics, fieldRect_, static_cast<float>(Scale(kRadius)), kSurface, editFocused ? kAccent : kBorder, stroke);

        const float pill = (statusRect_.bottom - statusRect_.top) / 2.0f;
        FillRoundRect(graphics, statusRect_, pill, kSurface, CLR_NONE);
        FillRoundRect(graphics, regionRect_, pill, kSurface, CLR_NONE);

        const COLORREF dotColor = recording ? (blinkOn ? kAccent : kAccentDim) : kReady;
        Gdiplus::SolidBrush dotBrush(ToGdiplus(dotColor));
        graphics.FillEllipse(&dotBrush, static_cast<float>(dotX), centerY - dot / 2.0f, static_cast<float>(dot), static_cast<float>(dot));
    }

    const std::wstring status = recording ? FormatClock(recorder_.Elapsed()) : std::wstring(L"Ready");
    DrawTextAt(dc, labelFont_, status.c_str(), RECT{ dotX + dot + Scale(10), statusRect_.top, statusRect_.right - Scale(12), statusRect_.bottom }, kText);

    const std::wstring size = FormatRegionSize(selectedRegion_);
    DrawIconLabel(dc, regionRect_, iconFont_, labelFont_, kGlyphArea, size.c_str(), kMuted, kText, Scale(8));
}

void MainWindow::DrawButton(const DRAWITEMSTRUCT* drawItem) const {
    if (drawItem == nullptr) {
        return;
    }

    const int width = drawItem->rcItem.right - drawItem->rcItem.left;
    const int height = drawItem->rcItem.bottom - drawItem->rcItem.top;
    MemoryCanvas canvas(drawItem->hDC, width, height);
    HDC dc = canvas.dc();
    const RECT bounds = { 0, 0, width, height };
    FillRect(dc, &bounds, backgroundBrush_);

    const UINT id = drawItem->CtlID;
    const bool disabled = (drawItem->itemState & ODS_DISABLED) != 0;
    const bool pressed = (drawItem->itemState & ODS_SELECTED) != 0;
    const bool hovered = drawItem->hwndItem == hoveredButton_;
    const bool focused = (drawItem->itemState & ODS_FOCUS) != 0 && (drawItem->itemState & ODS_NOFOCUSRECT) == 0;
    const bool primary = id == kControlRecord || id == kControlSettingsDone;
    const bool recording = recorder_.IsRecording();
    const bool choice = id >= kControlChoiceBase && id < kControlSettingsDone;
    const int choiceRow = choice ? static_cast<int>(id - kControlChoiceBase) / 10 : 0;
    const int choiceIndex = choice ? static_cast<int>(id - kControlChoiceBase) % 10 : 0;
    const bool selected = choice && settings_[choiceRow] == choiceIndex;
    const bool overlayActive = id == kControlArea && overlayWindow_ != nullptr && IsWindowVisible(overlayWindow_);

    const wchar_t* glyph = nullptr;
    const wchar_t* label = L"";
    switch (id) {
    case kControlBrowse: glyph = kGlyphFolder; break;
    case kControlSettings: glyph = kGlyphSettings; break;
    case kControlArea: glyph = kGlyphArea; label = L"Area"; break;
    case kControlFull: glyph = kGlyphFull; label = L"Full"; break;
    case kControlRecord: label = recording ? L"Stop" : L"Record"; break;
    case kControlSettingsDone: label = L"Done"; break;
    default:
        if (choice) {
            label = kSettingRows[choiceRow].labels.begin()[choiceIndex];
        }
        break;
    }

    COLORREF fill = kSurface;
    COLORREF border = kBorder;
    COLORREF textColor = kText;
    COLORREF iconColor = kMuted;
    if (primary) {
        const bool active = id == kControlRecord && recording;
        fill = pressed ? kAccentPressed : hovered ? kAccentHover : active ? kAccentActive : kAccent;
        border = CLR_NONE;
        textColor = iconColor = kWhite;
    } else if (disabled) {
        fill = kSurfacePressed;
        border = CLR_NONE;
        textColor = iconColor = kDisabledText;
    } else if (selected) {
        fill = kAccentDim;
        border = kAccent;
        textColor = kWhite;
    } else {
        fill = pressed ? kSurfacePressed : hovered ? kSurfaceHover : kSurface;
        if (choice) {
            border = CLR_NONE;
            textColor = hovered ? kText : kMuted;
        }
        if (overlayActive) {
            border = kAccent;
            iconColor = kAccent;
        }
    }

    const float radius = static_cast<float>(Scale(kRadius));
    const float stroke = static_cast<float>(dpi_) / USER_DEFAULT_SCREEN_DPI;
    const int gap = Scale(8);
    const bool hasMark = id == kControlRecord;
    const SIZE labelSize = MeasureText(dc, labelFont_, label);
    const int mark = Scale(10);
    const int markX = (width - (mark + gap + labelSize.cx)) / 2;
    {
        Gdiplus::Graphics graphics(dc);
        PrepareGraphics(graphics);
        FillRoundRect(graphics, bounds, radius, fill, border, stroke);
        if (focused) {
            const int inset = Scale(3);
            FillRoundRect(graphics, RECT{ inset, inset, width - inset, height - inset }, radius - inset, CLR_NONE, primary ? kWhite : kText, stroke);
        }
        if (hasMark) {
            const RECT markRect = { markX, (height - mark) / 2, markX + mark, (height - mark) / 2 + mark };
            if (recording) {
                FillRoundRect(graphics, markRect, Scale(2) * 1.0f, kWhite, CLR_NONE);
            } else {
                Gdiplus::SolidBrush markBrush(ToGdiplus(kWhite));
                graphics.FillEllipse(&markBrush, static_cast<float>(markRect.left), static_cast<float>(markRect.top), static_cast<float>(mark), static_cast<float>(mark));
            }
        }
    }

    if (hasMark) {
        const int labelX = markX + mark + gap;
        DrawTextAt(dc, labelFont_, label, RECT{ labelX, 0, labelX + labelSize.cx, height }, textColor);
    } else if (glyph != nullptr) {
        DrawIconLabel(dc, bounds, iconFont_, labelFont_, glyph, label, iconColor, textColor, gap);
    } else {
        DrawTextAt(dc, labelFont_, label, bounds, textColor, DT_CENTER);
    }
}

OutputFormat MainWindow::SelectedFormat() const {
    return settings_[kRowFormat] == 1 ? OutputFormat::Wmv : OutputFormat::Mp4;
}

const wchar_t* MainWindow::FileExtension() const {
    return SelectedFormat() == OutputFormat::Wmv ? L".wmv" : L".mp4";
}

void MainWindow::LoadSettings() {
    const std::wstring path = SettingsFilePath();
    for (int row = 0; row < kSettingRowCount; ++row) {
        const SettingRow& setting = kSettingRows[row];
        const int value = path.empty() ? setting.defaultIndex : static_cast<int>(GetPrivateProfileIntW(L"Recording", setting.key, setting.defaultIndex, path.c_str()));
        settings_[row] = (value >= 0 && value < static_cast<int>(setting.labels.size())) ? value : setting.defaultIndex;
    }
}

void MainWindow::SaveSettings() const {
    const std::wstring path = SettingsFilePath();
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    for (int row = 0; row < kSettingRowCount; ++row) {
        WritePrivateProfileStringW(L"Recording", kSettingRows[row].key, std::to_wstring(settings_[row]).c_str(), path.c_str());
    }
}

void MainWindow::SelectChoice(int row, int index) {
    if (settings_[row] == index) {
        return;
    }
    settings_[row] = index;
    SaveSettings();
    if (row == kRowFormat) {
        ApplyFormatToOutputPath();
    }
}

void MainWindow::ApplyFormatToOutputPath() {
    const std::wstring current = CurrentOutputPath();
    const std::wstring updated = EnsureExtension(current, FileExtension());
    if (current == autoOutputPath_) {
        autoOutputPath_ = updated;
    }
    SetWindowTextW(outputEdit_, updated.c_str());
}

void MainWindow::ShowSettings() {
    if (settingsWindow_ != nullptr) {
        SetForegroundWindow(settingsWindow_);
        return;
    }

    RECT frame = { 0, 0, Scale(kSettingsWidth), Scale(kSettingsHeight) };
    AdjustWindowRectExForDpi(&frame, kSettingsStyle, FALSE, 0, dpi_);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    RECT owner = {};
    GetWindowRect(hwnd_, &owner);
    const int x = owner.left + ((owner.right - owner.left) - width) / 2;
    const int y = owner.top + Scale(32);

    settingsWindow_ = CreateWindowExW(0, kSettingsClassName, L"Settings", kSettingsStyle, x, y, width, height, hwnd_, nullptr, instance_, this);
    if (settingsWindow_ != nullptr) {
        ShowWindow(settingsWindow_, SW_SHOW);
    }
}

void MainWindow::CreateSettingsControls(HWND hwnd) {
    const DWORD buttonStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
    const int padding = Scale(kSettingsPadding);
    const int row = Scale(kRowHeight);
    const int left = padding + Scale(kSettingsLabelWidth);
    const int right = Scale(kSettingsWidth) - padding;
    const int gap = Scale(kSettingsChoiceGap);

    for (int rowIndex = 0; rowIndex < kSettingRowCount; ++rowIndex) {
        const SettingRow& setting = kSettingRows[rowIndex];
        const int count = static_cast<int>(setting.labels.size());
        const int y = padding + rowIndex * (row + Scale(kRowGap));
        const int buttonWidth = ((right - left) - gap * (count - 1)) / count;
        for (int index = 0; index < count; ++index) {
            HWND button = CreateWindowExW(
                0, WC_BUTTONW, setting.labels.begin()[index], buttonStyle,
                left + index * (buttonWidth + gap), y, buttonWidth, row,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlChoiceBase + rowIndex * 10 + index)), instance_, nullptr);
            SetWindowSubclass(button, ButtonSubclassProc, 0, reinterpret_cast<DWORD_PTR>(this));
        }
    }

    const int doneY = Scale(kSettingsHeight) - padding - row;
    HWND done = CreateWindowExW(
        0, WC_BUTTONW, L"Done", buttonStyle | BS_DEFPUSHBUTTON,
        right - Scale(kSettingsDoneWidth), doneY, Scale(kSettingsDoneWidth), row,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kControlSettingsDone)), instance_, nullptr);
    SetWindowSubclass(done, ButtonSubclassProc, 0, reinterpret_cast<DWORD_PTR>(this));
}

void MainWindow::PaintSettings(HWND hwnd, HDC target) const {
    RECT client = {};
    GetClientRect(hwnd, &client);
    MemoryCanvas canvas(target, client.right, client.bottom);
    HDC dc = canvas.dc();
    FillRect(dc, &client, backgroundBrush_);

    const int padding = Scale(kSettingsPadding);
    const int row = Scale(kRowHeight);
    for (int rowIndex = 0; rowIndex < kSettingRowCount; ++rowIndex) {
        const int y = padding + rowIndex * (row + Scale(kRowGap));
        DrawTextAt(dc, labelFont_, kSettingRows[rowIndex].title, RECT{ padding, y, padding + Scale(kSettingsLabelWidth), y + row }, kText);
    }

    const int doneY = Scale(kSettingsHeight) - padding - row;
    const RECT divider = { padding, doneY - Scale(kGap), client.right - padding, doneY - Scale(kGap) + 1 };
    HBRUSH dividerBrush = CreateSolidBrush(kBorder);
    FillRect(dc, &divider, dividerBrush);
    DeleteObject(dividerBrush);

    const RECT hint = { padding, doneY, client.right - padding - Scale(kSettingsDoneWidth) - Scale(kGap), doneY + row };
    DrawTextAt(dc, textFont_, L"Saved automatically. Applies to the next recording.", hint, kMuted, DT_END_ELLIPSIS);
}

RECT MainWindow::OverlayHandleRect(HWND hwnd) const {
    RECT client = {};
    GetClientRect(hwnd, &client);
    const int border = Scale(kOverlayBorder);
    const int width = std::min<int>(Scale(kOverlayHandleWidth), client.right - border * 2);
    const int left = (client.right - width) / 2;
    return RECT{ left, border, left + width, border + Scale(kOverlayHandleHeight) };
}

void MainWindow::PaintOverlay(HWND hwnd, HDC dc) const {
    RECT client = {};
    GetClientRect(hwnd, &client);
    HBRUSH accent = CreateSolidBrush(kAccent);
    FillRect(dc, &client, accent);
    DeleteObject(accent);

    // Only the window region (frame + handle) is visible, so the label lands on the handle.
    const std::wstring size = FormatRegionSize(selectedRegion_);
    DrawIconLabel(dc, OverlayHandleRect(hwnd), iconFont_, labelFont_, kGlyphMove, size.c_str(), kWhite, kWhite, Scale(6));
}

std::wstring MainWindow::CurrentOutputPath() const {
    return ReadWindowText(outputEdit_);
}

std::wstring MainWindow::BuildDefaultOutputPath() const {
    PWSTR videosFolder = nullptr;
    std::wstring baseFolder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, &videosFolder)) && videosFolder != nullptr) {
        baseFolder = videosFolder;
    } else {
        baseFolder = ExecutableDirectory();
    }
    CoTaskMemFree(videosFolder);  // Required even when the call fails.

    std::filesystem::path folder = std::filesystem::path(baseFolder) / L"OnAir";

    SYSTEMTIME localTime = {};
    GetLocalTime(&localTime);
    wchar_t name[64] = {};
    StringCchPrintfW(
        name,
        std::size(name),
        L"recording-%04u%02u%02u-%02u%02u%02u",
        localTime.wYear,
        localTime.wMonth,
        localTime.wDay,
        localTime.wHour,
        localTime.wMinute,
        localTime.wSecond);

    return (folder / name).wstring() + FileExtension();
}

LRESULT MainWindow::HandleOverlayMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST: {
        POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT windowRect = {};
        GetWindowRect(hwnd, &windowRect);
        const int x = point.x - windowRect.left;
        const int y = point.y - windowRect.top;
        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top;

        const RECT handle = OverlayHandleRect(hwnd);
        if (PtInRect(&handle, POINT{ x, y })) {
            return HTCAPTION;
        }

        const int edge = Scale(kOverlayEdge);
        const bool left = x < edge;
        const bool right = x >= width - edge;
        const bool top = y < edge;
        const bool bottom = y >= height - edge;

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

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCAPTION) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        PaintOverlay(hwnd, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_MOVE:
    case WM_SIZE:
    case WM_EXITSIZEMOVE: {
        RECT client = {};
        GetClientRect(hwnd, &client);
        const LONG width = std::max<LONG>(0, client.right - client.left);
        const LONG height = std::max<LONG>(0, client.bottom - client.top);
        const LONG border = Scale(kOverlayBorder);
        const LONG innerLeft = std::min<LONG>(width, border);
        const LONG innerTop = std::min<LONG>(height, border);
        const LONG innerRight = std::max<LONG>(innerLeft, width - border);
        const LONG innerBottom = std::max<LONG>(innerTop, height - border);

        const RECT handle = OverlayHandleRect(hwnd);
        const int round = Scale(kRadius);
        HRGN outer = CreateRectRgn(0, 0, width, height);
        HRGN inner = CreateRectRgn(innerLeft, innerTop, innerRight, innerBottom);
        HRGN handleRegion = CreateRoundRectRgn(handle.left, handle.top - round, handle.right + 1, handle.bottom + 1, round, round);
        HRGN frame = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(frame, outer, inner, RGN_DIFF);
        CombineRgn(frame, frame, handleRegion, RGN_OR);
        SetWindowRgn(hwnd, frame, TRUE);  // The system owns `frame` from here on.
        DeleteObject(outer);
        DeleteObject(inner);
        DeleteObject(handleRegion);

        SyncRegionFromOverlay();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 64;
        info->ptMinTrackSize.y = 64;
        return 0;
    }

    // The frame marks exact screen pixels, so it must not be rescaled when it crosses monitors with different scaling.
    case WM_GETDPISCALEDSIZE: {
        RECT rect = {};
        GetWindowRect(hwnd, &rect);
        *reinterpret_cast<SIZE*>(lParam) = SIZE{ rect.right - rect.left, rect.bottom - rect.top };
        return TRUE;
    }

    case WM_DPICHANGED:
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            ShowWindow(hwnd, SW_HIDE);
            InvalidateRect(areaButton_, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        InvalidateRect(areaButton_, nullptr, FALSE);
        return 0;

    case WM_DESTROY:
        if (overlayWindow_ == hwnd) {
            overlayWindow_ = nullptr;
        }
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT MainWindow::HandleSettingsMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        ApplyDarkTitleBar(hwnd, kBackground);
        CreateSettingsControls(hwnd);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        PaintSettings(hwnd, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_DRAWITEM:
        DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;

    // shortcut: sized and laid out for the main window's DPI; does not re-layout if dragged to a monitor with other scaling.
    case WM_DPICHANGED:
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == kControlSettingsDone || id == IDOK || id == IDCANCEL) {
            DestroyWindow(hwnd);
        } else if (id >= kControlChoiceBase && id < kControlSettingsDone && HIWORD(wParam) == BN_CLICKED) {
            SelectChoice((id - kControlChoiceBase) / 10, (id - kControlChoiceBase) % 10);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        }
        return 0;
    }

    case WM_DESTROY:
        if (hoveredButton_ != nullptr && GetParent(hoveredButton_) == hwnd) {
            hoveredButton_ = nullptr;
        }
        settingsWindow_ = nullptr;
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        ApplyDarkTitleBar(hwnd_, kBackground);
        LoadSettings();
        CreateControls();
        // Auto-update is disabled for now. Uncomment to check the update server on launch.
        // StartUpdateThread();
        return 0;
    }

    case WM_DPICHANGED:
        ApplyDpi(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        PaintWindow(dc);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;  // PaintWindow paints every pixel.

    case WM_DRAWITEM:
        DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {  // A disabled edit asks for WM_CTLCOLORSTATIC.
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, kSurface);
        SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lParam)) ? kText : kMuted);
        return reinterpret_cast<INT_PTR>(surfaceBrush_);
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kControlOutput:
            if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS) {
                InvalidateRect(hwnd_, &fieldRect_, FALSE);
            }
            return 0;
        case kControlBrowse:
            if (HIWORD(wParam) == BN_CLICKED) {
                BrowseForOutputPath();
            }
            return 0;
        case kControlArea:
            if (HIWORD(wParam) == BN_CLICKED) {
                ToggleOverlay();
            }
            return 0;
        case kControlFull:
            if (HIWORD(wParam) == BN_CLICKED) {
                ApplyFullRegion();
            }
            return 0;
        case kControlSettings:
            if (HIWORD(wParam) == BN_CLICKED) {
                ShowSettings();
            }
            return 0;
        case kControlRecord:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (recorder_.IsRecording()) {
                    StopRecording();
                } else {
                    StartRecording();
                }
            }
            return 0;
        default:
            return 0;
        }

    case WM_TIMER:
        if (wParam == kTimerStatus) {
            if (recorder_.IsRecording()) {
                InvalidateRect(hwnd_, &statusRect_, FALSE);
            } else {
                StopRecording();  // The worker ended on its own (an error); surface it.
            }
            return 0;
        }
        break;

    case kMsgUpdateAvailable: {
        const std::unique_ptr<std::wstring> version(reinterpret_cast<std::wstring*>(lParam));
        if (ConfirmUpdate(hwnd_, *version)) {
            if (updateThread_.joinable()) {
                updateThread_.join();
            }
            updateThread_ = std::thread([hwnd = hwnd_, target = *version]() {
                std::wstring error = DownloadAndStartInstaller(target);
                if (!error.empty()) {
                    PostString(hwnd, kMsgUpdateFailed, std::move(error));
                }
            });
        }
        return 0;
    }

    case kMsgUpdateFailed: {
        const std::unique_ptr<std::wstring> error(reinterpret_cast<std::wstring*>(lParam));
        ShowUpdateError(hwnd_, *error);
        return 0;
    }

    case WM_CLOSE:
        if (overlayWindow_ != nullptr && IsWindow(overlayWindow_)) {
            DestroyWindow(overlayWindow_);
        }
        StopRecording();
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        if (updateThread_.joinable()) {
            updateThread_.join();
        }
        DeleteFonts();
        for (HBRUSH* brush : { &backgroundBrush_, &surfaceBrush_ }) {
            if (*brush != nullptr) {
                DeleteObject(*brush);
                *brush = nullptr;
            }
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace onair
