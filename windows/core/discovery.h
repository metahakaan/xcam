#pragma once

// Finds phones running XCam on the local network.
//
// A UDP broadcast beacon rather than mDNS. mDNS would be the conventional
// answer and is what a phone can already speak through NsdManager, but the
// Windows side of it means DnsServiceBrowse and a callback-driven resolver --
// a great deal of machinery to learn one address. A datagram on a fixed port,
// twice a second, costs forty bytes and needs nothing that is not already here.
//
// Only used when there is no cable and no address given. USB does not need
// finding, and an address typed by hand outranks anything discovered.

#include <cstdint>
#include <string>
#include <vector>

namespace xcam {

inline constexpr uint16_t kDiscoveryPort = 27184;

struct DiscoveredDevice {
    std::string address;      // dotted IPv4, ready for NetClient::Connect
    std::string name;         // as the phone reports itself
    uint16_t port = 0;
    uint16_t version = 0;
    uint64_t lastSeenTick = 0;
};

// Listens for beacons in the background. Cheap enough to leave running: one
// socket, one thread, woken twice a second per phone on the network.
class DeviceDiscovery {
public:
    DeviceDiscovery();
    ~DeviceDiscovery();

    DeviceDiscovery(const DeviceDiscovery&) = delete;
    DeviceDiscovery& operator=(const DeviceDiscovery&) = delete;

    bool Start();
    void Stop();
    bool IsRunning() const { return running_; }

    // Everything heard from in the last few seconds, most recently seen first.
    // A phone that has gone quiet drops out rather than being offered as a
    // destination that will not answer.
    std::vector<DiscoveredDevice> Devices() const;

    const std::string& LastError() const { return lastError_; }

private:
    void Listen();

    void* socket_ = nullptr;          // SOCKET, kept opaque
    void* thread_ = nullptr;          // HANDLE
    bool running_ = false;

    mutable void* lock_ = nullptr;    // CRITICAL_SECTION*
    std::vector<DiscoveredDevice> devices_;
    std::string lastError_;
};

}  // namespace xcam
