// XCam Virtual Microphone -- a DirectShow audio push source.
//
// The camera's counterpart, and deliberately simpler. A camera has to negotiate
// a resolution, a frame rate and a pixel format, and every application asks for
// a different combination. A microphone has one format worth offering -- the
// 48kHz stereo 16-bit PCM the phone already sends -- so there is nothing to
// negotiate and nothing to convert.
//
// The one thing this must do that the camera does not is keep talking. A video
// source that goes quiet for a second shows a stale picture; an audio source
// that goes quiet stalls the graph, and applications treat a capture device that
// stops delivering as broken. So the push loop always produces buffers on
// schedule, filling with silence whatever the producer did not supply.

#include <windows.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>

// No initguid.h here for the same reason as the camera: the project's GUIDs are
// defined in xcam_guids.cpp, and defining them here would turn every standard
// GUID in uuids.h into a definition that collides with strmiids.lib.
#include "core/shared_audio.h"
#include "dsfilter/xcam_audio_filter.h"
#include "dsfilter/xcam_guids.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

using namespace xcam;

namespace {

std::atomic<long> g_audioLockCount{0};

constexpr REFERENCE_TIME kOneSecond = 10'000'000;

// Twenty milliseconds a buffer. Short enough that the delay this adds is below
// what anyone notices, long enough that the graph is not woken fifty times more
// often than it needs to be.
constexpr uint32_t kBufferMs = 20;
constexpr uint32_t kBytesPerSecond = kAudioSampleRate * kAudioBytesPerFrame;
constexpr uint32_t kBufferBytes = kBytesPerSecond * kBufferMs / 1000;

void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
    if (mt.pUnk) mt.pUnk->Release();
    mt.pbFormat = nullptr;
    mt.cbFormat = 0;
    mt.pUnk = nullptr;
}

void DeleteMediaType(AM_MEDIA_TYPE* mt) {
    if (!mt) return;
    FreeMediaType(*mt);
    CoTaskMemFree(mt);
}

// The only media type this device offers.
AM_MEDIA_TYPE* CreateMediaType() {
    auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!mt) return nullptr;
    std::memset(mt, 0, sizeof(*mt));

    auto* wfx = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!wfx) {
        CoTaskMemFree(mt);
        return nullptr;
    }
    std::memset(wfx, 0, sizeof(*wfx));
    wfx->wFormatTag = WAVE_FORMAT_PCM;
    wfx->nChannels = static_cast<WORD>(kAudioChannels);
    wfx->nSamplesPerSec = kAudioSampleRate;
    wfx->wBitsPerSample = static_cast<WORD>(kAudioBitsPerSample);
    wfx->nBlockAlign = static_cast<WORD>(kAudioBytesPerFrame);
    wfx->nAvgBytesPerSec = kBytesPerSecond;
    wfx->cbSize = 0;

    mt->majortype = MEDIATYPE_Audio;
    mt->subtype = MEDIASUBTYPE_PCM;
    mt->bFixedSizeSamples = TRUE;
    mt->bTemporalCompression = FALSE;
    mt->lSampleSize = kAudioBytesPerFrame;
    mt->formattype = FORMAT_WaveFormatEx;
    mt->cbFormat = sizeof(WAVEFORMATEX);
    mt->pbFormat = reinterpret_cast<BYTE*>(wfx);
    return mt;
}

bool WriteRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* name,
                         const wchar_t* value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const LONG result = RegSetValueExW(
        key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
        static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------

class AudioFilter;

class AudioPin final : public IPin, public IAMStreamConfig, public IKsPropertySet {
public:
    explicit AudioPin(AudioFilter* filter);
    ~AudioPin();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP Connect(IPin* receive, const AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* info) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* dir) override;
    STDMETHODIMP QueryId(LPWSTR* id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** en) override;
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return S_OK; }
    STDMETHODIMP BeginFlush() override { return S_OK; }
    STDMETHODIMP EndFlush() override { return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* mt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** mt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) override;
    STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** mt, BYTE* scc) override;

    STDMETHODIMP Set(REFGUID, DWORD, void*, DWORD, void*, DWORD) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP Get(REFGUID set, DWORD id, void*, DWORD, void* data,
                     DWORD dataLength, DWORD* returned) override;
    STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD* support) override;

    HRESULT Active();
    HRESULT Inactive();

private:
    static DWORD WINAPI ThreadEntry(void* self);
    void PushLoop();

    AudioFilter* filter_;                // weak; the filter owns this pin
    IPin* connected_ = nullptr;
    IMemInputPin* memInput_ = nullptr;
    IMemAllocator* allocator_ = nullptr;

    AM_MEDIA_TYPE currentType_{};

    HANDLE thread_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    std::atomic<bool> running_{false};

    SharedAudioReader reader_;

    std::atomic<long> refCount_{1};
};

class AudioFilter final : public IBaseFilter {
public:
    AudioFilter();
    ~AudioFilter();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetClassID(CLSID* clsid) override;

    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME start) override;
    STDMETHODIMP GetState(DWORD timeout, FILTER_STATE* state) override;
    STDMETHODIMP SetSyncSource(IReferenceClock* clock) override;
    STDMETHODIMP GetSyncSource(IReferenceClock** clock) override;

    STDMETHODIMP EnumPins(IEnumPins** en) override;
    STDMETHODIMP FindPin(LPCWSTR id, IPin** pin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override;
    STDMETHODIMP QueryVendorInfo(LPWSTR* vendor) override;

    AudioPin* Pin() { return pin_; }
    FILTER_STATE State() const { return state_; }

private:
    AudioPin* pin_;
    IFilterGraph* graph_ = nullptr;      // weak, per the DirectShow contract
    IReferenceClock* clock_ = nullptr;
    FILTER_STATE state_ = State_Stopped;
    WCHAR name_[MAX_FILTER_NAME] = XCAM_AUDIO_FILTER_NAME;
    std::atomic<long> refCount_{1};
};

// ---- enumerators -----------------------------------------------------------

class AudioPinEnumerator final : public IEnumPins {
public:
    explicit AudioPinEnumerator(IPin* pin) : pin_(pin) { pin_->AddRef(); }
    ~AudioPinEnumerator() { pin_->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }
    STDMETHODIMP_(ULONG) Release() override {
        const long n = --refCount_;
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP Next(ULONG count, IPin** pins, ULONG* fetched) override {
        if (!pins) return E_POINTER;
        ULONG n = 0;
        if (count > 0 && index_ == 0) {
            pins[0] = pin_;
            pin_->AddRef();
            ++index_;
            n = 1;
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override {
        index_ += count;
        return index_ > 1 ? S_FALSE : S_OK;
    }
    STDMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumPins** en) override {
        if (!en) return E_POINTER;
        auto* copy = new AudioPinEnumerator(pin_);
        copy->index_ = index_;
        *en = copy;
        return S_OK;
    }

private:
    IPin* pin_;
    ULONG index_ = 0;
    std::atomic<long> refCount_{1};
};

class AudioTypeEnumerator final : public IEnumMediaTypes {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refCount_; }
    STDMETHODIMP_(ULONG) Release() override {
        const long n = --refCount_;
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP Next(ULONG count, AM_MEDIA_TYPE** types, ULONG* fetched) override {
        if (!types) return E_POINTER;
        ULONG n = 0;
        if (count > 0 && index_ == 0) {
            types[0] = CreateMediaType();
            if (!types[0]) return E_OUTOFMEMORY;
            ++index_;
            n = 1;
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override {
        index_ += count;
        return index_ > 1 ? S_FALSE : S_OK;
    }
    STDMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumMediaTypes** en) override {
        if (!en) return E_POINTER;
        auto* copy = new AudioTypeEnumerator();
        copy->index_ = index_;
        *en = copy;
        return S_OK;
    }

private:
    ULONG index_ = 0;
    std::atomic<long> refCount_{1};
};

// ---- pin -------------------------------------------------------------------

AudioPin::AudioPin(AudioFilter* filter) : filter_(filter) {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

AudioPin::~AudioPin() {
    Inactive();
    FreeMediaType(currentType_);
    if (stopEvent_) CloseHandle(stopEvent_);
}

STDMETHODIMP AudioPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
    } else if (riid == IID_IAMStreamConfig) {
        *ppv = static_cast<IAMStreamConfig*>(this);
    } else if (riid == IID_IKsPropertySet) {
        *ppv = static_cast<IKsPropertySet*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) AudioPin::AddRef() { return ++refCount_; }

STDMETHODIMP_(ULONG) AudioPin::Release() {
    const long n = --refCount_;
    if (n == 0) delete this;
    return n;
}

STDMETHODIMP AudioPin::QueryAccept(const AM_MEDIA_TYPE* mt) {
    if (!mt) return E_POINTER;
    if (mt->majortype != MEDIATYPE_Audio) return S_FALSE;
    if (mt->subtype != MEDIASUBTYPE_PCM) return S_FALSE;
    if (mt->formattype != FORMAT_WaveFormatEx || !mt->pbFormat) return S_FALSE;
    if (mt->cbFormat < sizeof(WAVEFORMATEX)) return S_FALSE;

    const auto* wfx = reinterpret_cast<const WAVEFORMATEX*>(mt->pbFormat);
    // One format, and no conversion in the middle of a live path: a resampler
    // here would add latency and drift to hide a mismatch that never happens,
    // since the phone is configured to produce exactly this.
    return (wfx->nSamplesPerSec == kAudioSampleRate &&
            wfx->nChannels == kAudioChannels &&
            wfx->wBitsPerSample == kAudioBitsPerSample) ? S_OK : S_FALSE;
}

STDMETHODIMP AudioPin::EnumMediaTypes(IEnumMediaTypes** en) {
    if (!en) return E_POINTER;
    *en = new AudioTypeEnumerator();
    return S_OK;
}

STDMETHODIMP AudioPin::Connect(IPin* receive, const AM_MEDIA_TYPE* mt) {
    if (!receive) return E_POINTER;
    if (connected_) return VFW_E_ALREADY_CONNECTED;
    if (filter_->State() != State_Stopped) return VFW_E_NOT_STOPPED;

    if (mt && mt->majortype != GUID_NULL && QueryAccept(mt) != S_OK) {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    AM_MEDIA_TYPE* chosen = CreateMediaType();
    if (!chosen) return E_OUTOFMEMORY;
    if (receive->QueryAccept(chosen) != S_OK) {
        DeleteMediaType(chosen);
        return VFW_E_NO_ACCEPTABLE_TYPES;
    }

    HRESULT hr = receive->ReceiveConnection(static_cast<IPin*>(this), chosen);
    if (FAILED(hr)) {
        DeleteMediaType(chosen);
        return hr;
    }

    hr = receive->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&memInput_));
    if (FAILED(hr)) {
        receive->Disconnect();
        DeleteMediaType(chosen);
        return hr;
    }

    FreeMediaType(currentType_);
    currentType_ = *chosen;
    CoTaskMemFree(chosen);          // the contents moved into currentType_

    hr = memInput_->GetAllocator(&allocator_);
    if (FAILED(hr) || !allocator_) {
        hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IMemAllocator, reinterpret_cast<void**>(&allocator_));
        if (FAILED(hr)) {
            memInput_->Release();
            memInput_ = nullptr;
            receive->Disconnect();
            return hr;
        }
    }

    // More buffers than the camera asks for. Audio buffers are tiny and go out
    // fifty times a second, so a downstream filter holding one or two must not
    // be able to stall the loop.
    ALLOCATOR_PROPERTIES request{};
    request.cBuffers = 8;
    request.cbBuffer = static_cast<long>(kBufferBytes);
    request.cbAlign = 1;

    ALLOCATOR_PROPERTIES actual{};
    hr = allocator_->SetProperties(&request, &actual);
    if (SUCCEEDED(hr)) hr = memInput_->NotifyAllocator(allocator_, FALSE);
    if (FAILED(hr)) {
        allocator_->Release();
        allocator_ = nullptr;
        memInput_->Release();
        memInput_ = nullptr;
        receive->Disconnect();
        return hr;
    }

    connected_ = receive;
    connected_->AddRef();
    return S_OK;
}

STDMETHODIMP AudioPin::Disconnect() {
    if (filter_->State() != State_Stopped) return VFW_E_NOT_STOPPED;
    Inactive();

    if (allocator_) { allocator_->Release(); allocator_ = nullptr; }
    if (memInput_) { memInput_->Release(); memInput_ = nullptr; }
    if (connected_) { connected_->Release(); connected_ = nullptr; }
    return S_OK;
}

STDMETHODIMP AudioPin::ConnectedTo(IPin** pin) {
    if (!pin) return E_POINTER;
    *pin = connected_;
    if (!connected_) return VFW_E_NOT_CONNECTED;
    connected_->AddRef();
    return S_OK;
}

STDMETHODIMP AudioPin::ConnectionMediaType(AM_MEDIA_TYPE* mt) {
    if (!mt) return E_POINTER;
    if (!connected_) {
        std::memset(mt, 0, sizeof(*mt));
        return VFW_E_NOT_CONNECTED;
    }
    *mt = currentType_;
    if (mt->cbFormat) {
        mt->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(mt->cbFormat));
        if (!mt->pbFormat) return E_OUTOFMEMORY;
        std::memcpy(mt->pbFormat, currentType_.pbFormat, mt->cbFormat);
    }
    if (mt->pUnk) mt->pUnk->AddRef();
    return S_OK;
}

STDMETHODIMP AudioPin::QueryPinInfo(PIN_INFO* info) {
    if (!info) return E_POINTER;
    info->pFilter = filter_;
    filter_->AddRef();
    info->dir = PINDIR_OUTPUT;
    wcscpy_s(info->achName, L"Capture");
    return S_OK;
}

STDMETHODIMP AudioPin::QueryDirection(PIN_DIRECTION* dir) {
    if (!dir) return E_POINTER;
    *dir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP AudioPin::QueryId(LPWSTR* id) {
    if (!id) return E_POINTER;
    const wchar_t name[] = L"Capture";
    *id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(name)));
    if (!*id) return E_OUTOFMEMORY;
    std::memcpy(*id, name, sizeof(name));
    return S_OK;
}

// ---- IAMStreamConfig -------------------------------------------------------

STDMETHODIMP AudioPin::SetFormat(AM_MEDIA_TYPE* mt) {
    // There is one format. Accepting it is polite; accepting anything else
    // would be a promise this device cannot keep.
    return QueryAccept(mt) == S_OK ? S_OK : VFW_E_INVALIDMEDIATYPE;
}

STDMETHODIMP AudioPin::GetFormat(AM_MEDIA_TYPE** mt) {
    if (!mt) return E_POINTER;
    *mt = CreateMediaType();
    return *mt ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP AudioPin::GetNumberOfCapabilities(int* count, int* size) {
    if (!count || !size) return E_POINTER;
    *count = 1;
    *size = sizeof(AUDIO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP AudioPin::GetStreamCaps(int index, AM_MEDIA_TYPE** mt, BYTE* scc) {
    if (!mt || !scc) return E_POINTER;
    if (index != 0) return S_FALSE;

    *mt = CreateMediaType();
    if (!*mt) return E_OUTOFMEMORY;

    auto* caps = reinterpret_cast<AUDIO_STREAM_CONFIG_CAPS*>(scc);
    std::memset(caps, 0, sizeof(*caps));
    caps->guid = FORMAT_WaveFormatEx;
    caps->MinimumChannels = caps->MaximumChannels = kAudioChannels;
    caps->ChannelsGranularity = 1;
    caps->MinimumBitsPerSample = caps->MaximumBitsPerSample = kAudioBitsPerSample;
    caps->BitsPerSampleGranularity = 1;
    caps->MinimumSampleFrequency = caps->MaximumSampleFrequency = kAudioSampleRate;
    caps->SampleFrequencyGranularity = 1;
    return S_OK;
}

// ---- IKsPropertySet --------------------------------------------------------

STDMETHODIMP AudioPin::Get(REFGUID set, DWORD id, void*, DWORD, void* data,
                           DWORD dataLength, DWORD* returned) {
    if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (returned) *returned = sizeof(GUID);
    if (!data) return S_OK;
    if (dataLength < sizeof(GUID)) return E_UNEXPECTED;

    // Declaring the pin a capture category is what makes an application list
    // this as a recording device rather than as a generic source filter.
    *static_cast<GUID*>(data) = PIN_CATEGORY_CAPTURE;
    return S_OK;
}

STDMETHODIMP AudioPin::QuerySupported(REFGUID set, DWORD id, DWORD* support) {
    if (set != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (id != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (support) *support = KSPROPERTY_SUPPORT_GET;
    return S_OK;
}

// ---- streaming -------------------------------------------------------------

HRESULT AudioPin::Active() {
    if (!connected_ || running_.load()) return S_OK;
    if (!allocator_) return VFW_E_NO_ALLOCATOR;

    HRESULT hr = allocator_->Commit();
    if (FAILED(hr)) return hr;

    ResetEvent(stopEvent_);
    running_ = true;
    thread_ = CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr);
    if (!thread_) {
        running_ = false;
        return E_FAIL;
    }
    return S_OK;
}

HRESULT AudioPin::Inactive() {
    if (!running_.exchange(false)) return S_OK;

    SetEvent(stopEvent_);
    if (thread_) {
        WaitForSingleObject(thread_, 3000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (allocator_) allocator_->Decommit();
    return S_OK;
}

DWORD WINAPI AudioPin::ThreadEntry(void* self) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static_cast<AudioPin*>(self)->PushLoop();
    CoUninitialize();
    return 0;
}

void AudioPin::PushLoop() {
    const REFERENCE_TIME bufferDuration = kOneSecond * kBufferMs / 1000;
    REFERENCE_TIME nextStart = 0;
    bool firstSample = true;

    // Paced against a real clock rather than against the ring.
    //
    // Every buffer claims 20ms of the timeline whether or not the producer had
    // 20ms of audio to fill it, so a loop that ran as fast as the ring allowed
    // would hand downstream more timeline than time had actually passed --
    // measured at 1.44x, which is six seconds of audio delivered in four. A
    // live capture device produces exactly real time; the ring decides what is
    // in the buffers, not how often they go out.
    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    uint64_t bufferIndex = 0;

    while (running_.load()) {
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;

        IMediaSample* sample = nullptr;
        HRESULT hr = allocator_->GetBuffer(&sample, nullptr, nullptr, 0);
        if (FAILED(hr) || !sample) {
            Sleep(2);
            continue;
        }

        BYTE* dst = nullptr;
        if (FAILED(sample->GetPointer(&dst)) || !dst) {
            sample->Release();
            continue;
        }

        if (!reader_.IsAttached()) reader_.Attach();

        size_t got = 0;
        if (reader_.IsAttached() && reader_.ProducerAlive()) {
            // Waiting a little longer than one buffer lets the producer supply a
            // whole one; beyond that, going out short-but-on-time beats going
            // out complete-but-late.
            got = reader_.Read(dst, kBufferBytes, kBufferMs + 5);
        }

        if (got < kBufferBytes) {
            // Silence for the remainder. An audio capture device that simply
            // stops delivering looks broken to the application, and to a
            // listener a gap of silence is what a gap sounds like anyway.
            std::memset(dst + got, 0, kBufferBytes - got);
        }

        // Hold this buffer until its slot in real time comes round.
        ++bufferIndex;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const int64_t dueTicks = start.QuadPart +
            static_cast<int64_t>(bufferIndex * kBufferMs * frequency.QuadPart / 1000);
        if (dueTicks > now.QuadPart) {
            const int64_t waitMs = (dueTicks - now.QuadPart) * 1000 / frequency.QuadPart;
            if (waitMs > 0) Sleep(static_cast<DWORD>(waitMs));
        }

        sample->SetActualDataLength(static_cast<long>(kBufferBytes));
        sample->SetSyncPoint(TRUE);
        sample->SetDiscontinuity(firstSample || reader_.TakeDiscontinuity());
        firstSample = false;

        // Not `start`: that name belongs to the pacing origin above, and two
        // clocks under one name in one loop is how a paced stream ends up
        // stamped with the wrong times.
        REFERENCE_TIME sampleStart = nextStart;
        REFERENCE_TIME sampleEnd = sampleStart + bufferDuration;
        sample->SetTime(&sampleStart, &sampleEnd);
        nextStart = sampleEnd;

        hr = memInput_->Receive(sample);
        sample->Release();

        if (FAILED(hr)) break;
    }
}

// ---- filter ----------------------------------------------------------------

AudioFilter::AudioFilter() {
    pin_ = new AudioPin(this);
    ++g_audioLockCount;
}

AudioFilter::~AudioFilter() {
    if (pin_) {
        pin_->Release();
        pin_ = nullptr;
    }
    if (clock_) clock_->Release();
    --g_audioLockCount;
}

STDMETHODIMP AudioFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter ||
        riid == IID_IBaseFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) AudioFilter::AddRef() { return ++refCount_; }

STDMETHODIMP_(ULONG) AudioFilter::Release() {
    const long n = --refCount_;
    if (n == 0) delete this;
    return n;
}

STDMETHODIMP AudioFilter::GetClassID(CLSID* clsid) {
    if (!clsid) return E_POINTER;
    *clsid = CLSID_XCamVirtualMicrophone;
    return S_OK;
}

STDMETHODIMP AudioFilter::Stop() {
    state_ = State_Stopped;
    if (pin_) pin_->Inactive();
    return S_OK;
}

STDMETHODIMP AudioFilter::Pause() {
    if (state_ == State_Stopped && pin_) {
        const HRESULT hr = pin_->Active();
        if (FAILED(hr)) return hr;
    }
    state_ = State_Paused;
    return S_OK;
}

STDMETHODIMP AudioFilter::Run(REFERENCE_TIME) {
    if (state_ == State_Stopped) {
        const HRESULT hr = Pause();
        if (FAILED(hr)) return hr;
    }
    state_ = State_Running;
    return S_OK;
}

STDMETHODIMP AudioFilter::GetState(DWORD, FILTER_STATE* state) {
    if (!state) return E_POINTER;
    *state = state_;
    return S_OK;
}

STDMETHODIMP AudioFilter::SetSyncSource(IReferenceClock* clock) {
    if (clock_) clock_->Release();
    clock_ = clock;
    if (clock_) clock_->AddRef();
    return S_OK;
}

STDMETHODIMP AudioFilter::GetSyncSource(IReferenceClock** clock) {
    if (!clock) return E_POINTER;
    *clock = clock_;
    if (clock_) clock_->AddRef();
    return S_OK;
}

STDMETHODIMP AudioFilter::EnumPins(IEnumPins** en) {
    if (!en) return E_POINTER;
    *en = new AudioPinEnumerator(static_cast<IPin*>(pin_));
    return S_OK;
}

STDMETHODIMP AudioFilter::FindPin(LPCWSTR id, IPin** pin) {
    if (!pin) return E_POINTER;
    if (id && wcscmp(id, L"Capture") == 0) {
        *pin = static_cast<IPin*>(pin_);
        pin_->AddRef();
        return S_OK;
    }
    *pin = nullptr;
    return VFW_E_NOT_FOUND;
}

STDMETHODIMP AudioFilter::QueryFilterInfo(FILTER_INFO* info) {
    if (!info) return E_POINTER;
    wcscpy_s(info->achName, name_);
    info->pGraph = graph_;
    if (graph_) graph_->AddRef();
    return S_OK;
}

STDMETHODIMP AudioFilter::JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) {
    graph_ = graph;                     // weak by contract; the graph outlives us
    if (name) wcscpy_s(name_, name);
    return S_OK;
}

STDMETHODIMP AudioFilter::QueryVendorInfo(LPWSTR* vendor) {
    if (!vendor) return E_POINTER;
    *vendor = nullptr;
    return E_NOTIMPL;
}

}  // namespace

// ---- the interface the DLL entry points use --------------------------------

namespace xcam::audiofilter {

HRESULT CreateInstance(REFIID riid, void** ppv) {
    auto* filter = new AudioFilter();
    const HRESULT hr = filter->QueryInterface(riid, ppv);
    filter->Release();
    return hr;
}

bool InUse() { return g_audioLockCount.load() != 0; }

HRESULT Register(const wchar_t* modulePath) {
    if (!modulePath) return E_POINTER;

    std::wstring clsidKey = L"CLSID\\" XCAM_AUDIO_CLSID_STRING;
    if (!WriteRegistryString(HKEY_CLASSES_ROOT, clsidKey.c_str(), nullptr,
                             XCAM_AUDIO_FILTER_NAME)) {
        clsidKey = L"Software\\Classes\\CLSID\\" XCAM_AUDIO_CLSID_STRING;
        if (!WriteRegistryString(HKEY_CURRENT_USER, clsidKey.c_str(), nullptr,
                                 XCAM_AUDIO_FILTER_NAME)) {
            return E_ACCESSDENIED;
        }
        const std::wstring inproc = clsidKey + L"\\InprocServer32";
        WriteRegistryString(HKEY_CURRENT_USER, inproc.c_str(), nullptr, modulePath);
        WriteRegistryString(HKEY_CURRENT_USER, inproc.c_str(), L"ThreadingModel", L"Both");
    } else {
        const std::wstring inproc = clsidKey + L"\\InprocServer32";
        WriteRegistryString(HKEY_CLASSES_ROOT, inproc.c_str(), nullptr, modulePath);
        WriteRegistryString(HKEY_CLASSES_ROOT, inproc.c_str(), L"ThreadingModel", L"Both");
    }

    IFilterMapper2* mapper = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFilterMapper2, reinterpret_cast<void**>(&mapper));
    if (SUCCEEDED(hr)) {
        REGPINTYPES pinType{};
        pinType.clsMajorType = &MEDIATYPE_Audio;
        pinType.clsMinorType = &MEDIASUBTYPE_PCM;

        REGFILTERPINS pins{};
        pins.strName = const_cast<LPWSTR>(L"Capture");
        pins.bRendered = FALSE;
        pins.bOutput = TRUE;
        pins.bZero = FALSE;
        pins.bMany = FALSE;
        pins.clsConnectsToFilter = &CLSID_NULL;
        pins.strConnectsToPin = nullptr;
        pins.nMediaTypes = 1;
        pins.lpMediaType = &pinType;

        REGFILTER2 filter{};
        filter.dwVersion = 1;
        filter.dwMerit = MERIT_DO_NOT_USE + 1;   // selectable, never auto-connected
        filter.cPins = 1;
        filter.rgPins = &pins;

        IMoniker* moniker = nullptr;
        hr = mapper->RegisterFilter(CLSID_XCamVirtualMicrophone, XCAM_AUDIO_FILTER_NAME,
                                    &moniker, &CLSID_AudioInputDeviceCategory, nullptr,
                                    &filter);
        if (moniker) moniker->Release();
        mapper->Release();
        if (SUCCEEDED(hr)) return S_OK;
    }

    // Per-user fallback, exactly as for the camera: IFilterMapper2 writes
    // machine-wide keys that need elevation, and HKCR is a merged view, so the
    // device enumerator finds these too.
    const std::wstring instance =
        L"Software\\Classes\\CLSID\\{33D9A762-90C8-11D0-BD43-00A0C911CE86}"
        L"\\Instance\\" XCAM_AUDIO_CLSID_STRING;

    if (!WriteRegistryString(HKEY_CURRENT_USER, instance.c_str(), L"FriendlyName",
                             XCAM_AUDIO_FILTER_NAME) ||
        !WriteRegistryString(HKEY_CURRENT_USER, instance.c_str(), L"CLSID",
                             XCAM_AUDIO_CLSID_STRING)) {
        return E_ACCESSDENIED;
    }
    return S_OK;
}

HRESULT Unregister() {
    IFilterMapper2* mapper = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IFilterMapper2, reinterpret_cast<void**>(&mapper)))) {
        mapper->UnregisterFilter(&CLSID_AudioInputDeviceCategory, nullptr,
                                 CLSID_XCamVirtualMicrophone);
        mapper->Release();
    }

    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\" XCAM_AUDIO_CLSID_STRING);
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   L"Software\\Classes\\CLSID\\" XCAM_AUDIO_CLSID_STRING);
    return S_OK;
}

}  // namespace xcam::audiofilter
