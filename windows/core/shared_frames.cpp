#include "core/shared_frames.h"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace xcam {
namespace {

// Local\ rather than Global\: the filter always runs as the same user in the
// same session as the app, and Global\ would need SeCreateGlobalPrivilege for
// no benefit.
constexpr wchar_t kMappingName[] = L"Local\\XCamFrames";
constexpr wchar_t kEventName[] = L"Local\\XCamNewFrame";

// A producer that has not published in this long is treated as gone, whether it
// crashed or is merely wedged. Either way the consumer should stop showing its
// last frame as if it were live.
constexpr uint64_t kProducerTimeoutMs = 2000;

size_t Nv12Bytes(uint32_t stride, uint32_t height) {
    return static_cast<size_t>(stride) * height * 3 / 2;
}

std::atomic<uint64_t>* AsAtomic(volatile uint64_t* p) {
    return reinterpret_cast<std::atomic<uint64_t>*>(const_cast<uint64_t*>(p));
}

}  // namespace

// ---- writer ----------------------------------------------------------------

SharedFrameWriter::SharedFrameWriter() = default;

SharedFrameWriter::~SharedFrameWriter() { Close(); }

bool SharedFrameWriter::Open(uint32_t width, uint32_t height,
                             uint32_t fpsNum, uint32_t fpsDen) {
    Close();

    if (width == 0 || height == 0) {
        lastError_ = "invalid frame size";
        return false;
    }

    // Luma stride is padded to 64 so the copies stay aligned; consumers read it
    // from the header rather than assuming width.
    const uint32_t stride = (width + 63) & ~63u;
    const size_t slotBytes = Nv12Bytes(stride, height);
    const size_t total = sizeof(SharedHeader) + slotBytes * kSlotCount;

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
    header_ = static_cast<SharedHeader*>(view);
    slots_ = static_cast<uint8_t*>(view) + sizeof(SharedHeader);

    // Zero first so a reader that attaches mid-initialisation sees a bad magic
    // rather than a half-built header.
    std::memset(header_, 0, sizeof(SharedHeader));

    header_->version = kSharedVersion;
    header_->width = width;
    header_->height = height;
    header_->format = static_cast<uint32_t>(SharedFormat::Nv12);
    header_->stride = stride;
    header_->fpsNum = fpsNum;
    header_->fpsDen = fpsDen;
    header_->slotCount = kSlotCount;
    header_->slotBytes = static_cast<uint32_t>(slotBytes);
    header_->producerAlive = 1;

    // Magic last: it is what tells a reader the rest is trustworthy.
    MemoryBarrier();
    header_->magic = kSharedMagic;

    newFrameEvent_ = CreateEventW(nullptr, FALSE, FALSE, kEventName);
    seq_ = 0;
    lastError_.clear();
    return true;
}

void SharedFrameWriter::Close() {
    if (header_) {
        header_->producerAlive = 0;
        header_->magic = 0;
        MemoryBarrier();

        // Wake anyone parked on the event so they notice the producer left
        // instead of waiting out their timeout.
        if (newFrameEvent_) SetEvent(static_cast<HANDLE>(newFrameEvent_));
    }
    if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
    if (mapping_) { CloseHandle(static_cast<HANDLE>(mapping_)); mapping_ = nullptr; }
    if (newFrameEvent_) { CloseHandle(static_cast<HANDLE>(newFrameEvent_)); newFrameEvent_ = nullptr; }
    header_ = nullptr;
    slots_ = nullptr;
}

bool SharedFrameWriter::Publish(const uint8_t* nv12, size_t stride, uint64_t ptsUs) {
    if (!header_ || !nv12) return false;

    const uint64_t seq = ++seq_;
    const uint32_t slot = static_cast<uint32_t>(seq % header_->slotCount);
    uint8_t* dst = slots_ + static_cast<size_t>(slot) * header_->slotBytes;

    const uint32_t dstStride = header_->stride;
    const uint32_t height = header_->height;

    if (stride == dstStride) {
        std::memcpy(dst, nv12, Nv12Bytes(dstStride, height));
    } else {
        for (uint32_t y = 0; y < height; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * dstStride,
                        nv12 + static_cast<size_t>(y) * stride, header_->width);
        }
        const uint8_t* srcChroma = nv12 + stride * height;
        uint8_t* dstChroma = dst + static_cast<size_t>(dstStride) * height;
        for (uint32_t y = 0; y < height / 2; ++y) {
            std::memcpy(dstChroma + static_cast<size_t>(y) * dstStride,
                        srcChroma + static_cast<size_t>(y) * stride, header_->width);
        }
    }

    // Publish in this order: slot contents, then the slot's sequence, then the
    // global one. A reader that sees writeSeq has necessarily seen a complete
    // slotSeq, and rechecks it afterwards to catch a slot recycled underneath it.
    MemoryBarrier();
    AsAtomic(&header_->slotSeq[slot])->store(seq, std::memory_order_release);
    AsAtomic(&header_->slotPtsUs[slot])->store(ptsUs, std::memory_order_relaxed);
    AsAtomic(&header_->writeSeq)->store(seq, std::memory_order_release);
    AsAtomic(&header_->lastWriteTick)->store(GetTickCount64(), std::memory_order_relaxed);

    if (newFrameEvent_) SetEvent(static_cast<HANDLE>(newFrameEvent_));
    return true;
}

uint32_t SharedFrameWriter::Width() const { return header_ ? header_->width : 0; }
uint32_t SharedFrameWriter::Height() const { return header_ ? header_->height : 0; }

int64_t SharedFrameWriter::MillisecondsSinceRead() const {
    if (!header_) return -1;
    const uint64_t last = AsAtomic(&header_->lastReadTick)->load(std::memory_order_relaxed);
    if (last == 0) return -1;

    // The tick counter can only go forwards, but the reader is in another
    // process and may have written its value a moment after this one was read.
    const uint64_t now = GetTickCount64();
    return now > last ? static_cast<int64_t>(now - last) : 0;
}

// ---- reader ----------------------------------------------------------------

SharedFrameReader::SharedFrameReader() = default;

SharedFrameReader::~SharedFrameReader() { Detach(); }

bool SharedFrameReader::Attach() {
    Detach();

    // Read/write if it can be had, read-only if not.
    //
    // A consumer has one thing to write -- the heartbeat that says something is
    // actually looking through this camera -- and a read-only view cannot carry
    // it. Falling back rather than failing: a mapping this process may only read
    // is still a mapping it can show pictures from, and losing the tally is a
    // smaller loss than losing the camera.
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kMappingName);
    bool writable = mapping != nullptr;
    if (!mapping) {
        mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kMappingName);
        if (!mapping) return false;
    }
    const DWORD access = writable ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ;

    // Map the header first: the full size is not known until it has been read.
    void* peek = MapViewOfFile(mapping, access, 0, 0, sizeof(SharedHeader));
    if (!peek) {
        CloseHandle(mapping);
        return false;
    }

    const SharedHeader* peekHeader = static_cast<const SharedHeader*>(peek);
    if (peekHeader->magic != kSharedMagic || peekHeader->version != kSharedVersion ||
        peekHeader->slotCount == 0 || peekHeader->slotCount > kSlotCount) {
        UnmapViewOfFile(peek);
        CloseHandle(mapping);
        return false;
    }

    const size_t total = sizeof(SharedHeader) +
                         static_cast<size_t>(peekHeader->slotBytes) * peekHeader->slotCount;
    UnmapViewOfFile(peek);

    void* view = MapViewOfFile(mapping, access, 0, 0, total);
    if (!view) {
        CloseHandle(mapping);
        return false;
    }
    writable_ = writable;

    mapping_ = mapping;
    view_ = view;
    header_ = static_cast<SharedHeader*>(view);
    slots_ = static_cast<uint8_t*>(view) + sizeof(SharedHeader);
    lastSeq_ = 0;

    newFrameEvent_ = OpenEventW(SYNCHRONIZE, FALSE, kEventName);
    return true;
}

void SharedFrameReader::Detach() {
    if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
    if (mapping_) { CloseHandle(static_cast<HANDLE>(mapping_)); mapping_ = nullptr; }
    if (newFrameEvent_) { CloseHandle(static_cast<HANDLE>(newFrameEvent_)); newFrameEvent_ = nullptr; }
    header_ = nullptr;
    writable_ = false;
    slots_ = nullptr;
}

bool SharedFrameReader::ProducerAlive() const {
    if (!header_ || header_->magic != kSharedMagic || !header_->producerAlive) return false;

    const uint64_t last = AsAtomic(&header_->lastWriteTick)->load(std::memory_order_relaxed);
    return last != 0 && GetTickCount64() - last < kProducerTimeoutMs;
}

void SharedFrameReader::NoteRead() {
    // A read-only view would fault on the store rather than ignore it.
    if (!header_ || !writable_) return;
    AsAtomic(&header_->lastReadTick)->store(GetTickCount64(), std::memory_order_relaxed);
}

uint32_t SharedFrameReader::Width() const { return header_ ? header_->width : 0; }
uint32_t SharedFrameReader::Height() const { return header_ ? header_->height : 0; }
uint32_t SharedFrameReader::Stride() const { return header_ ? header_->stride : 0; }

void SharedFrameReader::FrameRate(uint32_t& num, uint32_t& den) const {
    num = header_ && header_->fpsNum ? header_->fpsNum : 30;
    den = header_ && header_->fpsDen ? header_->fpsDen : 1;
}

bool SharedFrameReader::ReadLatest(uint8_t* dst, size_t capacity, uint64_t& ptsUs,
                                   uint32_t timeoutMs) {
    if (!header_ || !dst) return false;

    const size_t needed = Nv12Bytes(header_->stride, header_->height);
    if (capacity < needed) return false;

    const uint64_t deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        const uint64_t seq = AsAtomic(&header_->writeSeq)->load(std::memory_order_acquire);

        if (seq != 0 && seq != lastSeq_) {
            const uint32_t slot = static_cast<uint32_t>(seq % header_->slotCount);
            const uint8_t* src = slots_ + static_cast<size_t>(slot) * header_->slotBytes;

            std::memcpy(dst, src, needed);

            // If the writer recycled this slot while the copy was running, the
            // bytes are a mix of two frames. Cheaper to notice afterwards and
            // retry than to lock the writer out of a slot it needs.
            const uint64_t stillSeq =
                AsAtomic(&header_->slotSeq[slot])->load(std::memory_order_acquire);
            if (stillSeq == seq) {
                ptsUs = AsAtomic(&header_->slotPtsUs[slot])->load(std::memory_order_relaxed);
                lastSeq_ = seq;
                return true;
            }
            continue;      // torn; the newest frame is already waiting
        }

        const uint64_t now = GetTickCount64();
        if (now >= deadline) return false;

        if (newFrameEvent_) {
            WaitForSingleObject(static_cast<HANDLE>(newFrameEvent_),
                                static_cast<DWORD>(deadline - now));
        } else {
            Sleep(1);
        }
    }
}

}  // namespace xcam
