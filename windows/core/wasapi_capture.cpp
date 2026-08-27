#include "core/wasapi_capture.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <cmath>
#include <thread>

namespace xcam {
namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

std::string Narrow(const wchar_t* wide) {
    if (!wide) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string FriendlyName(IMMDevice* device) {
    IPropertyStore* store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return {};

    PROPVARIANT name;
    PropVariantInit(&name);
    std::string out;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) &&
        name.vt == VT_LPWSTR) {
        out = Narrow(name.pwszVal);
    }
    PropVariantClear(&name);
    store->Release();
    return out;
}

}  // namespace

struct WasapiCapture::Impl {
    std::thread thread;
    std::atomic<bool> stop{false};

    // Peak since the last read, as an integer so the capture thread can raise it
    // without a lock and the reader can take it with one exchange.
    std::atomic<int> peak{0};
    std::atomic<uint32_t> silentBuffers{0};
};

WasapiCapture::WasapiCapture() : impl_(new Impl) {}

WasapiCapture::~WasapiCapture() {
    Stop();
    delete impl_;
}

std::vector<AudioInput> WasapiCapture::List() {
    std::vector<AudioInput> found;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        return found;
    }

    std::string defaultId;
    IMMDevice* preferred = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &preferred))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(preferred->GetId(&id))) {
            defaultId = Narrow(id);
            CoTaskMemFree(id);
        }
        preferred->Release();
    }

    IMMDeviceCollection* collection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE,
                                                 &collection))) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device))) continue;

            LPWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id))) {
                AudioInput input;
                input.id = Narrow(id);
                input.name = FriendlyName(device);
                input.isDefault = !defaultId.empty() && input.id == defaultId;
                if (!input.name.empty()) found.push_back(std::move(input));
                CoTaskMemFree(id);
            }
            device->Release();
        }
        collection->Release();
    }
    enumerator->Release();
    return found;
}

float WasapiCapture::TakePeak() {
    return impl_->peak.exchange(0) / 32767.0f;
}

uint32_t WasapiCapture::TakeSilentBuffers() {
    return impl_->silentBuffers.exchange(0);
}

bool WasapiCapture::Start(const std::string& id, const Callback& onSamples) {
    Stop();
    impl_->stop = false;

    // Opened on this thread so a failure is a return value rather than a log
    // line the caller never sees, then handed over.
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               IID_PPV_ARGS(&enumerator)))) {
        lastError_ = "no audio device enumerator";
        return false;
    }

    IMMDevice* device = nullptr;
    if (id.empty()) {
        enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    } else {
        const int n = MultiByteToWideChar(CP_UTF8, 0, id.data(),
                                          static_cast<int>(id.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, id.data(), static_cast<int>(id.size()),
                            wide.data(), n);
        if (FAILED(enumerator->GetDevice(wide.c_str(), &device))) {
            // A remembered device that has been unplugged is a normal thing to
            // find, and the default is a better answer than nothing.
            enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        }
    }
    enumerator->Release();

    if (!device) {
        lastError_ = "no microphone on this machine";
        return false;
    }
    deviceName_ = FriendlyName(device);

    device->Release();
    running_ = true;
    impl_->thread = std::thread(&WasapiCapture::Run, this, id, onSamples);
    lastError_.clear();
    return true;
}

void WasapiCapture::Stop() {
    if (!impl_->thread.joinable()) {
        running_ = false;
        return;
    }
    impl_->stop = true;
    impl_->thread.join();
    running_ = false;
}

void WasapiCapture::Run(std::string id, Callback callback) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* mix = nullptr;
    HANDLE ready = nullptr;

    auto cleanup = [&] {
        if (client) client->Stop();
        SafeRelease(capture);
        SafeRelease(client);
        SafeRelease(device);
        SafeRelease(enumerator);
        if (mix) CoTaskMemFree(mix);
        if (ready) CloseHandle(ready);
        CoUninitialize();
    };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        lastError_ = "no audio device enumerator";
        cleanup();
        return;
    }

    if (id.empty()) {
        enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    } else {
        const int n = MultiByteToWideChar(CP_UTF8, 0, id.data(),
                                          static_cast<int>(id.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, id.data(), static_cast<int>(id.size()),
                            wide.data(), n);
        if (FAILED(enumerator->GetDevice(wide.c_str(), &device))) {
            enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        }
    }

    if (!device ||
        FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&client))) ||
        FAILED(client->GetMixFormat(&mix))) {
        lastError_ = "could not open the microphone";
        cleanup();
        return;
    }

    sampleRate_ = mix->nSamplesPerSec;
    channels_ = mix->nChannels;

    ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Event driven rather than polled: the device decides when there is
    // something to take, and a poll loop would either burn a core or add its
    // own interval to the latency.
    constexpr REFERENCE_TIME kBuffer = 20 * 10000;      // 20 ms
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBuffer, 0, mix,
                                  nullptr)) ||
        FAILED(client->SetEventHandle(ready)) ||
        FAILED(client->GetService(IID_PPV_ARGS(&capture))) ||
        FAILED(client->Start())) {
        lastError_ = "could not start the microphone";
        cleanup();
        return;
    }

    const bool isFloat =
        mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat ==
             KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    std::vector<int16_t> scratch;

    while (!impl_->stop) {
        if (WaitForSingleObject(ready, 200) != WAIT_OBJECT_0) continue;

        UINT32 available = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&available)) && available > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 position = 0, qpc = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, &position, &qpc))) break;

            scratch.resize(static_cast<size_t>(frames) * channels_);
            const size_t samples = scratch.size();

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                impl_->silentBuffers.fetch_add(1);
                // The device says it has nothing; writing its buffer would be
                // reading whatever was left there.
                std::fill(scratch.begin(), scratch.end(), static_cast<int16_t>(0));
            } else if (isFloat) {
                const auto* in = reinterpret_cast<const float*>(data);
                for (size_t i = 0; i < samples; ++i) {
                    const float clamped = in[i] > 1.0f ? 1.0f : (in[i] < -1.0f ? -1.0f : in[i]);
                    scratch[i] = static_cast<int16_t>(clamped * 32767.0f);
                }
            } else {
                std::memcpy(scratch.data(), data, samples * sizeof(int16_t));
            }

            int loudest = 0;
            for (size_t i = 0; i < samples; ++i) {
                const int magnitude = scratch[i] < 0 ? -scratch[i] : scratch[i];
                if (magnitude > loudest) loudest = magnitude;
            }
            int previous = impl_->peak.load();
            while (loudest > previous &&
                   !impl_->peak.compare_exchange_weak(previous, loudest)) {
            }

            if (callback) callback(scratch.data(), frames, qpc);
            capture->ReleaseBuffer(frames);
        }
    }

    cleanup();
}

}  // namespace xcam
