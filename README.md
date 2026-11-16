<p align="center">
  <img src="docs/icon.png" width="128" alt="OnAir icon">
</p>

<h1 align="center">OnAir</h1>

<p align="center">A compact, dark-themed screen recorder for Windows, written in native C++.</p>

<p align="center">Author: <b>Manggdy</b></p>

## Features

- **One-click recording**: a single Record / Stop button with a live timer.
- **Capture area frame**: drag the red frame by its handle to move it, or pull its edges to resize. Press `Esc` to hide it. **Full** captures every monitor at once.
- **System audio**: records what you hear (WASAPI loopback). The audio stays in sync with the video through silent passages.
- **Settings window** (gear button), saved automatically:

  | Option | Choices | Default |
  |---|---|---|
  | Format | MP4 (H.264 + AAC), WMV (WMV9 + WMA) | MP4 |
  | Quality | Low / Standard / High / Max | High |
  | Frame rate | 15 / 24 / 30 / 60 fps | 15 |
  | System audio | On / Off | On |

- **Multi-monitor aware**: the capture frame keeps its exact pixel size across monitors with different scaling. Areas larger than the encoder limit (4096×2304) are scaled down to fit.
- **Update check** (currently disabled): the code that checks for and installs newer versions is still included. Re-enable it by uncommenting `StartUpdateThread()` in `OnAir/MainWindow.cpp`.

Recordings are saved to `Videos\OnAir` by default. Settings are stored in `%LOCALAPPDATA%\OnAir\settings.ini`.

## Requirements

- Windows 10 or 11 (x64)
- To build: Visual Studio 2022 with the C++ desktop workload and a Windows 10/11 SDK

## Build

Open [OnAir.sln](OnAir.sln) in Visual Studio and build `Release|x64`, or run:

```powershell
msbuild OnAir.sln /p:Configuration=Release /p:Platform=x64
```

The output is `OnAir\build\Release\OnAir.exe`.

## Versioning

[`OnAir/latest-version.txt`](OnAir/latest-version.txt) is the single source of the version number. The build compiles it into the app, and the publish script uses it for the release.

## Publish Installer

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\Publish-Velopack.ps1
```

The script builds the app, prepares a publish folder, and creates a Velopack installer and update feed in `artifacts\velopack\Releases`. It requires the `vpk` CLI (`dotnet tool install -g vpk`).

## Project Layout

| Path | Contents |
|---|---|
| `OnAir/` | Native Win32 app (the shipped product) |
| `OnAir.WinUI/` | Experimental WinUI 3 front-end (not part of the solution build) |
| `scripts/` | Release packaging |
