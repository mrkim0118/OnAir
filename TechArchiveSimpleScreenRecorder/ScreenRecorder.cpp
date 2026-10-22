#include "ScreenRecorder.h"

#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <ksmedia.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "Mmdevapi.lib")

namespace tasr {
namespace {

template <typename T>
void SafeRelease(T*& value) {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

template <typename T>
void SafeCoTaskMemFree(T*& value) {
    if (value != nullptr) {
        CoTaskMemFree(value);
        value = nullptr;
    }
}

std::wstring HResultMessage(const wchar_t* prefix, HRESULT hr) {
    wchar_t buffer[128] = {};
    wsprintfW(buffer, L"%s (0x%08X)", prefix, static_cast<unsigned int>(hr));
    return buffer;
}

enum class AudioSampleKind {
    IntPcm,
    Float32,
    Unsupported,
};

struct AudioFormatInfo {
    AudioSampleKind kind = AudioSampleKind::Unsupported;
    UINT32 sampleRate = 0;
    WORD inputChannels = 0;
    WORD outputChannels = 0;
    WORD bitsPerSample = 0;
    WORD validBitsPerSample = 0;
    WORD bytesPerInputSample = 0;
    WORD inputBlockAlign = 0;
    WORD outputBlockAlign = 0;
};

struct AudioCaptureContext {
    IMMDeviceEnumerator* deviceEnumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    AudioFormatInfo format = {};
    UINT64 framesWritten = 0;
};

bool TryDescribeAudioFormat(const WAVEFORMATEX* format, AudioFormatInfo& info) {
    if (format == nullptr) {
        return false;
    }

    info.sampleRate = format->nSamplesPerSec;
    info.inputChannels = format->nChannels;
    info.outputChannels = static_cast<WORD>(format->nChannels > 1 ? 2 : 1);
    info.bitsPerSample = format->wBitsPerSample;
    info.validBitsPerSample = format->wBitsPerSample;
    info.bytesPerInputSample = static_cast<WORD>(format->wBitsPerSample / 8);
    info.inputBlockAlign = format->nBlockAlign;
    info.outputBlockAlign = static_cast<WORD>(info.outputChannels * sizeof(int16_t));

    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        info.kind = AudioSampleKind::IntPcm;
        return info.bytesPerInputSample > 0;
    }

    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) {
        info.kind = AudioSampleKind::Float32;
        return true;
    }

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        info.validBitsPerSample = extensible->Samples.wValidBitsPerSample != 0 ? extensible->Samples.wValidBitsPerSample : format->wBitsPerSample;
        if (IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            info.kind = AudioSampleKind::IntPcm;
            return info.bytesPerInputSample > 0;
        }
        if (IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && format->wBitsPerSample == 32) {
            info.kind = AudioSampleKind::Float32;
            return true;
        }
    }

    return false;
}

float ClampAudioSample(float value) {
    if (value > 1.0f) {
        return 1.0f;
    }
    if (value < -1.0f) {
        return -1.0f;
    }
    return value;
}

int32_t ReadSignedIntegerSample(const BYTE* source, WORD bytesPerSample) {
    switch (bytesPerSample) {
    case 1:
        return (static_cast<int32_t>(source[0]) - 128) << 8;
    case 2:
        return *reinterpret_cast<const int16_t*>(source);
    case 3: {
        int32_t value = static_cast<int32_t>(source[0]) |
                        (static_cast<int32_t>(source[1]) << 8) |
                        (static_cast<int32_t>(source[2]) << 16);
        if ((value & 0x00800000) != 0) {
            value |= 0xFF000000;
        }
        return value;
    }
    case 4:
        return *reinterpret_cast<const int32_t*>(source);
    default:
        return 0;
    }
}

float ReadInputSample(const BYTE* frame, WORD channelIndex, const AudioFormatInfo& format) {
    const BYTE* source = frame + (channelIndex * format.bytesPerInputSample);

    if (format.kind == AudioSampleKind::Float32) {
        return ClampAudioSample(*reinterpret_cast<const float*>(source));
    }

    if (format.kind == AudioSampleKind::IntPcm) {
        const int32_t signedValue = ReadSignedIntegerSample(source, format.bytesPerInputSample);
        const int scaleBits = std::max<int>(1, format.validBitsPerSample - 1);
        const float denominator = static_cast<float>(1u << std::min(scaleBits, 30));
        return ClampAudioSample(static_cast<float>(signedValue) / denominator);
    }

    return 0.0f;
}

int16_t FloatToPcm16(float sample) {
    const float clamped = ClampAudioSample(sample);
    if (clamped >= 0.0f) {
        return static_cast<int16_t>(std::lround(clamped * 32767.0f));
    }
    return static_cast<int16_t>(std::lround(clamped * 32768.0f));
}

void ConvertAudioPacketToPcm16(const BYTE* source, UINT32 frameCount, const AudioFormatInfo& format, std::vector<BYTE>& output) {
    output.resize(static_cast<size_t>(frameCount) * format.outputBlockAlign);
    auto* destination = reinterpret_cast<int16_t*>(output.data());

    for (UINT32 frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        const BYTE* inputFrame = source + (static_cast<size_t>(frameIndex) * format.inputBlockAlign);

        if (format.outputChannels == 1) {
            destination[frameIndex] = FloatToPcm16(ReadInputSample(inputFrame, 0, format));
            continue;
        }

        const float left = ReadInputSample(inputFrame, 0, format);
        const float right = ReadInputSample(inputFrame, format.inputChannels > 1 ? 1 : 0, format);
        destination[(static_cast<size_t>(frameIndex) * 2)] = FloatToPcm16(left);
        destination[(static_cast<size_t>(frameIndex) * 2) + 1] = FloatToPcm16(right);
    }
}

bool InitializeAudioCapture(AudioCaptureContext& audio, std::wstring& error) {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&audio.deviceEnumerator));
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not create the audio device enumerator", hr);
        return false;
    }

    hr = audio.deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &audio.device);
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not open the default playback device", hr);
        return false;
    }

    hr = audio.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio.audioClient));
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not activate the audio client", hr);
        return false;
    }

    hr = audio.audioClient->GetMixFormat(&audio.mixFormat);
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not read the audio mix format", hr);
        return false;
    }

    if (!TryDescribeAudioFormat(audio.mixFormat, audio.format)) {
        error = L"The current audio output format is not supported.";
        return false;
    }

    constexpr REFERENCE_TIME kAudioBufferDuration = 1'000'000;
    hr = audio.audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        kAudioBufferDuration,
        0,
        audio.mixFormat,
        nullptr);
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not initialize loopback audio capture", hr);
        return false;
    }

    hr = audio.audioClient->GetService(IID_PPV_ARGS(&audio.captureClient));
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not create the audio capture service", hr);
        return false;
    }

    return true;
}

void ReleaseAudioCapture(AudioCaptureContext& audio) {
    if (audio.audioClient != nullptr) {
        audio.audioClient->Stop();
    }
    SafeCoTaskMemFree(audio.mixFormat);
    SafeRelease(audio.captureClient);
    SafeRelease(audio.audioClient);
    SafeRelease(audio.device);
    SafeRelease(audio.deviceEnumerator);
    audio = {};
}

bool AddAudioStream(
    IMFSinkWriter* writer,
    const AudioFormatInfo& format,
    DWORD& streamIndex,
    std::wstring& error) {
    IMFMediaType* outputType = nullptr;
    IMFMediaType* inputType = nullptr;

    const auto cleanup = [&]() {
        SafeRelease(inputType);
        SafeRelease(outputType);
    };

    HRESULT hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not create the audio output type", hr);
        cleanup();
        return false;
    }

    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, format.outputChannels);
    outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, format.sampleRate);
    outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24'000);
    outputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    outputType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);

    hr = writer->AddStream(outputType, &streamIndex);
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not add the audio output stream", hr);
        cleanup();
        return false;
    }

    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not create the audio input type", hr);
        cleanup();
        return false;
    }

    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, format.outputChannels);
    inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, format.sampleRate);
    inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, format.outputBlockAlign);
    inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, format.sampleRate * format.outputBlockAlign);
    inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

    hr = writer->SetInputMediaType(streamIndex, inputType, nullptr);
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not set the audio input type", hr);
        cleanup();
        return false;
    }

    cleanup();
    return true;
}

bool WriteAudioPacket(
    IMFSinkWriter* writer,
    DWORD streamIndex,
    AudioCaptureContext& audio,
    const BYTE* packetData,
    UINT32 frameCount,
    bool isSilent,
    std::vector<BYTE>& convertedBuffer,
    std::wstring& error) {
    if (frameCount == 0) {
        return true;
    }

    if (isSilent) {
        convertedBuffer.assign(static_cast<size_t>(frameCount) * audio.format.outputBlockAlign, 0);
    } else {
        ConvertAudioPacketToPcm16(packetData, frameCount, audio.format, convertedBuffer);
    }

    IMFMediaBuffer* buffer = nullptr;
    IMFSample* sample = nullptr;
    BYTE* destination = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;

    const auto cleanup = [&]() {
        if (buffer != nullptr && destination != nullptr) {
            buffer->Unlock();
        }
        SafeRelease(sample);
        SafeRelease(buffer);
    };

    const DWORD bufferSize = static_cast<DWORD>(convertedBuffer.size());
    HRESULT hr = MFCreateMemoryBuffer(bufferSize, &buffer);
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not allocate the audio sample buffer", hr);
        cleanup();
        return false;
    }

    hr = buffer->Lock(&destination, &maxLength, &currentLength);
    if (FAILED(hr) || destination == nullptr || maxLength < bufferSize) {
        error = HResultMessage(L"Could not lock the audio sample buffer", hr);
        cleanup();
        return false;
    }

    CopyMemory(destination, convertedBuffer.data(), bufferSize);
    buffer->Unlock();
    destination = nullptr;
    buffer->SetCurrentLength(bufferSize);

    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not create the audio sample", hr);
        cleanup();
        return false;
    }

    hr = sample->AddBuffer(buffer);
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not attach the audio sample buffer", hr);
        cleanup();
        return false;
    }

    const LONGLONG sampleTime = static_cast<LONGLONG>((audio.framesWritten * 10'000'000ULL) / audio.format.sampleRate);
    const LONGLONG sampleDuration = static_cast<LONGLONG>((static_cast<UINT64>(frameCount) * 10'000'000ULL) / audio.format.sampleRate);
    sample->SetSampleTime(sampleTime);
    sample->SetSampleDuration(sampleDuration);

    hr = writer->WriteSample(streamIndex, sample);
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not write the audio sample", hr);
        cleanup();
        return false;
    }

    audio.framesWritten += frameCount;
    cleanup();
    return true;
}

bool DrainAudioPackets(
    IMFSinkWriter* writer,
    DWORD streamIndex,
    AudioCaptureContext& audio,
    std::vector<BYTE>& convertedBuffer,
    std::wstring& error) {
    while (true) {
        UINT32 packetLength = 0;
        HRESULT hr = audio.captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) {
            error = HResultMessage(L"Could not read the next audio packet size", hr);
            return false;
        }

        if (packetLength == 0) {
            return true;
        }

        BYTE* packetData = nullptr;
        UINT32 frameCount = 0;
        DWORD flags = 0;
        hr = audio.captureClient->GetBuffer(&packetData, &frameCount, &flags, nullptr, nullptr);
        if (FAILED(hr)) {
            error = HResultMessage(L"Could not read the audio packet", hr);
            return false;
        }

        const bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const bool success = WriteAudioPacket(writer, streamIndex, audio, packetData, frameCount, isSilent, convertedBuffer, error);
        audio.captureClient->ReleaseBuffer(frameCount);
        if (!success) {
            return false;
        }
    }
}

}  // namespace

ScreenRecorder::~ScreenRecorder() {
    Stop();
}

bool ScreenRecorder::Start(const RecordingOptions& options, std::wstring& error) {
    if (IsRecording()) {
        error = L"Already recording.";
        return false;
    }

    Stop();
    {
        std::lock_guard lock(mutex_);
        lastError_.clear();
    }

    stopRequested_.store(false, std::memory_order_relaxed);
    recording_.store(true, std::memory_order_relaxed);
    startedAt_ = std::chrono::steady_clock::now();
    worker_ = std::thread([this, options]() { WorkerMain(options); });
    return true;
}

void ScreenRecorder::Stop() {
    stopRequested_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) {
        worker_.join();
    }
    recording_.store(false, std::memory_order_relaxed);
}

bool ScreenRecorder::IsRecording() const noexcept {
    return recording_.load(std::memory_order_relaxed);
}

std::chrono::seconds ScreenRecorder::Elapsed() const {
    if (!IsRecording()) {
        return std::chrono::seconds(0);
    }
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startedAt_);
}

std::wstring ScreenRecorder::ConsumeLastError() {
    std::lock_guard lock(mutex_);
    std::wstring error = lastError_;
    lastError_.clear();
    return error;
}

void ScreenRecorder::WorkerMain(RecordingOptions options) {
    std::wstring error;
    const bool success = RecordLoop(options, error);
    if (!success && !stopRequested_.load(std::memory_order_relaxed)) {
        StoreLastError(error.empty() ? std::wstring(L"Recording failed.") : error);
    }
    recording_.store(false, std::memory_order_relaxed);
}

void ScreenRecorder::StoreLastError(const std::wstring& error) {
    std::lock_guard lock(mutex_);
    lastError_ = error;
}

bool ScreenRecorder::RecordLoop(const RecordingOptions& options, std::wstring& error) {
    const int width = options.region.right - options.region.left;
    const int height = options.region.bottom - options.region.top;
    if (width <= 1 || height <= 1) {
        error = L"Invalid recording size.";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path outputPath(options.outputPath);
    if (outputPath.has_parent_path()) {
        std::filesystem::create_directories(outputPath.parent_path(), ec);
        if (ec) {
            error = L"Could not create the output folder.";
            return false;
        }
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitializeCom = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        error = HResultMessage(L"COM initialization failed", comResult);
        return false;
    }

    const HRESULT startupResult = MFStartup(MF_VERSION);
    if (FAILED(startupResult)) {
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
        error = HResultMessage(L"Media Foundation startup failed", startupResult);
        return false;
    }

    HDC screenDc = nullptr;
    HDC memoryDc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    IMFSinkWriter* writer = nullptr;
    IMFMediaType* outputType = nullptr;
    IMFMediaType* inputType = nullptr;
    DWORD videoStreamIndex = 0;
    DWORD audioStreamIndex = 0;
    bool audioStarted = false;
    AudioCaptureContext audio = {};
    std::vector<BYTE> audioScratch;

    const auto cleanup = [&]() {
        if (audioStarted && audio.audioClient != nullptr) {
            audio.audioClient->Stop();
        }
        ReleaseAudioCapture(audio);
        SafeRelease(inputType);
        SafeRelease(outputType);
        SafeRelease(writer);
        if (oldBitmap != nullptr && memoryDc != nullptr) {
            SelectObject(memoryDc, oldBitmap);
        }
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        if (memoryDc != nullptr) {
            DeleteDC(memoryDc);
        }
        if (screenDc != nullptr) {
            ReleaseDC(nullptr, screenDc);
        }
        MFShutdown();
        if (shouldUninitializeCom) {
            CoUninitialize();
        }
    };

    if (!InitializeAudioCapture(audio, error)) {
        cleanup();
        return false;
    }

    screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        cleanup();
        error = L"Could not access the desktop.";
        return false;
    }

    memoryDc = CreateCompatibleDC(screenDc);
    if (memoryDc == nullptr) {
        cleanup();
        error = L"Could not create a capture device context.";
        return false;
    }

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bitmapBits = nullptr;
    bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bitmapBits, nullptr, 0);
    if (bitmap == nullptr || bitmapBits == nullptr) {
        cleanup();
        error = L"Could not allocate the capture buffer.";
        return false;
    }

    oldBitmap = SelectObject(memoryDc, bitmap);

    HRESULT hr = MFCreateSinkWriterFromURL(options.outputPath.c_str(), nullptr, nullptr, &writer);
    if (FAILED(hr)) {
        cleanup();
        error = HResultMessage(L"Could not create the video file", hr);
        return false;
    }

    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) {
        cleanup();
        error = HResultMessage(L"Could not create the output media type", hr);
        return false;
    }

    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outputType->SetUINT32(MF_MT_AVG_BITRATE, options.bitRate);
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outputType, MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
    MFSetAttributeRatio(outputType, MF_MT_FRAME_RATE, options.frameRate, 1);
    MFSetAttributeRatio(outputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = writer->AddStream(outputType, &videoStreamIndex);
    if (FAILED(hr)) {
        cleanup();
        error = HResultMessage(L"Could not add the video output stream", hr);
        return false;
    }

    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) {
        cleanup();
        error = HResultMessage(L"Could not create the input media type", hr);
        return false;
    }

    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inputType, MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
    MFSetAttributeRatio(inputType, MF_MT_FRAME_RATE, options.frameRate, 1);
    MFSetAttributeRatio(inputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = writer->SetInputMediaType(videoStreamIndex, inputType, nullptr);
    if (FAILED(hr)) {
        cleanup();
        error = HResultMessage(L"Could not set the video input media type", hr);
        return false;
    }

    if (!AddAudioStream(writer, audio.format, audioStreamIndex, error)) {
        cleanup();
        return false;
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        cleanup();
        error = HResultMessage(L"Could not start the encoder", hr);
        return false;
    }

    hr = audio.audioClient->Start();
    if (FAILED(hr)) {
        cleanup();
        error = HResultMessage(L"Could not start system audio capture", hr);
        return false;
    }
    audioStarted = true;

    const DWORD stride = static_cast<DWORD>(width * 4);
    const DWORD bufferSize = stride * static_cast<DWORD>(height);
    const LONGLONG frameDuration = 10'000'000LL / static_cast<LONGLONG>(options.frameRate);
    const auto sleepStep = std::chrono::nanoseconds(1'000'000'000LL / static_cast<long long>(options.frameRate));
    auto nextFrame = std::chrono::steady_clock::now();
    LONGLONG frameTime = 0;

    while (!stopRequested_.load(std::memory_order_relaxed)) {
        if (!DrainAudioPackets(writer, audioStreamIndex, audio, audioScratch, error)) {
            cleanup();
            return false;
        }

        if (!BitBlt(
                memoryDc,
                0,
                0,
                width,
                height,
                screenDc,
                options.region.left,
                options.region.top,
                SRCCOPY | CAPTUREBLT)) {
            cleanup();
            error = L"Could not capture the screen.";
            return false;
        }

        IMFMediaBuffer* buffer = nullptr;
        IMFSample* sample = nullptr;

        hr = MFCreateMemoryBuffer(bufferSize, &buffer);
        if (FAILED(hr)) {
            cleanup();
            error = HResultMessage(L"Could not allocate a video sample buffer", hr);
            return false;
        }

        BYTE* destination = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        hr = buffer->Lock(&destination, &maxLength, &currentLength);
        if (FAILED(hr) || destination == nullptr || maxLength < bufferSize) {
            SafeRelease(buffer);
            cleanup();
            error = HResultMessage(L"Could not lock the video sample buffer", hr);
            return false;
        }

        const auto* source = static_cast<const BYTE*>(bitmapBits);
        for (int row = 0; row < height; ++row) {
            const DWORD sourceOffset = static_cast<DWORD>((height - 1 - row) * width * 4);
            const DWORD destinationOffset = static_cast<DWORD>(row * width * 4);
            CopyMemory(destination + destinationOffset, source + sourceOffset, stride);
        }
        buffer->Unlock();
        buffer->SetCurrentLength(bufferSize);

        hr = MFCreateSample(&sample);
        if (FAILED(hr)) {
            SafeRelease(buffer);
            cleanup();
            error = HResultMessage(L"Could not create a video sample", hr);
            return false;
        }

        sample->AddBuffer(buffer);
        sample->SetSampleTime(frameTime);
        sample->SetSampleDuration(frameDuration);
        hr = writer->WriteSample(videoStreamIndex, sample);

        SafeRelease(sample);
        SafeRelease(buffer);

        if (FAILED(hr)) {
            cleanup();
            error = HResultMessage(L"Could not write the video sample", hr);
            return false;
        }

        frameTime += frameDuration;
        nextFrame += sleepStep;
        std::this_thread::sleep_until(nextFrame);
    }

    if (!DrainAudioPackets(writer, audioStreamIndex, audio, audioScratch, error)) {
        cleanup();
        return false;
    }

    hr = writer->Finalize();
    cleanup();
    if (FAILED(hr)) {
        error = HResultMessage(L"Could not finalize the recording file", hr);
        return false;
    }

    return true;
}

}  // namespace tasr
