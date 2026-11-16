#include "ScreenRecorder.h"

#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <windows.h>
#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "Mmdevapi.lib")
#pragma comment(lib, "Mf.lib")

namespace onair {
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

    // 1 s of headroom: the capture loop drains once per video frame, and a slow frame must not overflow the buffer.
    constexpr REFERENCE_TIME kAudioBufferDuration = 10'000'000;
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

// The WMA encoder only accepts the exact output types it advertises; pick the best one matching the device.
HRESULT FindWmaOutputType(const AudioFormatInfo& format, IMFMediaType** result) {
    IMFCollection* types = nullptr;
    HRESULT hr = MFTranscodeGetAudioOutputAvailableTypes(MFAudioFormat_WMAudioV8, MFT_ENUM_FLAG_ALL, nullptr, &types);
    if (FAILED(hr)) {
        return hr;
    }

    DWORD count = 0;
    types->GetElementCount(&count);
    UINT32 bestBytesPerSecond = 0;
    for (DWORD index = 0; index < count; ++index) {
        IUnknown* element = nullptr;
        IMFMediaType* type = nullptr;
        if (SUCCEEDED(types->GetElement(index, &element)) && SUCCEEDED(element->QueryInterface(IID_PPV_ARGS(&type)))) {
            const UINT32 channels = MFGetAttributeUINT32(type, MF_MT_AUDIO_NUM_CHANNELS, 0);
            const UINT32 sampleRate = MFGetAttributeUINT32(type, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
            const UINT32 bytesPerSecond = MFGetAttributeUINT32(type, MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 0);
            if (channels == format.outputChannels && sampleRate == format.sampleRate && bytesPerSecond > bestBytesPerSecond) {
                SafeRelease(*result);
                *result = type;
                type->AddRef();
                bestBytesPerSecond = bytesPerSecond;
            }
        }
        SafeRelease(type);
        SafeRelease(element);
    }
    SafeRelease(types);
    return *result != nullptr ? S_OK : MF_E_INVALIDMEDIATYPE;
}

bool AddAudioStream(
    IMFSinkWriter* writer,
    OutputFormat container,
    const AudioFormatInfo& format,
    DWORD& streamIndex,
    std::wstring& error) {
    IMFMediaType* outputType = nullptr;
    IMFMediaType* inputType = nullptr;

    const auto cleanup = [&]() {
        SafeRelease(inputType);
        SafeRelease(outputType);
    };

    HRESULT hr = S_OK;
    if (container == OutputFormat::Wmv) {
        hr = FindWmaOutputType(format, &outputType);
        if (FAILED(hr)) {
            error = HResultMessage(L"No WMA audio format matches the playback device", hr);
            cleanup();
            return false;
        }
    } else {
        hr = MFCreateMediaType(&outputType);
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
    }

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
    UINT64 expectedFrames,
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
            break;
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

    // Loopback delivers no packets while nothing is playing; fill the gap with silence so audio stays in sync with video.
    const UINT64 tolerance = audio.format.sampleRate / 10;
    if (expectedFrames > audio.framesWritten + tolerance) {
        const auto gap = static_cast<UINT32>(std::min<UINT64>(expectedFrames - audio.framesWritten, audio.format.sampleRate * 10ULL));
        return WriteAudioPacket(writer, streamIndex, audio, nullptr, gap, true, convertedBuffer, error);
    }
    return true;
}

UINT64 ElapsedAudioFrames(std::chrono::steady_clock::time_point start, UINT32 sampleRate) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
    return static_cast<UINT64>(elapsed.count()) * sampleRate / 1'000'000ULL;
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
    // Report failures even after a stop request: a failed Finalize leaves an unplayable file.
    if (!success) {
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

    // The Media Foundation H.264 encoder rejects frames above 4096x2304 (e.g. "Full" across several monitors): scale down to fit.
    const double scale = std::min({ 1.0, 4096.0 / width, 2304.0 / height });
    const int outWidth = std::max(2, static_cast<int>(width * scale) & ~1);
    const int outHeight = std::max(2, static_cast<int>(height * scale) & ~1);
    const bool scaled = outWidth != width || outHeight != height;

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

    // No playback device (or an unsupported mix format) should not block recording: fall back to video only.
    std::wstring audioError;
    const bool hasAudio = options.captureAudio && InitializeAudioCapture(audio, audioError);
    if (!hasAudio) {
        ReleaseAudioCapture(audio);
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
    bitmapInfo.bmiHeader.biWidth = outWidth;
    bitmapInfo.bmiHeader.biHeight = -outHeight;
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
    if (scaled) {
        SetStretchBltMode(memoryDc, HALFTONE);
        SetBrushOrgEx(memoryDc, 0, 0, nullptr);
    }

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
    outputType->SetGUID(MF_MT_SUBTYPE, options.format == OutputFormat::Wmv ? MFVideoFormat_WMV3 : MFVideoFormat_H264);
    outputType->SetUINT32(MF_MT_AVG_BITRATE, options.bitRate);
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outputType, MF_MT_FRAME_SIZE, static_cast<UINT32>(outWidth), static_cast<UINT32>(outHeight));
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
    MFSetAttributeSize(inputType, MF_MT_FRAME_SIZE, static_cast<UINT32>(outWidth), static_cast<UINT32>(outHeight));
    MFSetAttributeRatio(inputType, MF_MT_FRAME_RATE, options.frameRate, 1);
    MFSetAttributeRatio(inputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = writer->SetInputMediaType(videoStreamIndex, inputType, nullptr);
    if (FAILED(hr)) {
        cleanup();
        error = HResultMessage(L"Could not set the video input media type", hr);
        return false;
    }

    if (hasAudio && !AddAudioStream(writer, options.format, audio.format, audioStreamIndex, error)) {
        cleanup();
        return false;
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        cleanup();
        error = HResultMessage(L"Could not start the encoder", hr);
        return false;
    }

    if (hasAudio) {
        hr = audio.audioClient->Start();
        if (FAILED(hr)) {
            cleanup();
            error = HResultMessage(L"Could not start system audio capture", hr);
            return false;
        }
        audioStarted = true;
    }

    const DWORD stride = static_cast<DWORD>(outWidth * 4);
    const DWORD bufferSize = stride * static_cast<DWORD>(outHeight);
    const LONGLONG frameDuration = 10'000'000LL / static_cast<LONGLONG>(options.frameRate);
    const auto sleepStep = std::chrono::nanoseconds(1'000'000'000LL / static_cast<long long>(options.frameRate));
    const auto captureStart = std::chrono::steady_clock::now();
    auto nextFrame = captureStart;

    while (!stopRequested_.load(std::memory_order_relaxed)) {
        if (hasAudio && !DrainAudioPackets(writer, audioStreamIndex, audio, ElapsedAudioFrames(captureStart, audio.format.sampleRate), audioScratch, error)) {
            cleanup();
            return false;
        }

        const BOOL captured = scaled
            ? StretchBlt(memoryDc, 0, 0, outWidth, outHeight, screenDc, options.region.left, options.region.top, width, height, SRCCOPY | CAPTUREBLT)
            : BitBlt(memoryDc, 0, 0, width, height, screenDc, options.region.left, options.region.top, SRCCOPY | CAPTUREBLT);
        if (!captured) {
            cleanup();
            error = L"Could not capture the screen.";
            return false;
        }
        // Timestamp by wall clock so slow frames do not make the video run faster than the audio.
        const LONGLONG frameTime = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - captureStart).count() / 100;

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
        for (int row = 0; row < outHeight; ++row) {
            const DWORD sourceOffset = static_cast<DWORD>(outHeight - 1 - row) * stride;
            const DWORD destinationOffset = static_cast<DWORD>(row) * stride;
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

        nextFrame += sleepStep;
        const auto now = std::chrono::steady_clock::now();
        if (nextFrame < now) {
            nextFrame = now;  // Behind schedule: drop frames instead of bursting to catch up.
        }
        std::this_thread::sleep_until(nextFrame);
    }

    if (hasAudio && !DrainAudioPackets(writer, audioStreamIndex, audio, ElapsedAudioFrames(captureStart, audio.format.sampleRate), audioScratch, error)) {
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

}  // namespace onair
