#include "core/net_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstring>

namespace xcam {
namespace {

constexpr uint64_t kInvalidSocket = ~0ull;

// Guards against a desynchronised stream turning a length field into an OOM.
constexpr uint32_t kMaxPayload = 64u * 1024 * 1024;

struct WinsockScope {
    WinsockScope() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockScope() { WSACleanup(); }
};

// One init for the process, torn down at exit.
WinsockScope& Winsock() {
    static WinsockScope scope;
    return scope;
}

}  // namespace

NetClient::NetClient() : socket_(kInvalidSocket), sendLock_(nullptr) {
    Winsock();
    auto* cs = new CRITICAL_SECTION;
    InitializeCriticalSection(cs);
    sendLock_ = cs;
}

NetClient::~NetClient() {
    Disconnect();
    auto* cs = static_cast<CRITICAL_SECTION*>(sendLock_);
    DeleteCriticalSection(cs);
    delete cs;
}

void NetClient::Fail(const std::string& what) {
    lastError_ = what + " (winsock " + std::to_string(WSAGetLastError()) + ")";
}

bool NetClient::Connect(const std::string& host, uint16_t port, int timeoutMs) {
    Disconnect();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0) {
        Fail("getaddrinfo failed");
        return false;
    }

    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        sock = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == INVALID_SOCKET) continue;

        // Non-blocking, then select.
        //
        // A blocking connect ignores every timeout this function is given and
        // takes the operating system's instead -- about twenty seconds to an
        // address that is not answering. `timeoutMs` used to reach only
        // SO_RCVTIMEO, which applies after a connection exists and so governs
        // nothing about making one. A desktop retrying a remembered address that
        // had moved therefore sat still for twenty seconds at a time, which is
        // indistinguishable from a hung application, because that is what it is.
        u_long nonBlocking = 1;
        ioctlsocket(sock, FIONBIO, &nonBlocking);

        bool connected = ::connect(sock, it->ai_addr,
                                   static_cast<int>(it->ai_addrlen)) == 0;
        if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(sock, &writable);

            fd_set failed;
            FD_ZERO(&failed);
            FD_SET(sock, &failed);

            timeval limit{};
            limit.tv_sec = timeoutMs / 1000;
            limit.tv_usec = (timeoutMs % 1000) * 1000;

            if (select(0, nullptr, &writable, &failed, &limit) > 0 &&
                FD_ISSET(sock, &writable)) {
                // Writable is not the same as connected: a refused connection
                // also wakes select, and only SO_ERROR tells them apart.
                int error = 0;
                int size = sizeof(error);
                connected = getsockopt(sock, SOL_SOCKET, SO_ERROR,
                                       reinterpret_cast<char*>(&error), &size) == 0 &&
                            error == 0;
            }
        }

        u_long blocking = 0;
        ioctlsocket(sock, FIONBIO, &blocking);

        if (connected) break;
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(result);

    if (sock == INVALID_SOCKET) {
        Fail("connect to " + host + ":" + service + " failed");
        return false;
    }

    // Nagle would coalesce small control writes and, worse, delay them behind
    // an unacknowledged segment -- unacceptable on a path whose whole point is
    // low latency.
    BOOL nodelay = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    const DWORD timeout = static_cast<DWORD>(timeoutMs);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    int recvBuffer = 1 << 21;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&recvBuffer), sizeof(recvBuffer));

    socket_ = static_cast<uint64_t>(sock);
    lastError_.clear();
    return true;
}

void NetClient::Disconnect() {
    if (socket_ == kInvalidSocket) return;
    const SOCKET sock = static_cast<SOCKET>(socket_);
    socket_ = kInvalidSocket;
    shutdown(sock, SD_BOTH);
    closesocket(sock);
}

bool NetClient::ReadExact(void* dst, size_t bytes) {
    auto* out = static_cast<uint8_t*>(dst);
    size_t got = 0;
    while (got < bytes) {
        if (socket_ == kInvalidSocket) return false;
        const int n = recv(static_cast<SOCKET>(socket_),
                           reinterpret_cast<char*>(out + got),
                           static_cast<int>(bytes - got), 0);
        if (n == 0) {
            lastError_ = "connection closed by phone";
            return false;
        }
        if (n < 0) {
            Fail("recv failed");
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

bool NetClient::SendPairing(const std::string& code) {
    // Escaped by construction rather than by a quoting routine: the code is
    // six digits and anything else is refused here, so nothing a person could
    // paste into the box can reach the JSON.
    std::string digits;
    for (char c : code) {
        if (c >= '0' && c <= '9') digits.push_back(c);
    }
    if (digits.empty()) {
        lastError_ = "the pairing code is digits only";
        return false;
    }
    return SendControl("{\"cmd\":\"pair\",\"code\":\"" + digits + "\"}");
}

bool NetClient::ReadHandshake(DeviceInfo& out) {
    needsPairing_ = false;

    uint8_t head[12];
    if (!ReadExact(head, sizeof(head))) return false;

    if (std::memcmp(head, "XCAM", 4) != 0) {
        lastError_ = "bad handshake magic";
        return false;
    }

    uint16_t version = 0;
    uint32_t jsonLen = 0;
    std::memcpy(&version, head + 4, sizeof(version));
    std::memcpy(&jsonLen, head + 8, sizeof(jsonLen));

    if (version != kProtocolVersion) {
        lastError_ = "protocol version " + std::to_string(version) +
                     ", expected " + std::to_string(kProtocolVersion);
        return false;
    }
    if (jsonLen == 0 || jsonLen > kMaxPayload) {
        lastError_ = "handshake length out of range";
        return false;
    }

    std::string json(jsonLen, '\0');
    if (!ReadExact(json.data(), jsonLen)) return false;

    // The challenge. Deliberately checked before parsing: it carries no
    // cameras, so the parser below would reject it with a message about
    // cameras, and "this phone has no usable cameras" is the wrong thing to
    // tell somebody whose real problem is that they have not typed a code.
    if (json.find("\"pairing\"") != std::string::npos &&
        json.find("\"required\"") != std::string::npos) {
        needsPairing_ = true;
        lastError_ = "this phone wants a pairing code";
        return false;
    }

    if (!ParseHandshakeJson(json, out)) {
        lastError_ = "handshake JSON had no usable cameras";
        return false;
    }
    return true;
}

bool NetClient::ReadPacket(Packet& out) {
    uint8_t head[kHeaderSize];
    if (!ReadExact(head, sizeof(head))) return false;

    if (!ParseHeader(head, out.header)) {
        lastError_ = "bad packet magic; stream desynchronised";
        return false;
    }
    if (out.header.length > kMaxPayload) {
        lastError_ = "payload length out of range";
        return false;
    }

    out.payload.resize(out.header.length);
    if (out.header.length > 0 && !ReadExact(out.payload.data(), out.header.length)) return false;
    return true;
}

bool NetClient::SendControl(const std::string& json, uint32_t seq) {
    if (socket_ == kInvalidSocket) return false;

    PacketHeader header;
    header.type = PacketType::Control;
    header.length = static_cast<uint32_t>(json.size());
    header.seq = seq;

    std::vector<uint8_t> buffer(kHeaderSize + json.size());
    WriteHeader(header, buffer.data());
    std::memcpy(buffer.data() + kHeaderSize, json.data(), json.size());

    auto* cs = static_cast<CRITICAL_SECTION*>(sendLock_);
    EnterCriticalSection(cs);

    size_t sent = 0;
    bool ok = true;
    while (sent < buffer.size()) {
        const int n = send(static_cast<SOCKET>(socket_),
                           reinterpret_cast<const char*>(buffer.data() + sent),
                           static_cast<int>(buffer.size() - sent), 0);
        if (n <= 0) {
            Fail("send failed");
            ok = false;
            break;
        }
        sent += static_cast<size_t>(n);
    }

    LeaveCriticalSection(cs);
    return ok;
}

}  // namespace xcam
