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

namespace OnAir.WinUI;

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
        NativeMethods.OnAirSetFullRegion();
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
        NativeMethods.OnAirHideOverlay();
        NativeMethods.OnAirStopRecording();
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
        var overlayVisible = NativeMethods.OnAirShowOverlay();
        MessageTextBlock.Text = overlayVisible ? "Drag the red frame to set the capture area" : "Could not show the capture frame";
        RefreshRegion();
    }

    private void FullButton_Click(object sender, RoutedEventArgs e)
    {
        NativeMethods.OnAirSetFullRegion();
        RefreshRegion();
        MessageTextBlock.Text = "Capture area set to the full screen";
    }

    private void RecordButton_Click(object sender, RoutedEventArgs e)
    {
        if (!NativeMethods.OnAirGetRegion(out var region))
        {
            MessageTextBlock.Text = "Could not read the capture area";
            return;
        }

        if (string.IsNullOrWhiteSpace(OutputPathTextBox.Text))
        {
            MessageTextBlock.Text = "Choose an output path first";
            return;
        }

        NativeMethods.OnAirHideOverlay();
        if (!NativeMethods.OnAirStartRecording(OutputPathTextBox.Text, region, out var error))
        {
            MessageTextBlock.Text = string.IsNullOrWhiteSpace(error) ? "Recording could not be started" : error;
            return;
        }

        MessageTextBlock.Text = "Recording started";
        RefreshState();
    }

    private void StopButton_Click(object sender, RoutedEventArgs e)
    {
        NativeMethods.OnAirStopRecording();
        MessageTextBlock.Text = "Recording stopped";
        RefreshState();
    }

    private void RefreshRegion()
    {
        if (!NativeMethods.OnAirGetRegion(out var region))
        {
            return;
        }

        var width = Math.Max(0, region.Right - region.Left);
        var height = Math.Max(0, region.Bottom - region.Top);
        RegionTextBlock.Text = $"{width} x {height}";
    }

    private void RefreshState()
    {
        var isRecording = NativeMethods.OnAirIsRecording();
        RecordButton.IsEnabled = !isRecording;
        StopButton.IsEnabled = isRecording;
        OutputPathTextBox.IsEnabled = !isRecording;
        BrowseButton.IsEnabled = !isRecording;
        FrameButton.IsEnabled = !isRecording;
        FullButton.IsEnabled = !isRecording;

        StatusTextBlock.Text = isRecording ? "REC" : "READY";
        StatusTextBlock.Foreground = new SolidColorBrush(isRecording ? Windows.UI.Color.FromArgb(255, 226, 61, 61) : Windows.UI.Color.FromArgb(255, 255, 255, 255));

        var elapsed = TimeSpan.FromSeconds(NativeMethods.OnAirGetElapsedSeconds());
        ElapsedTextBlock.Text = $"{elapsed.Minutes:00}:{elapsed.Seconds:00}";
    }

    private static string BuildDefaultOutputPath()
    {
        var baseFolder = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
        var targetFolder = Path.Combine(baseFolder, "OnAir");
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
    private const string DllName = "OnAir.Core.dll";

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport(DllName, EntryPoint = "OnAirShowOverlay", CallingConvention = CallingConvention.StdCall)]
    private static extern bool OnAirShowOverlayNative();

    [DllImport(DllName, EntryPoint = "OnAirHideOverlay", CallingConvention = CallingConvention.StdCall)]
    private static extern bool OnAirHideOverlayNative();

    [DllImport(DllName, EntryPoint = "OnAirSetFullRegion", CallingConvention = CallingConvention.StdCall)]
    private static extern bool OnAirSetFullRegionNative();

    [DllImport(DllName, EntryPoint = "OnAirGetRegion", CallingConvention = CallingConvention.StdCall)]
    private static extern bool OnAirGetRegionNative(out NativeRect region);

    [DllImport(DllName, EntryPoint = "OnAirStartRecording", CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
    private static extern bool OnAirStartRecordingNative(string outputPath, ref NativeRect region, StringBuilder errorBuffer, int errorBufferCount);

    [DllImport(DllName, EntryPoint = "OnAirStopRecording", CallingConvention = CallingConvention.StdCall)]
    private static extern void OnAirStopRecordingNative();

    [DllImport(DllName, EntryPoint = "OnAirIsRecording", CallingConvention = CallingConvention.StdCall)]
    private static extern bool OnAirIsRecordingNative();

    [DllImport(DllName, EntryPoint = "OnAirGetElapsedSeconds", CallingConvention = CallingConvention.StdCall)]
    private static extern long OnAirGetElapsedSecondsNative();

    [DllImport(DllName, EntryPoint = "OnAirConsumeLastError", CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
    private static extern int OnAirConsumeLastErrorNative(StringBuilder buffer, int bufferCount);

    internal static bool OnAirShowOverlay() => OnAirShowOverlayNative();
    internal static bool OnAirHideOverlay() => OnAirHideOverlayNative();
    internal static bool OnAirSetFullRegion() => OnAirSetFullRegionNative();
    internal static bool OnAirGetRegion(out NativeRect region) => OnAirGetRegionNative(out region);
    internal static void OnAirStopRecording() => OnAirStopRecordingNative();
    internal static bool OnAirIsRecording() => OnAirIsRecordingNative();
    internal static long OnAirGetElapsedSeconds() => OnAirGetElapsedSecondsNative();

    internal static bool OnAirStartRecording(string outputPath, NativeRect region, out string error)
    {
        var buffer = new StringBuilder(1024);
        var started = OnAirStartRecordingNative(outputPath, ref region, buffer, buffer.Capacity);
        error = buffer.ToString();
        return started;
    }

    internal static string ConsumeLastError()
    {
        var buffer = new StringBuilder(1024);
        OnAirConsumeLastErrorNative(buffer, buffer.Capacity);
        return buffer.ToString();
    }
}
