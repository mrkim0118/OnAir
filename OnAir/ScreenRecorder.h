#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace onair {

// The container is chosen by Media Foundation from the output file extension, so it must match.
enum class OutputFormat {
    Mp4,  // .mp4: H.264 video + AAC audio
    Wmv,  // .wmv: WMV9 video + WMA audio
};

struct RecordingOptions {
    std::wstring outputPath;
    RECT region{};
    OutputFormat format = OutputFormat::Mp4;
    UINT32 frameRate = 15;
    UINT32 bitRate = 8000000;
    bool captureAudio = true;
};

class ScreenRecorder {
public:
    ScreenRecorder() = default;
    ~ScreenRecorder();

    bool Start(const RecordingOptions& options, std::wstring& error);
    void Stop();

    bool IsRecording() const noexcept;
    std::chrono::seconds Elapsed() const;
    std::wstring ConsumeLastError();

private:
    void WorkerMain(RecordingOptions options);
    bool RecordLoop(const RecordingOptions& options, std::wstring& error);
    void StoreLastError(const std::wstring& error);

    std::atomic_bool stopRequested_{false};
    std::atomic_bool recording_{false};
    std::thread worker_;
    std::chrono::steady_clock::time_point startedAt_{};

    mutable std::mutex mutex_;
    std::wstring lastError_;
};

}  // namespace onair
