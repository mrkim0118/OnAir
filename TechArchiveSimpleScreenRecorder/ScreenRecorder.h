#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace tasr {

struct RecordingOptions {
    std::wstring outputPath;
    RECT region{};
    UINT32 frameRate = 15;
    UINT32 bitRate = 8000000;
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

}  // namespace tasr
