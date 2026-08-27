#include "core/discovery.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace xcam {
namespace {

// A phone that has not been heard from in this long is dropped. Beacons go out
// twice a second, so this survives a handful of lost datagrams without offering
// a destination that has walked out of the building.
constexpr uint64_t kStaleAfterMs = 5000;

// Small enough that a malformed or hostile datagram cannot cost anything.
constexpr int kMaxDatagram = 512;

// The beacon is JSON, but reading it does not need a JSON reader: three fields,
// all at the top level, all of them optional. Anything unparseable simply
// leaves the default, and a datagram that does not identify itself as ours is
// dropped before any of this runs.
std::string ReadStringField(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":\"";
    const size_t at = json.find(needle);
    if (at == std::string::npos) return {};
    const size_t begin = at + needle.size();
    const size_t end = json.find('"', begin);
    if (end == std::string::npos) return {};
    return json.substr(begin, end - begin);
}

int ReadIntField(const std::string& json, const char* key, int fallback) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = json.find(needle);
    if (at == std::string::npos) return fallback;
    return std::atoi(json.c_str() + at + needle.size());
}

}  // namespace

DeviceDiscovery::DeviceDiscovery() {
    auto* cs = new CRITICAL_SECTION();
    InitializeCriticalSection(cs);
    lock_ = cs;
}

DeviceDiscovery::~DeviceDiscovery() {
    Stop();
    if (lock_) {
        DeleteCriticalSection(static_cast<CRITICAL_SECTION*>(lock_));
        delete static_cast<CRITICAL_SECTION*>(lock_);
        lock_ = nullptr;
    }
}

bool DeviceDiscovery::Start() {
    if (running_) return true;

    // WSAStartup is refcounted; NetClient owns one and this owns another.
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        lastError_ = "WSAStartup failed";
        return false;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        lastError_ = "could not create the discovery socket";
        return false;
    }

    // Sharing the port matters: two copies of the app, or the app alongside the
    // probe, should both be able to hear the same beacons rather than the
    // second one failing to bind.
    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(kDiscoveryPort);
    if (bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        lastError_ = "could not bind UDP " + std::to_string(kDiscoveryPort);
        closesocket(sock);
        return false;
    }

    // A receive timeout is what lets the thread notice it has been asked to
    // stop; without it, a network that never speaks would leave it blocked in
    // recvfrom until the process died.
    DWORD timeoutMs = 500;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs),
               sizeof(timeoutMs));

    socket_ = reinterpret_cast<void*>(sock);
    running_ = true;
    thread_ = CreateThread(nullptr, 0,
                           [](void* self) -> DWORD {
                               static_cast<DeviceDiscovery*>(self)->Listen();
                               return 0;
                           },
                           this, 0, nullptr);
    if (!thread_) {
        running_ = false;
        closesocket(sock);
        socket_ = nullptr;
        lastError_ = "could not start the discovery thread";
        return false;
    }
    return true;
}

void DeviceDiscovery::Stop() {
    if (!running_) return;
    running_ = false;

    if (socket_) {
        closesocket(reinterpret_cast<SOCKET>(socket_));
        socket_ = nullptr;
    }
    if (thread_) {
        WaitForSingleObject(static_cast<HANDLE>(thread_), 2000);
        CloseHandle(static_cast<HANDLE>(thread_));
        thread_ = nullptr;
    }
}

void DeviceDiscovery::Listen() {
    char buffer[kMaxDatagram];

    while (running_) {
        sockaddr_in from{};
        int fromLength = sizeof(from);
        const int received = recvfrom(reinterpret_cast<SOCKET>(socket_), buffer,
                                      sizeof(buffer) - 1, 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received <= 0) continue;      // timeout, or the socket was closed

        buffer[received] = '\0';
        const std::string json(buffer, static_cast<size_t>(received));

        // Anything on this port that is not ours is not our business.
        if (json.find("\"xcam\"") == std::string::npos) continue;

        char address[INET_ADDRSTRLEN] = "";
        inet_ntop(AF_INET, &from.sin_addr, address, sizeof(address));

        DiscoveredDevice device;
        device.address = address;
        device.name = ReadStringField(json, "name");
        device.port = static_cast<uint16_t>(ReadIntField(json, "port", 27183));
        device.version = static_cast<uint16_t>(ReadIntField(json, "version", 0));
        device.lastSeenTick = GetTickCount64();

        EnterCriticalSection(static_cast<CRITICAL_SECTION*>(lock_));
        const auto existing = std::find_if(
            devices_.begin(), devices_.end(),
            [&](const DiscoveredDevice& d) { return d.address == device.address; });
        if (existing == devices_.end()) {
            devices_.push_back(device);
        } else {
            *existing = device;
        }
        LeaveCriticalSection(static_cast<CRITICAL_SECTION*>(lock_));
    }
}

std::vector<DiscoveredDevice> DeviceDiscovery::Devices() const {
    std::vector<DiscoveredDevice> out;
    const uint64_t now = GetTickCount64();

    EnterCriticalSection(static_cast<CRITICAL_SECTION*>(lock_));
    for (const DiscoveredDevice& device : devices_) {
        if (now - device.lastSeenTick <= kStaleAfterMs) out.push_back(device);
    }
    LeaveCriticalSection(static_cast<CRITICAL_SECTION*>(lock_));

    std::sort(out.begin(), out.end(),
              [](const DiscoveredDevice& a, const DiscoveredDevice& b) {
                  return a.lastSeenTick > b.lastSeenTick;
              });
    return out;
}

}  // namespace xcam
