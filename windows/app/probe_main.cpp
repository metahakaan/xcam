// Console client that mirrors tools/dump_stream.py, so the C++ protocol, adb and
// decoder layers can be checked against a real phone before any window exists.
//
//   xcam-probe --info                 handshake only
//   xcam-probe --seconds 10           connect, decode, report timing
//   xcam-probe --out capture.h264     also save the elementary stream
//   xcam-probe --record               also record a take on the phone
//   xcam-probe --no-recorder          take the recording encoder out entirely
//   xcam-probe --audio-out mic.aac    save the sound as ADTS, playable anywhere
//   xcam-probe --host 192.168.1.10    over Wi-Fi instead of USB
//   xcam-probe --discover             list phones announcing themselves
//   xcam-probe --selftest-encode      check the grading encoder on this machine
//   xcam-probe --watch-debug          watch what the virtual camera filter says
//   xcam-probe --preroll 10           arm the ring, then record from before the start
//   xcam-probe --selftest-mic         list the microphones here, capture and encode one
//   xcam-probe --serve capture.h264   pretend to be a phone, replaying that stream

#include "app/camera_model.h"
#include "core/adb.h"
#include "core/discovery.h"
#include "core/mf_decoder.h"
#include "core/mf_aac_encoder.h"
#include "core/mf_encoder.h"
#include "core/wasapi_capture.h"
#include "core/mp4_writer.h"
#include "core/net_client.h"
#include "core/protocol.h"

// Before windows.h, which would otherwise pull in the older winsock and leave
// every socket name defined twice.
#include <thread>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace xcam;

namespace {

struct Options {
    bool info = false;
    bool decoders = false;
    bool discover = false;
    bool selftestEncode = false;
    bool watchDebug = false;
    int preRoll = 0;
    bool selftestMic = false;
    int micIndex = -1;
    std::string serve;
    int servePort = 27185;
    std::string serveName = "Phantom";
    double seconds = 10.0;
    std::string out;
    std::string audioOut;
    std::string host;
    std::string cameraId;
    int width = 0, height = 0, fps = 0, bitrate = 0;
    std::string codec;
    bool noAdb = false;
    bool noDecode = false;

    // Runs a local recording alongside the stream, so the whole two-encoder
    // path can be exercised without a window to click in.
    bool record = false;
    bool noRecorder = false;
    bool recordToPhone = false;
    int recordWidth = 0, recordHeight = 0, recordFps = 0;
    std::string recordCodec;
};

double NowSeconds() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

bool ParseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](int& out) { if (i + 1 < argc) out = std::atoi(argv[++i]); };

        if (arg == "--info")            opt.info = true;
        else if (arg == "--decoders")   opt.decoders = true;
        else if (arg == "--discover")   opt.discover = true;
        else if (arg == "--watch-debug") opt.watchDebug = true;
        else if (arg == "--preroll" && i + 1 < argc) opt.preRoll = std::atoi(argv[++i]);
        else if (arg == "--serve" && i + 1 < argc) opt.serve = argv[++i];
        else if (arg == "--port" && i + 1 < argc) opt.servePort = std::atoi(argv[++i]);
        else if (arg == "--name" && i + 1 < argc) opt.serveName = argv[++i];
        else if (arg == "--selftest-mic") {
            opt.selftestMic = true;
            // An optional index into the listing, so a machine whose default
            // input is a virtual device nobody feeds can still be checked.
            if (i + 1 < argc && argv[i + 1][0] != '-') opt.micIndex = std::atoi(argv[++i]);
        }
        else if (arg == "--selftest-encode") opt.selftestEncode = true;
        else if (arg == "--no-adb")     opt.noAdb = true;
        else if (arg == "--no-decode")  opt.noDecode = true;
        else if (arg == "--record")     opt.record = true;
        else if (arg == "--no-recorder") opt.noRecorder = true;
        else if (arg == "--record-to-phone") opt.recordToPhone = true;
        else if (arg == "--record-codec" && i + 1 < argc) opt.recordCodec = argv[++i];
        else if (arg == "--record-width")  next(opt.recordWidth);
        else if (arg == "--record-height") next(opt.recordHeight);
        else if (arg == "--record-fps")    next(opt.recordFps);
        else if (arg == "--seconds" && i + 1 < argc) opt.seconds = std::atof(argv[++i]);
        else if (arg == "--out" && i + 1 < argc)     opt.out = argv[++i];
        else if (arg == "--audio-out" && i + 1 < argc) opt.audioOut = argv[++i];
        else if (arg == "--host" && i + 1 < argc) {
            // A host means Wi-Fi, and Wi-Fi means adb has nothing to do with
            // this -- there may not be a cable attached at all.
            opt.host = argv[++i];
            opt.noAdb = true;
        }
        else if (arg == "--camera" && i + 1 < argc)  opt.cameraId = argv[++i];
        else if (arg == "--codec" && i + 1 < argc)   opt.codec = argv[++i];
        else if (arg == "--width")   next(opt.width);
        else if (arg == "--height")  next(opt.height);
        else if (arg == "--fps")     next(opt.fps);
        else if (arg == "--bitrate") next(opt.bitrate);
        else {
            std::printf("unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

void PrintDevice(const DeviceInfo& info) {
    std::printf("  device : %s (API %d)\n", info.deviceName.c_str(), info.androidApi);

    std::printf("  codecs : ");
    for (size_t i = 0; i < info.codecs.size(); ++i) {
        std::printf("%s%s", info.codecs[i].c_str(), i + 1 < info.codecs.size() ? ", " : "");
    }
    std::printf("\n");

    for (const CameraInfo& cam : info.cameras) {
        std::printf("\n  camera %s  %s (%s)  %dx%d  zoom %.1f-%.1fx%s\n",
                    cam.id.c_str(), cam.label.c_str(), cam.facing.c_str(),
                    cam.maxWidth, cam.maxHeight,
                    cam.zoomMin, cam.zoomMax, cam.logical ? "  [logical]" : "");

        std::printf("    manual   : sensor %s, post-processing %s\n",
                    cam.manualSensor ? "yes" : "NO",
                    cam.manualPostProcessing ? "yes" : "NO");
        std::printf("    log      : %s\n", cam.logProfile ? "available" : "NO");
        std::printf("    iso      : %d - %d\n", cam.isoMin, cam.isoMax);
        std::printf("    shutter  : %.0f us - %.0f ms\n",
                    cam.exposureMinNs / 1000.0, cam.exposureMaxNs / 1e6);
        std::printf("    focus    : %s (min distance %.2f dioptres)\n",
                    cam.SupportsManualFocus() ? "manual available" : "fixed",
                    cam.minFocusDistance);
        std::printf("    ev       : %.1f to %.1f step %.3f\n",
                    cam.evMin, cam.evMax, cam.evStep);

        std::printf("    af modes : ");
        for (const auto& m : cam.afModes) std::printf("%s ", m.c_str());
        std::printf("\n    wb modes : ");
        for (const auto& m : cam.awbModes) std::printf("%s ", m.c_str());

        std::printf("\n    modes    : ");
        for (size_t i = 0; i < cam.modes.size(); ++i) {
            std::printf("%dx%d@%d  ", cam.modes[i].width, cam.modes[i].height,
                        cam.modes[i].maxFps);
        }
        std::printf("\n");
    }
}

ID3D11Device* CreateDevice() {
    ID3D11Device* device = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

    // BGRA_SUPPORT for the preview later; VIDEO_SUPPORT is what lets the
    // decoder MFT bind this device for DXVA.
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, nullptr);
    if (FAILED(hr)) return nullptr;

    // The decoder and the renderer touch this device from different threads.
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    return device;
}

}  // namespace

// Listens on the system debug channel for whatever the filter says.
//
// Exists because the log file does not reach far enough. This filter is loaded
// into whoever wants a camera, and some of those run in a sandbox that can
// neither read the flag file that turns tracing on nor write the log -- so an
// empty log means "no evidence", not "nothing happened", which is the least
// useful answer a diagnostic can give.
//
// OutputDebugString needs no filesystem at all. The transport is a fixed-name
// section and two events, and only one listener may hold them at a time -- so a
// debugger already attached to the process, or another copy of this, will take
// the messages instead.
// A phone that is not there.
//
// Speaks the phone's side of the protocol and replays a recorded elementary
// stream. It exists because multicam cannot be built against one phone, and
// because a great deal of this desktop -- the decoder, the panel, the virtual
// camera, the recorder -- has nothing to do with a camera and everything to do
// with a stream arriving.
//
// It is also a phone that never runs out of battery, never gets hot, and
// reproduces the same failure every time, which is worth something on its own.
//
//   xcam-probe --serve capture.h264 --port 27185 --name "Phantom A"
int ServeStream(const std::string& path, uint16_t port, const std::string& name,
                int fps) {
    // ---- the stream, split into access units --------------------------------

    std::vector<uint8_t> file;
    {
        FILE* in = nullptr;
        if (fopen_s(&in, path.c_str(), "rb") != 0 || !in) {
            std::printf("could not open %s\n", path.c_str());
            return 1;
        }
        std::fseek(in, 0, SEEK_END);
        file.resize(static_cast<size_t>(std::ftell(in)));
        std::fseek(in, 0, SEEK_SET);
        const size_t read = std::fread(file.data(), 1, file.size(), in);
        file.resize(read);
        std::fclose(in);
    }
    if (file.size() < 8) {
        std::printf("%s holds no stream\n", path.c_str());
        return 1;
    }

    // Start codes, then one access unit per picture.
    //
    // Parameter sets are kept apart from the pictures: the client wants them as
    // a CONFIG packet once, and every picture as its own packet. Anything more
    // careful than this -- slice headers, first_mb_in_slice -- would be an H.264
    // parser, and what is being tested is the transport.
    std::vector<std::pair<size_t, size_t>> nals;      // offset, length
    for (size_t i = 0; i + 4 < file.size();) {
        const bool four = file[i] == 0 && file[i + 1] == 0 && file[i + 2] == 0 &&
                          file[i + 3] == 1;
        const bool three = file[i] == 0 && file[i + 1] == 0 && file[i + 2] == 1;
        if (!four && !three) { ++i; continue; }

        const size_t start = i;
        i += four ? 4 : 3;
        size_t next = i;
        while (next + 3 < file.size()) {
            if (file[next] == 0 && file[next + 1] == 0 &&
                (file[next + 2] == 1 ||
                 (file[next + 2] == 0 && next + 3 < file.size() && file[next + 3] == 1))) {
                break;
            }
            ++next;
        }
        nals.emplace_back(start, (next + 3 < file.size() ? next : file.size()) - start);
        i = next;
    }

    std::vector<uint8_t> config;
    std::vector<std::pair<std::vector<uint8_t>, bool>> pictures;   // bytes, isKey
    std::vector<uint8_t> pending;

    for (const auto& [offset, length] : nals) {
        const size_t payload = offset + (file[offset + 2] == 1 ? 3 : 4);
        if (payload >= file.size()) continue;
        const uint8_t type = file[payload] & 0x1F;

        if (type == 7 || type == 8) {                 // SPS, PPS
            config.insert(config.end(), file.begin() + offset,
                          file.begin() + offset + length);
            continue;
        }
        if (type == 1 || type == 5) {                 // a picture
            pending.assign(file.begin() + offset, file.begin() + offset + length);
            pictures.emplace_back(pending, type == 5);
        }
    }

    if (config.empty() || pictures.empty()) {
        std::printf("%s has %zu parameter-set bytes and %zu pictures; nothing to serve\n",
                    path.c_str(), config.size(), pictures.size());
        return 1;
    }
    std::printf("serving %zu pictures from %s on port %u as \"%s\"\n",
                pictures.size(), path.c_str(), port, name.c_str());

    // ---- the socket ---------------------------------------------------------

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        std::printf("could not open a socket\n");
        return 1;
    }
    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        std::printf("could not listen on %u\n", port);
        closesocket(listener);
        return 1;
    }

    while (true) {
        std::printf("waiting for a client...\n");
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        std::printf("client connected\n");

        BOOL nodelay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        // The handshake describes a camera that could plausibly exist. The
        // modes are the ones the client offers as choices, so a panel drawn
        // against this looks like a panel drawn against a phone.
        //
        // It claims every capability the panel has a control for -- a recorder,
        // a microphone, manual sensor, a log profile, a torch, a zoom range.
        // A phantom that admitted to none of them would draw half a panel, and
        // half a panel is no use for looking at the panel.
        char json[1400];
        const int jsonLen = std::snprintf(json, sizeof(json),
            "{\"deviceName\":\"%s\",\"androidApi\":36,\"maxBitrate\":200000000,"
            "\"recorder\":true,\"recordDir\":\"/sdcard/Movies/XCam\",\"resumed\":false,"
            "\"codecs\":[\"h264\"],"
            "\"audio\":{\"available\":true,\"sampleRate\":48000,\"channels\":2},"
            "\"cameras\":[{\"id\":\"phantom\",\"facing\":\"back\",\"label\":\"%s\","
            "\"maxFps\":60,\"hasTorch\":true,\"logical\":true,"
            "\"maxRes\":[3840,2160],\"zoomRange\":[0.6,10.0],"
            "\"logProfile\":true,\"manualSensor\":true,"
            // The ranges as well as the flag: SupportsManualExposure wants both,
            // and a phantom that claimed manual sensor without them drew an ISO
            // cell that could not be clicked -- which looked like a panel fault
            // and was a handshake fault.
            "\"isoRange\":[50,6400],\"exposureRangeNs\":[125000,500000000],"
            "\"evRange\":[-4.0,4.0],"
            "\"manualPostProcessing\":true,\"minFocusDistance\":10.0,\"evStep\":0.1666,"
            "\"modes\":[{\"size\":[3840,2160],\"maxFps\":30},"
            "{\"size\":[2560,1440],\"maxFps\":60},"
            "{\"size\":[1920,1080],\"maxFps\":60},"
            "{\"size\":[1280,720],\"maxFps\":60}]}]}",
            name.c_str(), name.c_str());
        (void)fps;

        uint8_t head[12] = {'X', 'C', 'A', 'M'};
        const uint16_t version = kProtocolVersion;
        const uint32_t length = static_cast<uint32_t>(jsonLen);
        std::memcpy(head + 4, &version, sizeof(version));
        std::memcpy(head + 8, &length, sizeof(length));

        bool alive = send(client, reinterpret_cast<const char*>(head), sizeof(head), 0) > 0 &&
                     send(client, json, jsonLen, 0) > 0;

        // Whether a take is supposed to be running.
        //
        // Nothing is written -- there is no camera and no encoder -- but the
        // client is told, because a take that never reports itself as started
        // means every path that follows one is unreachable without a phone:
        // the graded recording, the sidecar, the pre-roll indicator, the tally.
        std::atomic<bool> recording{false};
        std::atomic<int64_t> recordingSince{0};

        // Control commands are read and acknowledged so the client does not sit
        // waiting for an answer. Nothing else is acted on: there is no camera.
        std::thread reader([client, &recording, &recordingSince] {
            std::vector<uint8_t> buffer(64 * 1024);
            while (true) {
                uint8_t header[kHeaderSize];
                int got = recv(client, reinterpret_cast<char*>(header), kHeaderSize,
                               MSG_WAITALL);
                if (got != static_cast<int>(kHeaderSize)) break;

                PacketHeader parsed;
                if (!ParseHeader(header, parsed)) break;
                if (parsed.length > buffer.size()) buffer.resize(parsed.length);
                if (parsed.length > 0) {
                    got = recv(client, reinterpret_cast<char*>(buffer.data()),
                               static_cast<int>(parsed.length), MSG_WAITALL);
                    if (got != static_cast<int>(parsed.length)) break;
                }
                if (parsed.type != PacketType::Control) continue;

                const std::string body(reinterpret_cast<const char*>(buffer.data()),
                                       parsed.length);
                std::string command = "unknown";
                const size_t at = body.find("\"cmd\":\"");
                if (at != std::string::npos) {
                    const size_t end = body.find('"', at + 7);
                    if (end != std::string::npos) command = body.substr(at + 7, end - at - 7);
                }

                if (command == "record") {
                    const bool start = body.find("\"start\"") != std::string::npos;
                    if (start && !recording.load()) {
                        recordingSince.store(static_cast<int64_t>(NowSeconds() * 1000));
                    }
                    recording.store(start);
                }

                char ack[256];
                const int ackLen = std::snprintf(ack, sizeof(ack),
                    "{\"ok\":true,\"cmd\":\"%s\",\"applied\":{}}", command.c_str());

                PacketHeader out;
                out.type = PacketType::Ack;
                out.length = static_cast<uint32_t>(ackLen);
                uint8_t outHead[kHeaderSize];
                WriteHeader(out, outHead);
                if (send(client, reinterpret_cast<const char*>(outHead), kHeaderSize, 0) <= 0) break;
                if (send(client, ack, ackLen, 0) <= 0) break;
            }
        });

        auto sendPacket = [&](PacketType type, const uint8_t* data, size_t bytes,
                              uint64_t ptsUs, uint32_t seq) {
            PacketHeader header;
            header.type = type;
            header.length = static_cast<uint32_t>(bytes);
            header.ptsUs = ptsUs;
            header.seq = seq;

            uint8_t buffer[kHeaderSize];
            WriteHeader(header, buffer);
            if (send(client, reinterpret_cast<const char*>(buffer), kHeaderSize, 0) <= 0) {
                return false;
            }
            return bytes == 0 ||
                   send(client, reinterpret_cast<const char*>(data),
                        static_cast<int>(bytes), 0) > 0;
        };

        if (alive) alive = sendPacket(PacketType::Config, config.data(), config.size(), 0, 0);

        const double frameSeconds = 1.0 / (fps > 0 ? fps : 30);
        const double started = NowSeconds();
        double nextStats = started + 1.0;
        uint32_t seq = 0;
        size_t index = 0;

        while (alive) {
            const auto& [bytes, isKey] = pictures[index % pictures.size()];
            const uint64_t ptsUs = static_cast<uint64_t>(seq * frameSeconds * 1e6);

            // A key frame every time the loop comes round, whatever the file
            // says, or a client joining mid-replay would wait for one that only
            // exists at the start of the recording.
            const bool key = isKey || (index % pictures.size()) == 0;
            alive = sendPacket(key ? PacketType::KeyFrame : PacketType::Delta,
                               bytes.data(), bytes.size(), ptsUs, seq);
            ++seq;
            ++index;

            const double now = NowSeconds();
            if (alive && now >= nextStats) {
                nextStats = now + 1.0;
                const bool taking = recording.load();
                const int64_t took = taking
                    ? static_cast<int64_t>(NowSeconds() * 1000) - recordingSince.load()
                    : 0;

                char stats[320];
                const int statsLen = std::snprintf(stats, sizeof(stats),
                    "{\"actualFps\":%.1f,\"actualBitrate\":0,\"droppedFrames\":0,"
                    "\"battery\":100,\"thermal\":\"none\",\"recording\":%s,"
                    "\"recordMs\":%lld,\"recordBytes\":%lld,\"storageFreeMb\":40000,"
                    "\"surfaceRotation\":0,\"audioPeak\":0.0}",
                    static_cast<double>(fps), taking ? "true" : "false",
                    static_cast<long long>(took),
                    static_cast<long long>(took * 6000));
                alive = sendPacket(PacketType::Stats,
                                   reinterpret_cast<const uint8_t*>(stats), statsLen, 0, 0);
            }

            // Paced against the start rather than by sleeping a frame each time,
            // so the replay does not drift a little later every picture.
            const double due = started + seq * frameSeconds;
            const double wait = due - NowSeconds();
            if (wait > 0) {
                std::this_thread::sleep_for(std::chrono::duration<double>(wait));
            }
        }

        std::printf("client gone after %u pictures\n", seq);
        shutdown(client, SD_BOTH);
        closesocket(client);
        if (reader.joinable()) reader.join();
    }

    closesocket(listener);
    return 0;
}

int WatchDebugChannel(double seconds) {
    // The names are fixed by the OS; DBWIN_BUFFER is 4096 bytes, of which the
    // first four are the writing process id.
    struct DbWin {
        DWORD processId;
        char data[4096 - sizeof(DWORD)];
    };

    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_BUFFER_READY");
    HANDLE dataReady = CreateEventW(nullptr, FALSE, FALSE, L"DBWIN_DATA_READY");
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, sizeof(DbWin), L"DBWIN_BUFFER");

    if (!ready || !dataReady || !section) {
        std::printf("could not open the debug channel: %lu\n", GetLastError());
        std::printf("something else is listening -- a debugger, or another copy of this.\n");
        return 1;
    }

    auto* buffer = static_cast<DbWin*>(
        MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(DbWin)));
    if (!buffer) {
        std::printf("could not map the debug buffer: %lu\n", GetLastError());
        return 1;
    }

    std::printf("listening on the debug channel for %.0f seconds.\n", seconds);
    std::printf("select XCam as the camera now; only [XCam] lines are shown.\n\n");

    const double until = NowSeconds() + seconds;
    size_t shown = 0;

    while (NowSeconds() < until) {
        // Telling the writer the buffer is free is what starts a round trip;
        // without it every OutputDebugString in the system blocks on its
        // timeout instead.
        SetEvent(ready);

        if (WaitForSingleObject(dataReady, 250) != WAIT_OBJECT_0) continue;

        buffer->data[sizeof(buffer->data) - 1] = '\0';
        std::string text = buffer->data;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();

        if (text.rfind("[XCam]", 0) == 0) {
            std::printf("  pid %-6lu %s\n", buffer->processId, text.c_str() + 7);
            std::fflush(stdout);
            ++shown;
        }
    }

    UnmapViewOfFile(buffer);
    CloseHandle(section);
    CloseHandle(dataReady);
    CloseHandle(ready);

    if (shown == 0) {
        std::printf("\nnothing from the filter. Either it was never loaded into the\n");
        std::printf("application at all, or the application never got as far as\n");
        std::printf("creating it.\n");
    } else {
        std::printf("\n%zu lines.\n", shown);
    }
    return shown == 0 ? 1 : 0;
}

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) return 2;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (opt.decoders) {
        for (const char* codec : {"h264", "hevc"}) {
            std::printf("%s decoders:\n", codec);
            const auto list = MfDecoder::ListDecoders(codec);
            if (list.empty()) std::printf("  (none)\n");
            for (const auto& d : list) {
                std::printf("  %-52s %s%s\n", d.name.c_str(),
                            d.hardware ? "hardware" : "software",
                            d.async ? ", async (unusable here)" : "");
            }
        }
        return 0;
    }

    if (opt.selftestEncode) {
        // The encoder only exists for graded recording, and that path cannot be
        // exercised without a phone. This can: a moving synthetic frame, encoded
        // and muxed exactly as a real one would be, so a machine with no working
        // encoder says so here rather than half way through someone's take.
        const uint32_t w = 1280, h = 720, fps = 30, seconds = 3;
        std::vector<uint8_t> frame(static_cast<size_t>(w) * h * 3 / 2);

        for (const char* codec : {"h264", "hevc"}) {
            std::printf("%-5s encoder: opening...\n", codec);
            std::fflush(stdout);
            MfEncoder encoder;
            if (!encoder.Open(codec, w, h, fps, 20'000'000)) {
                std::printf("%-5s encoder: %s\n", codec, encoder.LastError().c_str());
                continue;
            }

            const std::string dir = Mp4Writer::DefaultDirectory();
            const std::string path = (dir.empty() ? std::string() : dir + "\\") +
                                     "xcam-selftest-" + codec + ".mp4";

            Mp4Writer writer;
            uint64_t bytes = 0, samples = 0;
            bool opened = false;

            for (uint32_t i = 0; i < fps * seconds; ++i) {
                // A sweep and a moving bar: something an encoder cannot make
                // cheap, so the bitrate means something.
                for (uint32_t y = 0; y < h; ++y) {
                    uint8_t* row = frame.data() + static_cast<size_t>(y) * w;
                    for (uint32_t x = 0; x < w; ++x) {
                        row[x] = static_cast<uint8_t>((x + y + i * 9) & 0xFF);
                    }
                }
                std::memset(frame.data() + static_cast<size_t>(w) * h, 128,
                            static_cast<size_t>(w) * h / 2);

                if (i % 15 == 0) { std::printf("  frame %u\n", i); std::fflush(stdout); }
                encoder.Encode(frame.data(), i * 1'000'000ull / fps,
                               [&](const uint8_t* data, size_t n, uint64_t pts, bool key) {
                                   if (!opened) {
                                       opened = writer.Open(
                                           path, codec, w, h, fps,
                                           reinterpret_cast<const uint8_t*>(
                                               encoder.CodecPrivateData().data()),
                                           encoder.CodecPrivateData().size(),
                                           nullptr, 0, 0, 0);
                                   }
                                   const bool wrote =
                                       opened && writer.WriteVideo(data, n, pts, key);
                                   if (samples < 6 || !wrote) {
                                       std::printf("    sample %llu pts=%llu key=%d %s\n",
                                                   (unsigned long long)samples,
                                                   (unsigned long long)pts, key ? 1 : 0,
                                                   wrote ? "written" : "DROPPED");
                                   }
                                   bytes += n;
                                   ++samples;
                               });
            }
            encoder.Drain([&](const uint8_t* data, size_t n, uint64_t pts, bool key) {
                if (opened) writer.WriteVideo(data, n, pts, key);
                bytes += n;
                ++samples;
            });
            writer.Close();

            std::printf("%-5s encoder: %llu samples, %.1f MB -> %s\n", codec,
                        static_cast<unsigned long long>(samples),
                        static_cast<double>(bytes) / 1e6,
                        opened ? path.c_str() : "(no file)");
        }
        return 0;
    }

    if (!opt.serve.empty()) {
        return ServeStream(opt.serve, static_cast<uint16_t>(opt.servePort),
                           opt.serveName, opt.fps > 0 ? opt.fps : 30);
    }

    if (opt.selftestMic) {
        const auto inputs = WasapiCapture::List();
        if (inputs.empty()) {
            std::printf("no capture endpoints on this machine\n");
            return 1;
        }
        for (size_t i = 0; i < inputs.size(); ++i) {
            const auto& input = inputs[i];
            std::printf("  [%zu] ", i);
            std::printf("%s%s\n      %s\n", input.name.c_str(),
                        input.isDefault ? "  (default)" : "", input.id.c_str());
        }

        WasapiCapture mic;
        MfAacEncoder aac;
        std::atomic<int> frames{0};
        std::atomic<size_t> encoded{0};

        const std::string chosen =
            (opt.micIndex >= 0 && opt.micIndex < static_cast<int>(inputs.size()))
                ? inputs[opt.micIndex].id
                : std::string();

        if (!mic.Start(chosen, [&](const int16_t* samples, size_t count, uint64_t) {
                aac.Encode(samples, count, 0,
                           [&](const uint8_t*, size_t bytes, uint64_t) {
                               ++frames;
                               encoded += bytes;
                           });
            })) {
            std::printf("\ncould not open the default input: %s\n", mic.LastError().c_str());
            return 1;
        }

        // Opened after the capture, because the rate and channel count are the
        // device's to decide and the encoder has to be told them.
        if (!aac.Open(mic.SampleRate(), mic.Channels(), 192000)) {
            std::printf("\ncould not encode: %s\n", aac.LastError().c_str());
            mic.Stop();
            return 1;
        }
        std::printf("\ncapturing from %s at %u Hz, %u ch for 3 seconds...\n",
                    mic.DeviceName().c_str(), mic.SampleRate(), mic.Channels());

        float loudest = 0.0f;
        uint32_t silent = 0;
        for (int tick = 0; tick < 30; ++tick) {
            Sleep(100);
            loudest = (std::max)(loudest, mic.TakePeak());
            silent += mic.TakeSilentBuffers();
        }
        mic.Stop();

        std::printf("  %d AAC frames, %zu kB, peak %.2f of full scale\n",
                    frames.load(), encoded.load() / 1000, loudest);
        std::printf("  AudioSpecificConfig: %zu bytes\n",
                    aac.AudioSpecificConfig().size());

        // A microphone that produced nothing at all is the failure this whole
        // project keeps meeting, so it is called out rather than counted.
        if (loudest <= 0.0f && silent > 0) {
            std::printf("  the endpoint reported %u silent buffers: this input is\n"
                        "  muted, or Windows is not letting desktop apps hear it\n",
                        silent);
            return 1;
        }
        if (loudest <= 0.0f) {
            std::printf("  the microphone delivered silence\n");
            return 1;
        }
        return frames.load() > 0 ? 0 : 1;
    }

    if (opt.watchDebug) return WatchDebugChannel(opt.seconds);

    if (opt.discover) {
        DeviceDiscovery discovery;
        if (!discovery.Start()) {
            std::printf("%s\n", discovery.LastError().c_str());
            return 1;
        }
        std::printf("listening on UDP %u for %.0f seconds...\n",
                    kDiscoveryPort, opt.seconds);

        const double until = NowSeconds() + opt.seconds;
        size_t reported = 0;
        while (NowSeconds() < until) {
            Sleep(200);
            const auto found = discovery.Devices();
            if (found.size() > reported) {
                for (size_t i = reported; i < found.size(); ++i) {
                    std::printf("  %-16s %s (protocol v%u, port %u)\n",
                                found[i].address.c_str(), found[i].name.c_str(),
                                found[i].version, found[i].port);
                }
                reported = found.size();
            }
        }
        if (reported == 0) {
            std::printf("  nothing found. The phone announces itself only while\n");
            std::printf("  capture is running, and only on Wi-Fi.\n");
        }
        discovery.Stop();
        return reported == 0 ? 1 : 0;
    }

    std::string serial;
    if (!opt.noAdb) {
        const std::string adb = FindAdb();
        if (adb.empty()) {
            std::printf("adb not found; set ANDROID_HOME or pass --no-adb\n");
            return 1;
        }
        const auto devices = ListDevices(adb);
        const auto usable = std::find_if(devices.begin(), devices.end(),
                                         [](const AdbDevice& d) { return d.IsUsable(); });
        if (usable == devices.end()) {
            std::printf("no usable device on adb (check the cable and USB debugging)\n");
            return 1;
        }
        serial = usable->serial;
        std::printf("device : %s %s\n", serial.c_str(), usable->model.c_str());

        if (!EnsureForward(adb, kDefaultPort, serial)) {
            std::printf("could not set up the adb forward\n");
            return 1;
        }
    }

    NetClient client;
    const std::string target = opt.host.empty() ? std::string("127.0.0.1") : opt.host;
    if (!client.Connect(target, kDefaultPort)) {
        std::printf("%s\n", client.LastError().c_str());
        std::printf("  - is the XCam app running with capture started?\n");
        return 1;
    }
    std::printf("connected to %s:%u\n", target.c_str(), kDefaultPort);

    DeviceInfo info;
    if (!client.ReadHandshake(info)) {
        std::printf("handshake failed: %s\n", client.LastError().c_str());
        return 1;
    }
    std::printf("protocol v%u\n", kProtocolVersion);
    PrintDevice(info);

    // What the panel would actually offer, which is not the same as what the
    // phone reports -- the filter is where a default can go wrong.
    for (size_t i = 0; i < info.cameras.size(); ++i) {
        CameraModel model;
        model.device = info;
        model.cameraIndex = i;
        std::printf("\n  camera %s offered modes: ", info.cameras[i].id.c_str());
        for (const CaptureMode& mode : model.PreferredModes()) {
            std::printf("%dx%d@%d  ", mode.width, mode.height, mode.maxFps);
        }
        std::printf("\n");
    }

    if (opt.info) return 0;

    if (opt.width || opt.height || opt.fps || opt.bitrate ||
        !opt.codec.empty() || !opt.cameraId.empty()) {
        const std::string cmd = MakeSetCommand(opt.cameraId, opt.width, opt.height,
                                               opt.fps, opt.bitrate, opt.codec);
        std::printf("applying %s\n", cmd.c_str());
        client.SendControl(cmd);
    }

    ID3D11Device* device = nullptr;
    MfDecoder decoder;
    if (!opt.noDecode) {
        device = CreateDevice();
        if (!device) {
            std::printf("could not create a D3D11 device; decoding disabled\n");
            opt.noDecode = true;
        }
    }

    FILE* outFile = nullptr;
    if (!opt.out.empty()) {
        fopen_s(&outFile, opt.out.c_str(), "wb");
        if (!outFile) std::printf("could not open %s for writing\n", opt.out.c_str());
    }

    if (opt.noRecorder) {
        client.SendControl(MakeRecordConfigCommand(false, true));
    } else if (opt.preRoll > 0) {
        // The ring is local takes only, so asking for it also picks the target.
        client.SendControl(MakeRecordConfigCommand(true, false,
                                                   opt.recordWidth, opt.recordHeight,
                                                   opt.recordFps, 0, opt.recordCodec,
                                                   opt.preRoll));
    } else if (opt.record && (opt.recordWidth > 0 || opt.recordHeight > 0 ||
                              opt.recordFps > 0 || !opt.recordCodec.empty())) {
        client.SendControl(MakeRecordConfigCommand(true, !opt.recordToPhone,
                                                   opt.recordWidth, opt.recordHeight,
                                                   opt.recordFps, 0, opt.recordCodec));
    }

    const double started = NowSeconds();
    double lastReport = started;
    bool recordRequested = false;
    std::string recordFile;
    StatsInfo lastStats;
    uint64_t audioPackets = 0, audioBytes = 0;
    bool haveAudioConfig = false;
    std::vector<uint8_t> audioAsc;

    Mp4Writer recording;
    int recordWidth = 0, recordHeight = 0, recordFps = 0;
    std::string recordCodec = "hevc";

    FILE* audioFile = nullptr;
    if (!opt.audioOut.empty()) {
        fopen_s(&audioFile, opt.audioOut.c_str(), "wb");
        if (!audioFile) std::printf("could not open %s\n", opt.audioOut.c_str());
    }
    // Read out of the AudioSpecificConfig, so the ADTS headers below describe
    // the stream the phone actually sent rather than what we assumed.
    int ascObjectType = 2, ascRateIndex = 3, ascChannels = 2;
    uint64_t frames = 0, decoded = 0, totalBytes = 0;
    uint32_t expectedSeq = 0;
    bool haveSeq = false;
    int gaps = 0;
    bool haveConfig = false;
    bool reportedDecodeError = false;

    double latencyBase = 0;
    bool haveBase = false;
    std::vector<double> window;

    Packet packet;
    while (client.ReadPacket(packet)) {
        const auto& header = packet.header;

        if (header.type == PacketType::Config) {
            haveConfig = true;
            std::printf("CONFIG %zu bytes\n", packet.payload.size());
            if (outFile) std::fwrite(packet.payload.data(), 1, packet.payload.size(), outFile);

            // A new encoder session restarts both the timestamp clock and the
            // frame counter, so neither baseline survives it.
            haveBase = false;
            haveSeq = false;
            window.clear();

            if (!opt.noDecode) {
                const std::string codec = opt.codec.empty() ? "h264" : opt.codec;
                if (!decoder.Open(codec, device) ||
                    !decoder.SetCodecConfig(packet.payload.data(), packet.payload.size())) {
                    std::printf("decoder setup failed: %s\n", decoder.LastError().c_str());
                    opt.noDecode = true;
                }
            }
        } else if (packet.IsVideo()) {
            if (!haveConfig) continue;

            if (haveSeq && header.seq != expectedSeq) {
                ++gaps;
                client.SendControl(MakeSimpleCommand("idr"));
            }
            expectedSeq = header.seq + 1;
            haveSeq = true;

            const double arrival = NowSeconds();
            const double delta = arrival * 1000.0 - static_cast<double>(header.ptsUs) / 1000.0;
            if (!haveBase || delta < latencyBase) { latencyBase = delta; haveBase = true; }
            window.push_back(delta - latencyBase);

            ++frames;
            totalBytes += packet.payload.size();

            // Start the take once video is actually arriving, so the frame
            // counts either side of it are comparable.
            //
            // With a ring armed, wait for it to fill first and then start. The
            // whole claim being tested is that the file is longer than the take:
            // arm five seconds, wait eight, record three, and the result should
            // be about eight seconds long rather than three.
            const bool ready = opt.preRoll <= 0 ||
                               NowSeconds() - started >= opt.preRoll + 2.0;
            if ((opt.record || opt.preRoll > 0) && !recordRequested && ready) {
                recordRequested = true;
                if (opt.preRoll > 0) {
                    std::printf("  ring should hold ~%ds; starting the take now\n",
                                opt.preRoll);
                }
                client.SendControl(MakeRecordCommand("start"));
            }
            if (outFile) std::fwrite(packet.payload.data(), 1, packet.payload.size(), outFile);

            if (!opt.noDecode && decoder.IsOpen()) {
                const bool ok = decoder.Decode(
                    packet.payload.data(), packet.payload.size(), header.ptsUs,
                               [&](const DecodedFrame& frame) {
                                   ++decoded;
                                   if (decoded == 1) {
                                       std::printf("first decoded frame: %ux%u %s\n",
                                                   frame.width, frame.height,
                                                   frame.texture ? "on GPU (D3D11 texture)"
                                                                 : "in system memory");
                                   }
                               });
                if (!ok && !reportedDecodeError) {
                    reportedDecodeError = true;
                    std::printf("decode failed: %s\n", decoder.LastError().c_str());
                }
            }
        } else if (header.type == PacketType::Record) {
            if (header.flags & kFlagCodecConfig) {
                const std::string dir = Mp4Writer::DefaultDirectory();
                const std::string path =
                    dir.empty() ? Mp4Writer::TimestampedName()
                                : dir + "\\" + Mp4Writer::TimestampedName();
                if (recording.Open(path, recordCodec,
                                   static_cast<uint32_t>(recordWidth),
                                   static_cast<uint32_t>(recordHeight),
                                   static_cast<uint32_t>(recordFps),
                                   packet.payload.data(), packet.payload.size(),
                                   audioAsc.data(), audioAsc.size(),
                                   48000, 2)) {
                    std::printf("recording to %s (%dx%d@%d %s, audio %s)\n",
                                path.c_str(), recordWidth, recordHeight, recordFps,
                                recordCodec.c_str(), recording.HasAudio() ? "yes" : "no");
                } else {
                    std::printf("could not start the recording: %s\n",
                                recording.LastError().c_str());
                }
            } else if (recording.IsOpen()) {
                recording.WriteVideo(packet.payload.data(), packet.payload.size(),
                                     header.ptsUs, (header.flags & kFlagKeyFrame) != 0);
            }
        } else if (header.type == PacketType::Audio) {
            if (header.flags & kFlagCodecConfig) {
                haveAudioConfig = true;
                audioAsc = packet.payload;
                // Five bits of object type, four of sample-rate index, four of
                // channel configuration -- the whole of what a decoder needs.
                if (packet.payload.size() >= 2) {
                    const uint8_t b0 = packet.payload[0];
                    const uint8_t b1 = packet.payload[1];
                    ascObjectType = b0 >> 3;
                    ascRateIndex = ((b0 & 0x07) << 1) | (b1 >> 7);
                    ascChannels = (b1 >> 3) & 0x0F;
                }
                std::printf("AUDIO config %zu bytes: object type %d, rate index %d, "
                            "%d channels\n", packet.payload.size(),
                            ascObjectType, ascRateIndex, ascChannels);
            } else {
                ++audioPackets;
                audioBytes += packet.payload.size();

                if (recording.IsOpen()) {
                    recording.WriteAudio(packet.payload.data(), packet.payload.size(),
                                         header.ptsUs);
                }
                if (audioFile) {
                    // ADTS: seven bytes that turn a raw access unit into
                    // something any player will open. The wire format carries no
                    // headers because the receiver already knows the
                    // configuration; a file on disk has no such luxury.
                    const size_t total = packet.payload.size() + 7;
                    const uint8_t adts[7] = {
                        0xFF,
                        0xF1,
                        static_cast<uint8_t>(((ascObjectType - 1) << 6) |
                                             (ascRateIndex << 2) | (ascChannels >> 2)),
                        static_cast<uint8_t>(((ascChannels & 3) << 6) | (total >> 11)),
                        static_cast<uint8_t>((total >> 3) & 0xFF),
                        static_cast<uint8_t>(((total & 7) << 5) | 0x1F),
                        0xFC,
                    };
                    std::fwrite(adts, 1, sizeof(adts), audioFile);
                    std::fwrite(packet.payload.data(), 1, packet.payload.size(), audioFile);
                }
            }
        } else if (header.type == PacketType::Stats) {
            const std::string payload = packet.PayloadAsString();
            ParseStats(payload, lastStats);
            std::printf("  phone: %s\n", payload.c_str());
        } else if (header.type == PacketType::Ack) {
            const std::string payload = packet.PayloadAsString();
            std::printf("  ack:   %s\n", payload.c_str());

            Ack ack;
            if (ParseAck(payload, ack) && ack.cmd == "record") {
                ReadAppliedString(ack.appliedJson, "file", recordFile);
                ReadAppliedString(ack.appliedJson, "codec", recordCodec);
                double value = 0;
                if (ReadAppliedNumber(ack.appliedJson, "width", value)) {
                    recordWidth = static_cast<int>(value);
                }
                if (ReadAppliedNumber(ack.appliedJson, "height", value)) {
                    recordHeight = static_cast<int>(value);
                }
                if (ReadAppliedNumber(ack.appliedJson, "fps", value)) {
                    recordFps = static_cast<int>(value);
                }
                if (ack.appliedJson.find("\"state\":\"recording\"") == std::string::npos &&
                    recording.IsOpen()) {
                    const std::string path = recording.Path();
                    const double megabytes = static_cast<double>(recording.Bytes()) / 1e6;
                    const uint64_t ms = recording.DurationMs();
                    recording.Close();
                    std::printf("recorded %llums, %.0f MB -> %s\n",
                                static_cast<unsigned long long>(ms), megabytes,
                                path.c_str());
                }
            }
        }

        const double now = NowSeconds();
        if (now - lastReport >= 1.0) {
            const double elapsed = now - started;
            std::sort(window.begin(), window.end());
            const double median = window.empty() ? 0.0 : window[window.size() / 2];
            const double worst = window.empty() ? 0.0 : window.back();

            std::printf("  local: %llu frames (%llu decoded), %5.1f fps, %6.1f Mbps, "
                        "%d gaps, queue %5.1fms med / %6.1fms max\n",
                        static_cast<unsigned long long>(frames),
                        static_cast<unsigned long long>(decoded),
                        frames / elapsed,
                        static_cast<double>(totalBytes) * 8.0 / elapsed / 1e6,
                        gaps, median, worst);
            window.clear();
            lastReport = now;
        }

        if (opt.seconds > 0 && now - started >= opt.seconds) break;
    }

    if (!client.LastError().empty()) std::printf("stream ended: %s\n", client.LastError().c_str());
    if (recordRequested) {
        // Stop the take before the session, and wait for the closing ACK: the
        // duration and byte count only exist there, and dropping the connection
        // first would leave the file open on the phone.
        client.SendControl(MakeRecordCommand("stop"));
        const double deadline = NowSeconds() + 5.0;
        while (NowSeconds() < deadline && client.ReadPacket(packet)) {
            if (packet.header.type != PacketType::Ack) continue;
            const std::string payload = packet.PayloadAsString();
            Ack ack;
            if (ParseAck(payload, ack) && ack.cmd == "record" &&
                ack.appliedJson.find("\"state\":\"idle\"") != std::string::npos) {
                std::printf("  ack:   %s\n", payload.c_str());
                break;
            }
        }
    }

    // Finalise whatever is still open. A file without its index has every byte
    // of footage in it and no player will touch it, so this runs on the way out
    // whether the take was stopped cleanly or the session simply ended.
    if (recording.IsOpen()) {
        const std::string path = recording.Path();
        const double megabytes = static_cast<double>(recording.Bytes()) / 1e6;
        const uint64_t ms = recording.DurationMs();
        recording.Close();
        std::printf("recorded %llums, %.0f MB -> %s\n",
                    static_cast<unsigned long long>(ms), megabytes, path.c_str());
    }

    client.SendControl(MakeSimpleCommand("stop"));

    const double elapsed = (std::max)(NowSeconds() - started, 1e-6);
    std::printf("\n%llu frames in %.1fs -> %.1f fps, %.1f MB, %.1f Mbps, %d gaps\n",
                static_cast<unsigned long long>(frames), elapsed, frames / elapsed,
                static_cast<double>(totalBytes) / 1e6,
                static_cast<double>(totalBytes) * 8.0 / elapsed / 1e6, gaps);
    std::printf("%llu frames decoded\n", static_cast<unsigned long long>(decoded));

    if (audioPackets > 0) {
        std::printf("audio: %llu frames, %.0f kB, %.0f kbit/s%s\n",
                    static_cast<unsigned long long>(audioPackets),
                    static_cast<double>(audioBytes) / 1000.0,
                    static_cast<double>(audioBytes) * 8.0 / elapsed / 1000.0,
                    haveAudioConfig ? "" : "  (no codec config -- undecodable)");
    } else {
        std::printf("audio: none\n");
    }

    if (!recordFile.empty()) {
        std::printf("\nrecorded to %s on the phone\n", recordFile.c_str());
        std::printf("collect it with: powershell tools\\pull-recordings.ps1\n");
    }

    if (audioFile) {
        std::fclose(audioFile);
        std::printf("audio written to %s\n", opt.audioOut.c_str());
    }
    if (outFile) std::fclose(outFile);
    if (device) device->Release();
    CoUninitialize();
    return 0;
}
