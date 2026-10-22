# Tech Archive Simple Screen Recorder

Compact Win32 screen recorder written in C++.

## Build

Open [TechArchiveSimpleScreenRecorder.sln](TechArchiveSimpleScreenRecorder.sln) in Visual Studio and build `Release|x64`.

## Publish Installer

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\Publish-Velopack.ps1
```

The script builds the native app, prepares a publish folder, and creates a Velopack installer/update feed.
