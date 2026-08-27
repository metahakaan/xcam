#pragma once

#include "core/protocol.h"

#include <cstdint>
#include <string>

namespace xcam {

// Blocking TCP client for one phone. Reads run on whichever thread calls
// ReadPacket; sends are guarded so the UI thread can issue control commands
// while a reader thread is parked in recv.
class NetClient {
public:
    NetClient();
    ~NetClient();

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    bool Connect(const std::string& host, uint16_t port, int timeoutMs = 5000);
    void Disconnect();
    bool IsConnected() const { return socket_ != ~0ull; }

    // Reads the handshake that the phone sends immediately on accept. Must be
    // called once, before any packet read.
    //
    // A phone that is allowing Wi-Fi connections answers a network client with
    // a challenge instead -- it will not name its cameras, or itself, to
    // something that has not said the code. That case returns false with
    // NeedsPairing() set; send the code with SendPairing and read again.
    bool ReadHandshake(DeviceInfo& out);

    // Set when the last ReadHandshake was answered with a challenge rather than
    // a description. Distinguishes "type your code in" from a real failure,
    // which matters because they need opposite things from the person.
    bool NeedsPairing() const { return needsPairing_; }

    // Answers the challenge. The phone either goes on to send the real
    // handshake or closes the connection without saying which digit was wrong.
    bool SendPairing(const std::string& code);

    // Blocks until a whole packet has arrived. False means the connection is
    // gone or the stream desynchronised; either way the caller should reconnect.
    bool ReadPacket(Packet& out);

    bool SendControl(const std::string& json, uint32_t seq = 0);

    const std::string& LastError() const { return lastError_; }

private:
    bool ReadExact(void* dst, size_t bytes);
    void Fail(const std::string& what);

    uint64_t socket_;
    bool needsPairing_ = false;
    std::string lastError_;
    void* sendLock_;        // CRITICAL_SECTION, kept out of the header
};

}  // namespace xcam
