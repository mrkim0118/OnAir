#include "MainWindow.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

#include "UpdateCheck.h"
#include "resource.h"

namespace tasr {
namespace {

constexpr wchar_t kWindowClassName[] = L"TechArchiveSimpleScreenRecorderWindow";
constexpr wchar_t kOverlayClassName[] = L"TechArchiveSimpleScreenRecorderOverlayWindow";
constexpr wchar_t kAppTitle[] = L"Tech Archive Simple Screen Recorder";
constexpr int kMargin = 12;
constexpr int kRowGap = 10;
constexpr int kTopRowHeight = 34;
constexpr int kSecondRowHeight = 34;
constexpr int kBrowseWidth = 44;
constexpr int kStatusWidth = 104;
constexpr int kRegionWidth = 120;
constexpr int kActionWidth = 82;
constexpr int kTimerStatus = 1001;
constexpr int kOverlayEdge = 8;
constexpr int kOverlayBorderThickness = 6;
constexpr COLORREF kWindowColor = RGB(245, 247, 250);
constexpr COLORREF kEditColor = RGB(255, 255, 255);
constexpr COLORREF kTextColor = RGB(33, 37, 41);
constexpr COLORREF kOverlayColor = RGB(230, 54, 54);

enum ControlId : int {
    kControlOutput = 101,
    kControlBrowse = 102,
    kControlArea = 103,
    kControlFull = 104,
    kControlRecord = 105,
    kControlStop = 106,
};

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

std::wstring EnsureMp4Extension(std::wstring path) {
    std::filesystem::path parsed(path);
    if (parsed.extension().empty()) {
        parsed.replace_extension(L".mp4");
        return parsed.wstring();
    }
    return path;
}

std::wstring FormatClock(std::chrono::seconds seconds) {
    const auto total = seconds.count();
    const auto minutes = total / 60;
    const auto remain = total % 60;

    wchar_t buffer[32] = {};
    StringCchPrintfW(buffer, std::size(buffer), L"REC %02lld:%02lld", minutes, remain);
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

COLORREF BlendColor(COLORREF base, COLORREF overlay, int alpha) {
    const auto blend = [&](int left, int right) {
        return ((left * (255 - alpha)) + (right * alpha)) / 255;
    };

    return RGB(
        blend(GetRValue(base), GetRValue(overlay)),
        blend(GetGValue(base), GetGValue(overlay)),
        blend(GetBValue(base), GetBValue(overlay)));
}

}  // namespace

int MainWindow::Run(HINSTANCE instance, int commandShow) {
    instance_ = instance;
    if (!RegisterWindowClass() || !RegisterOverlayClass()) {
        return 0;
    }

    hwnd_ = CreateWindowExW(
        0,
        kWindowClassName,
        kAppTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        720,
        158,
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
        TranslateMessage(&message);
        DispatchMessageW(&message);
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
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
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

void MainWindow::CreateControls() {
    const DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
    const DWORD buttonStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;

    outputEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", editStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kControlOutput), instance_, nullptr);
    browseButton_ = CreateWindowExW(0, WC_BUTTONW, L"...", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kControlBrowse), instance_, nullptr);
    statusLabel_ = CreateWindowExW(0, WC_STATICW, L"READY", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_RIGHT, 0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);

    regionLabel_ = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);
    areaButton_ = CreateWindowExW(0, WC_BUTTONW, L"AREA", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kControlArea), instance_, nullptr);
    fullButton_ = CreateWindowExW(0, WC_BUTTONW, L"FULL", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kControlFull), instance_, nullptr);
    recordButton_ = CreateWindowExW(0, WC_BUTTONW, L"REC", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kControlRecord), instance_, nullptr);
    stopButton_ = CreateWindowExW(0, WC_BUTTONW, L"STOP", buttonStyle, 0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kControlStop), instance_, nullptr);

    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        StringCchCopyW(metrics.lfMessageFont.lfFaceName, LF_FACESIZE, L"Segoe UI");
        metrics.lfMessageFont.lfHeight = -18;
        uiFont_ = CreateFontIndirectW(&metrics.lfMessageFont);
    }

    backgroundBrush_ = CreateSolidBrush(kWindowColor);
    editBrush_ = CreateSolidBrush(kEditColor);

    HWND controls[] = { outputEdit_, browseButton_, statusLabel_, regionLabel_, areaButton_, fullButton_, recordButton_, stopButton_ };
    for (HWND control : controls) {
        if (control != nullptr && uiFont_ != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
        }
    }

    SendMessageW(outputEdit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"save .mp4"));
    LayoutControls();
    InitializeDefaults();
    RefreshState();
}

void MainWindow::LayoutControls() {
    if (hwnd_ == nullptr) {
        return;
    }

    RECT client = {};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;

    const int topY = kMargin;
    const int secondY = topY + kTopRowHeight + kRowGap;
    const int browseX = width - kMargin - kStatusWidth - kBrowseWidth - kRowGap;
    const int statusX = width - kMargin - kStatusWidth;
    const int outputWidth = std::max(220, browseX - kMargin);

    MoveWindow(outputEdit_, kMargin, topY, outputWidth, kTopRowHeight, TRUE);
    MoveWindow(browseButton_, browseX, topY, kBrowseWidth, kTopRowHeight, TRUE);
    MoveWindow(statusLabel_, statusX, topY, kStatusWidth, kTopRowHeight, TRUE);

    int x = kMargin;
    MoveWindow(regionLabel_, x, secondY, kRegionWidth, kSecondRowHeight, TRUE);
    x += kRegionWidth + kRowGap;
    MoveWindow(areaButton_, x, secondY, kActionWidth, kSecondRowHeight, TRUE);
    x += kActionWidth + kRowGap;
    MoveWindow(fullButton_, x, secondY, kActionWidth, kSecondRowHeight, TRUE);
    x += kActionWidth + kRowGap;
    MoveWindow(recordButton_, x, secondY, kActionWidth, kSecondRowHeight, TRUE);
    x += kActionWidth + kRowGap;
    MoveWindow(stopButton_, x, secondY, kActionWidth, kSecondRowHeight, TRUE);
}

void MainWindow::InitializeDefaults() {
    autoOutputPath_ = BuildDefaultOutputPath();
    selectedRegion_ = CenteredDefaultRegion();
    SetWindowTextW(outputEdit_, autoOutputPath_.c_str());
    UpdateRegionLabel();
}

void MainWindow::StartUpdateThread() {
    updateThread_ = std::thread([this]() { CheckForUpdatesOnce(hwnd_, shutdown_); });
}

void MainWindow::RefreshState() const {
    const BOOL recording = recorder_.IsRecording() ? TRUE : FALSE;
    EnableWindow(outputEdit_, !recording);
    EnableWindow(browseButton_, !recording);
    EnableWindow(areaButton_, !recording);
    EnableWindow(fullButton_, !recording);
    EnableWindow(recordButton_, !recording);
    EnableWindow(stopButton_, recording);
}

void MainWindow::UpdateStatus() const {
    if (statusLabel_ == nullptr) {
        return;
    }

    if (recorder_.IsRecording()) {
        const std::wstring clock = FormatClock(recorder_.Elapsed());
        SetWindowTextW(statusLabel_, clock.c_str());
    } else {
        SetWindowTextW(statusLabel_, L"READY");
    }
    InvalidateRect(statusLabel_, nullptr, TRUE);
}

void MainWindow::UpdateRegionLabel() const {
    if (regionLabel_ == nullptr) {
        return;
    }

    const int width = selectedRegion_.right - selectedRegion_.left;
    const int height = selectedRegion_.bottom - selectedRegion_.top;
    wchar_t buffer[64] = {};
    StringCchPrintfW(buffer, std::size(buffer), L"%d x %d", width, height);
    SetWindowTextW(regionLabel_, buffer);
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
    dialog.lpstrFilter = L"MP4 Video (*.mp4)\0*.mp4\0All Files (*.*)\0*.*\0";
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    dialog.lpstrDefExt = L"mp4";
    dialog.lpstrTitle = L"Save Recording";

    if (GetSaveFileNameW(&dialog) != FALSE) {
        SetWindowTextW(outputEdit_, EnsureMp4Extension(buffer).c_str());
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
        SetLayeredWindowAttributes(overlayWindow_, 0, 78, LWA_ALPHA);
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
    InvalidateRect(regionLabel_, nullptr, TRUE);
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

    std::wstring output = EnsureMp4Extension(CurrentOutputPath());
    if (output.empty()) {
        error = L"Output path is required.";
        return false;
    }

    options.outputPath = output;
    options.region = region;
    options.frameRate = 15;
    options.bitRate = std::clamp<UINT32>(static_cast<UINT32>(width * height * 5), 4'000'000U, 16'000'000U);

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
    SetTimer(hwnd_, kTimerStatus, 200, nullptr);
    UpdateStatus();
    RefreshState();
}

void MainWindow::StopRecording() {
    recorder_.Stop();
    KillTimer(hwnd_, kTimerStatus);
    UpdateStatus();
    RefreshState();

    std::wstring current = CurrentOutputPath();
    if (!activeOutputPath_.empty() && current == activeOutputPath_ && current == autoOutputPath_) {
        autoOutputPath_ = BuildDefaultOutputPath();
        SetWindowTextW(outputEdit_, autoOutputPath_.c_str());
    }
    activeOutputPath_.clear();
}

void MainWindow::DrawButton(const DRAWITEMSTRUCT* drawItem) const {
    if (drawItem == nullptr) {
        return;
    }

    COLORREF fill = RGB(120, 130, 145);
    COLORREF border = RGB(105, 115, 130);
    COLORREF text = RGB(255, 255, 255);

    switch (drawItem->CtlID) {
    case kControlBrowse:
        fill = RGB(71, 85, 105);
        border = RGB(51, 65, 85);
        break;
    case kControlArea:
        fill = RGB(59, 130, 246);
        border = RGB(37, 99, 235);
        break;
    case kControlFull:
        fill = RGB(100, 116, 139);
        border = RGB(71, 85, 105);
        break;
    case kControlRecord:
        fill = RGB(220, 38, 38);
        border = RGB(185, 28, 28);
        break;
    case kControlStop:
        fill = RGB(30, 41, 59);
        border = RGB(15, 23, 42);
        break;
    default:
        break;
    }

    if ((drawItem->itemState & ODS_DISABLED) != 0) {
        fill = BlendColor(fill, RGB(255, 255, 255), 120);
        border = BlendColor(border, RGB(255, 255, 255), 120);
        text = RGB(235, 235, 235);
    } else if ((drawItem->itemState & ODS_SELECTED) != 0) {
        fill = BlendColor(fill, RGB(0, 0, 0), 30);
        border = BlendColor(border, RGB(0, 0, 0), 30);
    }

    HBRUSH fillBrush = CreateSolidBrush(fill);
    HBRUSH borderBrush = CreateSolidBrush(border);
    FillRect(drawItem->hDC, &drawItem->rcItem, fillBrush);
    FrameRect(drawItem->hDC, &drawItem->rcItem, borderBrush);
    DeleteObject(borderBrush);
    DeleteObject(fillBrush);

    SetBkMode(drawItem->hDC, TRANSPARENT);
    SetTextColor(drawItem->hDC, text);

    wchar_t textBuffer[32] = {};
    GetWindowTextW(drawItem->hwndItem, textBuffer, static_cast<int>(std::size(textBuffer)));
    RECT textRect = drawItem->rcItem;
    DrawTextW(drawItem->hDC, textBuffer, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if ((drawItem->itemState & ODS_FOCUS) != 0) {
        RECT focusRect = drawItem->rcItem;
        InflateRect(&focusRect, -3, -3);
        DrawFocusRect(drawItem->hDC, &focusRect);
    }
}

std::wstring MainWindow::CurrentOutputPath() const {
    return ReadWindowText(outputEdit_);
}

std::wstring MainWindow::BuildDefaultOutputPath() const {
    PWSTR videosFolder = nullptr;
    std::wstring baseFolder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, &videosFolder)) && videosFolder != nullptr) {
        baseFolder = videosFolder;
        CoTaskMemFree(videosFolder);
    } else {
        baseFolder = ExecutableDirectory();
    }

    std::filesystem::path folder = std::filesystem::path(baseFolder) / L"Tech Archive Simple Screen Recorder";

    SYSTEMTIME localTime = {};
    GetLocalTime(&localTime);
    wchar_t name[64] = {};
    StringCchPrintfW(
        name,
        std::size(name),
        L"recording-%04u%02u%02u-%02u%02u%02u.mp4",
        localTime.wYear,
        localTime.wMonth,
        localTime.wDay,
        localTime.wHour,
        localTime.wMinute,
        localTime.wSecond);

    return (folder / name).wstring();
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
        RECT client = {};
        GetClientRect(hwnd, &client);
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
        SetWindowRgn(hwnd, frame, TRUE);
        DeleteObject(outer);
        DeleteObject(inner);

        SyncRegionFromOverlay();
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
        if (overlayWindow_ == hwnd) {
            overlayWindow_ = nullptr;
        }
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR)));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 20, 20, LR_DEFAULTCOLOR)));
        CreateControls();
        StartUpdateThread();
        return 0;

    case WM_SIZE:
        LayoutControls();
        return 0;

    case WM_DRAWITEM:
        DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        if (reinterpret_cast<HWND>(lParam) == statusLabel_) {
            SetTextColor(dc, recorder_.IsRecording() ? RGB(220, 38, 38) : RGB(22, 101, 52));
        } else {
            SetTextColor(dc, kTextColor);
        }
        return reinterpret_cast<INT_PTR>(backgroundBrush_);
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, kEditColor);
        SetTextColor(dc, kTextColor);
        return reinterpret_cast<INT_PTR>(editBrush_);
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
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
        case kControlRecord:
            if (HIWORD(wParam) == BN_CLICKED) {
                StartRecording();
            }
            return 0;
        case kControlStop:
            if (HIWORD(wParam) == BN_CLICKED) {
                StopRecording();
            }
            return 0;
        default:
            return 0;
        }

    case WM_TIMER:
        if (wParam == kTimerStatus) {
            if (recorder_.IsRecording()) {
                UpdateStatus();
            } else {
                KillTimer(hwnd_, kTimerStatus);
                UpdateStatus();
                RefreshState();
                const std::wstring error = recorder_.ConsumeLastError();
                if (!error.empty()) {
                    MessageBoxW(hwnd_, error.c_str(), kAppTitle, MB_OK | MB_ICONERROR);
                }
            }
            return 0;
        }
        break;

    case WM_ERASEBKGND: {
        RECT client = {};
        GetClientRect(hwnd_, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, backgroundBrush_);
        return 1;
    }

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 600;
        info->ptMinTrackSize.y = 158;
        info->ptMaxTrackSize.y = 158;
        return 0;
    }

    case WM_CLOSE:
        shutdown_.store(true, std::memory_order_relaxed);
        if (overlayWindow_ != nullptr && IsWindow(overlayWindow_)) {
            DestroyWindow(overlayWindow_);
        }
        StopRecording();
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        shutdown_.store(true, std::memory_order_relaxed);
        if (updateThread_.joinable()) {
            updateThread_.join();
        }
        if (editBrush_ != nullptr) {
            DeleteObject(editBrush_);
            editBrush_ = nullptr;
        }
        if (backgroundBrush_ != nullptr) {
            DeleteObject(backgroundBrush_);
            backgroundBrush_ = nullptr;
        }
        if (uiFont_ != nullptr) {
            DeleteObject(uiFont_);
            uiFont_ = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace tasr
