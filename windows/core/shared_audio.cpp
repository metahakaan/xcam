#include "core/shared_audio.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace xcam {
namespace {

// Local\ rather than Global\, for the same reason as the video section: the
// filter runs as the same user in the same session, and Global\ would need a
// privilege for no benefit.
constexpr wchar_t kMappingName[] = L"Local\\XCamAudio";
constexpr wchar_t kEventName[] = L"Local\\XCamAudioData";

// A producer that has not published in this long is treated as gone. Shorter
// than the video timeout because audio arrives far more often: 200ms of silence
// from a live microphone already means something has stopped.
constexpr uint64_t kProducerTimeoutMs = 1000;

// How far behind the write head a fresh reader starts. Enough to absorb the
// jitter of a consumer that asks for buffers in bursts, small enough not to be
// heard as delay.
constexpr uint32_t kPrefillMs = 40;

std::atomic<uint64_t>* AsAtomic(volatile uint64_t* p) {
    return reinterpret_cast<std::atomic<uint64_t>*>(const_cast<uint64_t*>(p));
}

}  // namespace

// ---- writer ----------------------------------------------------------------

SharedAudioWriter::SharedAudioWriter() = default;

SharedAudioWriter::~SharedAudioWriter() { Close(); }

bool SharedAudioWriter::Open() {
    Close();

    const size_t total = sizeof(SharedAudioHeader) + kAudioRingBytes;

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        static_cast<DWORD>(total >> 32),
                                        static_cast<DWORD>(total & 0xFFFFFFFF),
                                        kMappingName);
    if (!mapping) {
        lastError_ = "CreateFileMapping failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, total);
    if (!view) {
        lastError_ = "MapViewOfFile failed (" + std::to_string(GetLastError()) + ")";
        CloseHandle(mapping);
        return false;
    }

    mapping_ = mapping;
    view_ = view;
    header_ = static_cast<SharedAudioHeader*>(view);
    ring_ = static_cast<uint8_t*>(view) + sizeof(SharedAudioHeader);

    // The section may already exist from a previous run of this app, in which
    // case it carries a stale write position that a reader would take for real
    // data. Rewriting the whole header is the only way to be sure.
    std::memset(header_, 0, sizeof(SharedAudioHeader));
    header_->magic = kSharedAudioMagic;
    header_->version = kSharedAudioVersion;
    header_->sampleRate = kAudioSampleRate;
    header_->channels = kAudioChannels;
    header_->bitsPerSample = kAudioBitsPerSample;
    header_->ringBytes = kAudioRingBytes;
    header_->producerAlive = 1;

    dataEvent_ = CreateEventW(nullptr, FALSE, FALSE, kEventName);
    return true;
}

void SharedAudioWriter::Close() {
    if (header_) header_->producerAlive = 0;
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (dataEvent_) CloseHandle(static_cast<HANDLE>(dataEvent_));
    view_ = nullptr;
    mapping_ = nullptr;
    dataEvent_ = nullptr;
    header_ = nullptr;
    ring_ = nullptr;
}

bool SharedAudioWriter::Publish(const uint8_t* pcm, size_t bytes) {
    if (!header_ || !pcm || bytes == 0) return false;

    // More than the ring in one go would wrap onto itself; keep the newest tail,
    // which is the only part a live consumer could still want.
    if (bytes > kAudioRingBytes) {
        pcm += bytes - kAudioRingBytes;
        bytes = kAudioRingBytes;
    }

    const uint64_t pos = AsAtomic(&header_->writePos)->load(std::memory_order_relaxed);
    const size_t offset = static_cast<size_t>(pos % kAudioRingBytes);
    const size_t firstPart = (std::min)(bytes, static_cast<size_t>(kAudioRingBytes - offset));

    std::memcpy(ring_ + offset, pcm, firstPart);
    if (firstPart < bytes) std::memcpy(ring_, pcm + firstPart, bytes - firstPart);

    // Release: the bytes above must be visible before the position that claims
    // them is.
    AsAtomic(&header_->writePos)->store(pos + bytes, std::memory_order_release);
    header_->lastWriteTick = GetTickCount64();
    header_->producerAlive = 1;

    if (dataEvent_) SetEvent(static_cast<HANDLE>(dataEvent_));
    return true;
}

// ---- reader ----------------------------------------------------------------

SharedAudioReader::SharedAudioReader() = default;

SharedAudioReader::~SharedAudioReader() { Detach(); }

bool SharedAudioReader::Attach() {
    Detach();

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName);
    if (!mapping) return false;

    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapping);
        return false;
    }

    auto* header = static_cast<SharedAudioHeader*>(view);
    if (header->magic != kSharedAudioMagic || header->version != kSharedAudioVersion ||
        header->ringBytes != kAudioRingBytes) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return false;
    }

    mapping_ = mapping;
    view_ = view;
    header_ = header;
    ring_ = static_cast<uint8_t*>(view) + sizeof(SharedAudioHeader);
    haveReadPos_ = false;
    discontinuity_ = false;

    dataEvent_ = OpenEventW(SYNCHRONIZE, FALSE, kEventName);
    return true;
}

void SharedAudioReader::Detach() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (dataEvent_) CloseHandle(static_cast<HANDLE>(dataEvent_));
    view_ = nullptr;
    mapping_ = nullptr;
    dataEvent_ = nullptr;
    header_ = nullptr;
    ring_ = nullptr;
}

bool SharedAudioReader::ProducerAlive() const {
    if (!header_) return false;
    if (!header_->producerAlive) return false;
    const uint64_t last = header_->lastWriteTick;
    return last != 0 && GetTickCount64() - last < kProducerTimeoutMs;
}

bool SharedAudioReader::TakeDiscontinuity() {
    const bool value = discontinuity_;
    discontinuity_ = false;
    return value;
}

size_t SharedAudioReader::Read(uint8_t* dst, size_t bytes, uint32_t timeoutMs) {
    if (!header_ || !dst || bytes == 0) return 0;

    uint64_t writePos = AsAtomic(&header_->writePos)->load(std::memory_order_acquire);

    if (!haveReadPos_) {
        // Start a little behind the write head rather than exactly on it, so a
        // consumer that asks for buffers in bursts does not underrun on its
        // first one.
        const uint64_t prefill =
            static_cast<uint64_t>(kAudioSampleRate) * kAudioBytesPerFrame * kPrefillMs / 1000;
        readPos_ = writePos > prefill ? writePos - prefill : 0;
        haveReadPos_ = true;
    }

    // Wait for enough to satisfy the request, but only for as long as the caller
    // allowed: returning short is better than returning late.
    const DWORD deadline = GetTickCount() + timeoutMs;
    while (writePos - readPos_ < bytes) {
        const DWORD now = GetTickCount();
        if (now >= deadline) break;
        if (!dataEvent_) {
            Sleep(1);
        } else if (WaitForSingleObject(static_cast<HANDLE>(dataEvent_),
                                       deadline - now) != WAIT_OBJECT_0) {
            break;
        }
        writePos = AsAtomic(&header_->writePos)->load(std::memory_order_acquire);
    }

    // Lapped: the producer has overwritten everything we had not read. There is
    // no way to recover the missing audio, so drop to a fresh position and say
    // so, rather than reading bytes that are half old and half new.
    if (writePos - readPos_ > kAudioRingBytes) {
        readPos_ = writePos > kAudioRingBytes / 2 ? writePos - kAudioRingBytes / 2 : 0;
        discontinuity_ = true;
    }

    const size_t available = static_cast<size_t>((std::min)(
        static_cast<uint64_t>(bytes), writePos - readPos_));
    if (available == 0) return 0;

    const size_t offset = static_cast<size_t>(readPos_ % kAudioRingBytes);
    const size_t firstPart = (std::min)(available, static_cast<size_t>(kAudioRingBytes - offset));
    std::memcpy(dst, ring_ + offset, firstPart);
    if (firstPart < available) std::memcpy(dst + firstPart, ring_, available - firstPart);

    readPos_ += available;
    return available;
}

}  // namespace xcam
