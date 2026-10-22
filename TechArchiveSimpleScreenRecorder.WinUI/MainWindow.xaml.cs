using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Velopack;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace TechArchiveSimpleScreenRecorder.WinUI;

public sealed partial class MainWindow : Window
{
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(250) };
    private readonly IntPtr _hwnd;
    private bool _checkingForUpdates;

    public MainWindow()
    {
        InitializeComponent();

        _hwnd = WindowNative.GetWindowHandle(this);
        var windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(_hwnd);
        var appWindow = AppWindow.GetFromWindowId(windowId);
        appWindow.Resize(new Windows.Graphics.SizeInt32(1060, 760));

        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        SystemBackdrop = new MicaBackdrop();

        _timer.Tick += Timer_Tick;
        _timer.Start();

        Activated += MainWindow_Activated;
        Closed += MainWindow_Closed;

        OutputPathTextBox.Text = BuildDefaultOutputPath();
        NativeMethods.TasrSetFullRegion();
        RefreshRegion();
        RefreshState();
        // Disabled on startup because the installed app crashed while checking updates during launch.
        // _ = CheckForUpdatesAsync();
    }

    private void MainWindow_Activated(object sender, WindowActivatedEventArgs args)
    {
        RefreshRegion();
        RefreshState();
    }

    private void MainWindow_Closed(object sender, WindowEventArgs args)
    {
        NativeMethods.TasrHideOverlay();
        NativeMethods.TasrStopRecording();
    }

    private void Timer_Tick(object? sender, object e)
    {
        RefreshRegion();
        RefreshState();

        var error = NativeMethods.ConsumeLastError();
        if (!string.IsNullOrWhiteSpace(error))
        {
            MessageTextBlock.Text = error;
        }
    }

    private async void BrowseButton_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FileSavePicker();
        InitializeWithWindow.Initialize(picker, _hwnd);
        picker.SuggestedStartLocation = PickerLocationId.VideosLibrary;
        picker.FileTypeChoices.Add("MP4 Video", new List<string> { ".mp4" });
        picker.SuggestedFileName = "recording";

        var file = await picker.PickSaveFileAsync();
        if (file is not null)
        {
            OutputPathTextBox.Text = file.Path;
            MessageTextBlock.Text = "Output path updated";
        }
    }

    private void FrameButton_Click(object sender, RoutedEventArgs e)
    {
        var overlayVisible = NativeMethods.TasrShowOverlay();
        MessageTextBlock.Text = overlayVisible ? "Drag the red frame to set the capture area" : "Could not show the capture frame";
        RefreshRegion();
    }

    private void FullButton_Click(object sender, RoutedEventArgs e)
    {
        NativeMethods.TasrSetFullRegion();
        RefreshRegion();
        MessageTextBlock.Text = "Capture area set to the full screen";
    }

    private void RecordButton_Click(object sender, RoutedEventArgs e)
    {
        if (!NativeMethods.TasrGetRegion(out var region))
        {
            MessageTextBlock.Text = "Could not read the capture area";
            return;
        }

        if (string.IsNullOrWhiteSpace(OutputPathTextBox.Text))
        {
            MessageTextBlock.Text = "Choose an output path first";
            return;
        }

        NativeMethods.TasrHideOverlay();
        if (!NativeMethods.TasrStartRecording(OutputPathTextBox.Text, region, out var error))
        {
            MessageTextBlock.Text = string.IsNullOrWhiteSpace(error) ? "Recording could not be started" : error;
            return;
        }

        MessageTextBlock.Text = "Recording started";
        RefreshState();
    }

    private void StopButton_Click(object sender, RoutedEventArgs e)
    {
        NativeMethods.TasrStopRecording();
        MessageTextBlock.Text = "Recording stopped";
        RefreshState();
    }

    private void RefreshRegion()
    {
        if (!NativeMethods.TasrGetRegion(out var region))
        {
            return;
        }

        var width = Math.Max(0, region.Right - region.Left);
        var height = Math.Max(0, region.Bottom - region.Top);
        RegionTextBlock.Text = $"{width} x {height}";
    }

    private void RefreshState()
    {
        var isRecording = NativeMethods.TasrIsRecording();
        RecordButton.IsEnabled = !isRecording;
        StopButton.IsEnabled = isRecording;
        OutputPathTextBox.IsEnabled = !isRecording;
        BrowseButton.IsEnabled = !isRecording;
        FrameButton.IsEnabled = !isRecording;
        FullButton.IsEnabled = !isRecording;

        StatusTextBlock.Text = isRecording ? "REC" : "READY";
        StatusTextBlock.Foreground = new SolidColorBrush(isRecording ? Windows.UI.Color.FromArgb(255, 226, 61, 61) : Windows.UI.Color.FromArgb(255, 255, 255, 255));

        var elapsed = TimeSpan.FromSeconds(NativeMethods.TasrGetElapsedSeconds());
        ElapsedTextBlock.Text = $"{elapsed.Minutes:00}:{elapsed.Seconds:00}";
    }

    private static string BuildDefaultOutputPath()
    {
        var baseFolder = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
        var targetFolder = Path.Combine(baseFolder, "Tech Archive Simple Screen Recorder");
        var fileName = $"recording-{DateTime.Now:yyyyMMdd-HHmmss}.mp4";
        return Path.Combine(targetFolder, fileName);
    }

    private async Task CheckForUpdatesAsync()
    {
        if (_checkingForUpdates)
        {
            return;
        }

        var updateSource = ResolveUpdateSource();
        if (string.IsNullOrWhiteSpace(updateSource))
        {
            UpdateStateTextBlock.Text = "Update source unavailable";
            return;
        }

        _checkingForUpdates = true;
        UpdateStateTextBlock.Text = "Checking for updates...";

        try
        {
            var manager = new UpdateManager(updateSource);
            if (!manager.IsInstalled)
            {
                UpdateStateTextBlock.Text = "Developer build";
                return;
            }

            var update = await manager.CheckForUpdatesAsync();
            if (update is null)
            {
                UpdateStateTextBlock.Text = "Up to date";
                return;
            }

            UpdateStateTextBlock.Text = $"Downloading {update.TargetFullRelease.Version}...";
            await manager.DownloadUpdatesAsync(update, progress =>
            {
                DispatcherQueue.TryEnqueue(() =>
                {
                    UpdateStateTextBlock.Text = $"Downloading {progress}%";
                });
            });

            UpdateStateTextBlock.Text = $"Ready to install {update.TargetFullRelease.Version}";
            MessageTextBlock.Text = $"Update {update.TargetFullRelease.Version} downloaded";
        }
        catch (Exception ex)
        {
            UpdateStateTextBlock.Text = "Update check skipped";
            MessageTextBlock.Text = ex.Message;
        }
        finally
        {
            _checkingForUpdates = false;
        }
    }

    private static string ResolveUpdateSource()
    {
        var baseDirectory = AppContext.BaseDirectory;
        var updateUrl = ReadTrimmedText(Path.Combine(baseDirectory, "update-url.txt"));
        if (!string.IsNullOrWhiteSpace(updateUrl))
        {
            var slash = updateUrl.LastIndexOf('/');
            if (slash >= 0)
            {
                return updateUrl[..(slash + 1)];
            }
        }

        var installerUrl = ReadTrimmedText(Path.Combine(baseDirectory, "installer-url.txt"));
        if (!string.IsNullOrWhiteSpace(installerUrl))
        {
            var slash = installerUrl.LastIndexOf('/');
            if (slash >= 0)
            {
                return installerUrl[..(slash + 1)];
            }
        }

        return string.Empty;
    }

    private static string ReadTrimmedText(string path)
    {
        if (!File.Exists(path))
        {
            return string.Empty;
        }

        return File.ReadAllText(path).Trim();
    }
}

internal static partial class NativeMethods
{
    private const string DllName = "TechArchiveSimpleScreenRecorder.Core.dll";

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport(DllName, EntryPoint = "TasrShowOverlay", CallingConvention = CallingConvention.StdCall)]
    private static extern bool TasrShowOverlayNative();

    [DllImport(DllName, EntryPoint = "TasrHideOverlay", CallingConvention = CallingConvention.StdCall)]
    private static extern bool TasrHideOverlayNative();

    [DllImport(DllName, EntryPoint = "TasrSetFullRegion", CallingConvention = CallingConvention.StdCall)]
    private static extern bool TasrSetFullRegionNative();

    [DllImport(DllName, EntryPoint = "TasrGetRegion", CallingConvention = CallingConvention.StdCall)]
    private static extern bool TasrGetRegionNative(out NativeRect region);

    [DllImport(DllName, EntryPoint = "TasrStartRecording", CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
    private static extern bool TasrStartRecordingNative(string outputPath, ref NativeRect region, StringBuilder errorBuffer, int errorBufferCount);

    [DllImport(DllName, EntryPoint = "TasrStopRecording", CallingConvention = CallingConvention.StdCall)]
    private static extern void TasrStopRecordingNative();

    [DllImport(DllName, EntryPoint = "TasrIsRecording", CallingConvention = CallingConvention.StdCall)]
    private static extern bool TasrIsRecordingNative();

    [DllImport(DllName, EntryPoint = "TasrGetElapsedSeconds", CallingConvention = CallingConvention.StdCall)]
    private static extern long TasrGetElapsedSecondsNative();

    [DllImport(DllName, EntryPoint = "TasrConsumeLastError", CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
    private static extern int TasrConsumeLastErrorNative(StringBuilder buffer, int bufferCount);

    internal static bool TasrShowOverlay() => TasrShowOverlayNative();
    internal static bool TasrHideOverlay() => TasrHideOverlayNative();
    internal static bool TasrSetFullRegion() => TasrSetFullRegionNative();
    internal static bool TasrGetRegion(out NativeRect region) => TasrGetRegionNative(out region);
    internal static void TasrStopRecording() => TasrStopRecordingNative();
    internal static bool TasrIsRecording() => TasrIsRecordingNative();
    internal static long TasrGetElapsedSeconds() => TasrGetElapsedSecondsNative();

    internal static bool TasrStartRecording(string outputPath, NativeRect region, out string error)
    {
        var buffer = new StringBuilder(1024);
        var started = TasrStartRecordingNative(outputPath, ref region, buffer, buffer.Capacity);
        error = buffer.ToString();
        return started;
    }

    internal static string ConsumeLastError()
    {
        var buffer = new StringBuilder(1024);
        TasrConsumeLastErrorNative(buffer, buffer.Capacity);
        return buffer.ToString();
    }
}
