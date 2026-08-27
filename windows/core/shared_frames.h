#pragma once

// The bridge between xcam-app and the DirectShow filter.
//
// The filter is a COM DLL that Windows loads *into the consuming application* --
// Zoom's process, OBS's, Chrome's. It cannot share anything with our app except
// through named kernel objects, which is why this exists and why it shapes the
// whole Windows design.
//
// One writer, many readers, no locks. A reader can be an unresponsive video
// conferencing app that stops calling us for two seconds; a writer that waited
// for it would stall the capture pipeline, so the writer never waits and never
// takes a lock a reader could be holding. Readers detect a torn frame after the
// fact and skip it rather than preventing one.

#include <cstdint>
#include <string>

namespace xcam {

inline constexpr uint32_t kSharedMagic = 0x314D4358;   // "XCM1"
inline constexpr uint32_t kSharedVersion = 1;

// Four is enough to absorb a reader that misses its slot without letting it fall
// so far behind that it shows something stale.
inline constexpr uint32_t kSlotCount = 4;

// The published format. NV12 because it is what the decoder produces natively,
// and it is half the bytes of anything RGB -- which matters when every frame
// crosses a process boundary.
enum class SharedFormat : uint32_t {
    Nv12 = 0,
};

struct SharedHeader {
    uint32_t magic;
    uint32_t version;

    uint32_t width;
    uint32_t height;
    uint32_t format;          // SharedFormat
    uint32_t stride;          // luma stride; chroma plane follows at stride*height

    uint32_t fpsNum;
    uint32_t fpsDen;

    uint32_t slotCount;
    uint32_t slotBytes;

    // Bumped by the writer once a slot is fully written. Readers take the
    // newest, so a slow reader skips ahead rather than replaying history.
    volatile uint64_t writeSeq;

    // Wall-clock of the most recent publish, so a reader can tell "no producer"
    // from "producer wedged" without a second channel.
    volatile uint64_t lastWriteTick;

    volatile uint32_t producerAlive;

    // Wall-clock of the most recent read, written by whoever is consuming
    // frames. The counterpart of lastWriteTick, and the only way this side can
    // know an application actually has the camera open: a filter that is merely
    // registered is not a filter anyone is looking through.
    //
    // Took `reserved`, which is why the section version does not move -- the
    // field was already in the layout and already zero.
    volatile uint64_t lastReadTick;

    volatile uint64_t slotSeq[kSlotCount];
    volatile uint64_t slotPtsUs[kSlotCount];
};

// Writer side, owned by xcam-app.
class SharedFrameWriter {
public:
    SharedFrameWriter();
    ~SharedFrameWriter();

    SharedFrameWriter(const SharedFrameWriter&) = delete;
    SharedFrameWriter& operator=(const SharedFrameWriter&) = delete;

    // Creates (or re-creates) the section for this format. Changing size means a
    // new mapping, since consumers size their buffers from the header.
    bool Open(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen);
    void Close();
    bool IsOpen() const { return header_ != nullptr; }

    // Copies one NV12 frame in and publishes it. `stride` is the source luma
    // stride; the chroma plane is expected at src + stride * height.
    bool Publish(const uint8_t* nv12, size_t stride, uint64_t ptsUs);

    uint32_t Width() const;
    uint32_t Height() const;

    // How long ago something last read a frame, in milliseconds, or -1 when
    // nothing ever has. This is the tally: an application that has the camera
    // open reads continuously, and one that has merely enumerated the device
    // does not read at all.
    int64_t MillisecondsSinceRead() const;

    const std::string& LastError() const { return lastError_; }

private:
    void* mapping_ = nullptr;         // HANDLE
    void* view_ = nullptr;
    void* newFrameEvent_ = nullptr;   // HANDLE
    SharedHeader* header_ = nullptr;
    uint8_t* slots_ = nullptr;
    uint64_t seq_ = 0;
    std::string lastError_;
};

// Reader side, living inside whatever process loaded the filter.
class SharedFrameReader {
public:
    SharedFrameReader();
    ~SharedFrameReader();

    SharedFrameReader(const SharedFrameReader&) = delete;
    SharedFrameReader& operator=(const SharedFrameReader&) = delete;

    // Attaching is allowed to fail: the app may not be running. Consumers are
    // expected to keep retrying and show a placeholder meanwhile.
    bool Attach();
    void Detach();
    bool IsAttached() const { return header_ != nullptr; }

    bool ProducerAlive() const;

    // Marks the section as being read right now. Called by a consumer once per
    // frame; costs one store.
    void NoteRead();

    uint32_t Width() const;
    uint32_t Height() const;
    uint32_t Stride() const;
    void FrameRate(uint32_t& num, uint32_t& den) const;

    // Copies the newest frame into `dst`, which must hold Stride()*Height()*3/2
    // bytes. Returns false when nothing new has arrived within `timeoutMs`.
    bool ReadLatest(uint8_t* dst, size_t capacity, uint64_t& ptsUs, uint32_t timeoutMs);

private:
    void* mapping_ = nullptr;
    void* view_ = nullptr;
    void* newFrameEvent_ = nullptr;
    SharedHeader* header_ = nullptr;
    uint8_t* slots_ = nullptr;
    uint64_t lastSeq_ = 0;

    // Whether the view can be written to. The heartbeat is the only write a
    // consumer makes, and it is optional.
    bool writable_ = false;
};

}  // namespace xcam
