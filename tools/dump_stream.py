#!/usr/bin/env python3
"""
Phase-1 verification client for XCam.

Connects to the phone through the adb tunnel, reads the handshake, optionally
reconfigures the pipeline, then writes the Annex-B elementary stream to a file or
straight into ffplay. This is the reference implementation of docs/PROTOCOL.md on
the receiving side -- the C++ client must agree with it byte for byte.

    adb forward tcp:27183 tcp:27183
    python tools/dump_stream.py --play
    python tools/dump_stream.py --out capture.h264 --width 1920 --height 1080 --fps 60
"""

import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time

PORT = 27183
MAGIC = 0x4D414358
HEADER = struct.Struct("<IBBHIQI")   # magic, type, flags, resv, len, ptsUs, seq
assert HEADER.size == 24

TYPE_CONFIG, TYPE_KEYFRAME, TYPE_DELTA = 1, 2, 3
TYPE_CONTROL, TYPE_STATS, TYPE_AUDIO, TYPE_ACK = 4, 5, 6, 7

TYPE_NAMES = {
    TYPE_CONFIG: "CONFIG", TYPE_KEYFRAME: "KEY", TYPE_DELTA: "DELTA",
    TYPE_CONTROL: "CONTROL", TYPE_STATS: "STATS", TYPE_AUDIO: "AUDIO", TYPE_ACK: "ACK",
}


def find_adb():
    """ANDROID_HOME first, then the default SDK location, then PATH."""
    candidates = []
    for env in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        root = os.environ.get(env)
        if root:
            candidates.append(os.path.join(root, "platform-tools", "adb.exe"))
    local = os.environ.get("LOCALAPPDATA")
    if local:
        candidates.append(os.path.join(local, "Android", "Sdk", "platform-tools", "adb.exe"))

    for path in candidates:
        if os.path.isfile(path):
            return path
    return shutil.which("adb")


def ensure_forward(port):
    """
    Re-establishes the adb tunnel if it is missing.

    Every time the phone re-enumerates on USB -- a cable nudge, a dropout, an adb
    server restart -- its forwards are silently discarded, and the only symptom is
    ConnectionRefused on localhost. Setting it up here costs one adb call and
    removes the single most common reason this script fails to connect.
    """
    adb = find_adb()
    if not adb:
        print("adb not found; set ANDROID_HOME or pass --no-adb", file=sys.stderr)
        return False

    try:
        listed = subprocess.run([adb, "forward", "--list"], capture_output=True,
                                text=True, timeout=15).stdout
        if f"tcp:{port} tcp:{port}" in listed:
            return True

        devices = subprocess.run([adb, "devices"], capture_output=True,
                                 text=True, timeout=15).stdout
        attached = [ln.split()[0] for ln in devices.splitlines()[1:]
                    if ln.strip() and ln.split()[-1] == "device"]
        if not attached:
            print("no device on adb -- check the cable and that USB debugging is on",
                  file=sys.stderr)
            return False

        subprocess.run([adb, "forward", f"tcp:{port}", f"tcp:{port}"],
                       capture_output=True, text=True, timeout=15, check=True)
        print(f"adb forward tcp:{port} -> {attached[0]}")
        return True
    except (subprocess.SubprocessError, OSError) as e:
        print(f"could not set up adb forward: {e}", file=sys.stderr)
        return False


def recv_exact(sock, n):
    """Reads exactly n bytes or raises -- short reads are the norm on TCP."""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError(f"connection closed with {n - len(buf)} bytes outstanding")
        buf += chunk
    return bytes(buf)


def read_handshake(sock):
    head = recv_exact(sock, 12)
    magic, version, _flags, json_len = struct.unpack("<4sHHI", head)
    if magic != b"XCAM":
        raise ValueError(f"bad handshake magic {magic!r}")
    payload = recv_exact(sock, json_len)
    return version, json.loads(payload.decode("utf-8"))


def read_packet(sock):
    magic, ptype, _flags, _resv, length, pts_us, seq = HEADER.unpack(recv_exact(sock, HEADER.size))
    if magic != MAGIC:
        raise ValueError(f"bad packet magic 0x{magic:08x}")
    payload = recv_exact(sock, length) if length else b""
    return ptype, seq, pts_us, payload


def send_control(sock, obj):
    payload = json.dumps(obj).encode("utf-8")
    sock.sendall(HEADER.pack(MAGIC, TYPE_CONTROL, 1, 0, len(payload), 0, 0) + payload)


def describe(info):
    print(f"  device : {info.get('deviceName')} (API {info.get('androidApi')})")
    print(f"  codecs : {', '.join(info.get('codecs', []))}")
    if info.get("recorder"):
        print(f"  records: {info.get('recordDir')}")
    for cam in info.get("cameras", []):
        res = cam.get("maxRes", [0, 0])
        zoom = cam.get("zoomRange", [1, 1])
        print(f"  camera {cam['id']:>2}  {cam.get('label','?'):<11} {cam.get('facing','?'):<8}"
              f" {res[0]}x{res[1]} @{cam.get('maxFps')}fps"
              f"  zoom {zoom[0]:.1f}-{zoom[1]:.1f}x"
              f"{'  [logical]' if cam.get('logical') else ''}")


def main():
    ap = argparse.ArgumentParser(description="XCam phase-1 stream dumper")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--out", help="write the elementary stream here")
    ap.add_argument("--play", action="store_true", help="pipe into ffplay instead")
    ap.add_argument("--seconds", type=float, default=0, help="stop after N seconds (0 = until Ctrl-C)")
    ap.add_argument("--camera")
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--fps", type=int)
    ap.add_argument("--bitrate", type=int, help="bits per second")
    ap.add_argument("--codec", choices=["h264", "hevc"])
    ap.add_argument("--info", action="store_true", help="print the handshake and exit")
    ap.add_argument("--record", action="store_true",
                    help="also record a full-quality take on the phone")
    ap.add_argument("--record-size", metavar="WxH",
                    help="recording size, e.g. 3840x2160 (default: the camera's best)")
    ap.add_argument("--record-codec", choices=["h264", "hevc"])
    ap.add_argument("--no-adb", action="store_true",
                    help="do not touch adb; assume the tunnel is already up")
    ap.add_argument("--hwaccel", default="d3d11va",
                    choices=["d3d11va", "cuda", "dxva2", "none"],
                    help="ffplay hardware decoder (default d3d11va)")
    ap.add_argument("--smooth", action="store_true",
                    help="keep ffplay's timestamp-based pacing; smoother but adds delay")
    args = ap.parse_args()

    # Progress lines are the whole point of this tool, and Python fully buffers
    # stdout the moment it is redirected or piped, which hides them until exit.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass

    if not args.no_adb and args.host in ("127.0.0.1", "localhost"):
        ensure_forward(args.port)

    try:
        sock = socket.create_connection((args.host, args.port), timeout=10)
    except ConnectionRefusedError:
        print(f"nothing is listening on {args.host}:{args.port}.", file=sys.stderr)
        print("  - is the XCam app open on the phone with capture started?", file=sys.stderr)
        print("  - does 'adb devices' list it?", file=sys.stderr)
        return 1

    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"connected to {args.host}:{args.port}")

    version, info = read_handshake(sock)
    print(f"protocol v{version}")
    describe(info)

    if args.info:
        sock.close()
        return 0

    overrides = {k: v for k, v in (
        ("camera", args.camera), ("width", args.width), ("height", args.height),
        ("fps", args.fps), ("bitrate", args.bitrate), ("codec", args.codec),
    ) if v is not None}
    if overrides:
        print(f"applying {overrides}")
        send_control(sock, {"cmd": "set", **overrides})

    if args.record and (args.record_size or args.record_codec):
        cfg = {"cmd": "record", "action": "config"}
        if args.record_size:
            cfg["width"], cfg["height"] = (int(v) for v in args.record_size.split("x"))
        if args.record_codec:
            cfg["codec"] = args.record_codec
        send_control(sock, cfg)

    sink, proc = None, None
    if args.play:
        ffplay = shutil.which("ffplay")
        if not ffplay:
            print("ffplay not on PATH", file=sys.stderr)
            return 1
        # -f is not optional here. Given a pipe with no container, ffplay has to
        # probe to identify the stream, but -fflags nobuffer starves the very
        # probe it depends on, so it sits there and never opens a window. Naming
        # the format skips probing entirely, which is also what makes
        # -probesize/-analyzeduration safe to pin to the minimum.
        fmt = args.codec or "h264"
        cmd = [ffplay, "-hide_banner", "-loglevel", "warning",
               "-f", fmt, "-probesize", "32", "-analyzeduration", "0",
               "-fflags", "nobuffer", "-flags", "low_delay", "-framedrop", "-an"]

        # Software-decoding 1080p60 at 50+ Mbit/s does not run in real time, and
        # ffplay falling behind is not a display problem: it stops draining the
        # pipe, which backs up through the socket into the phone's send queue.
        # Latency then climbs without bound -- measured here going past four
        # seconds inside fifteen. Hardware decode is what keeps it flat.
        if args.hwaccel != "none":
            cmd += ["-hwaccel", args.hwaccel]

        # ffplay schedules presentation off the stream's own timestamps, which
        # buys smoothness at the cost of a queue. For a live camera the newest
        # frame is the only one worth showing, so flatten the timestamps and let
        # it render on arrival.
        if not args.smooth:
            cmd += ["-vf", "setpts=0"]

        cmd += ["-window_title", "XCam preview", "-i", "pipe:0"]

        proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
        sink = proc.stdin
        print(f"ffplay started (pid {proc.pid}, {fmt}, hwaccel {args.hwaccel})")
    elif args.out:
        sink = open(args.out, "wb")
    else:
        print("nothing to do: pass --out or --play (or --info)", file=sys.stderr)
        return 1

    started = time.monotonic()
    frames = key_frames = total_bytes = 0
    expected_seq = None
    gaps = 0
    last_report = started
    have_config = False

    # Latency accounting. The phone's clock and ours are unrelated, so the
    # absolute difference between a frame's pts and its arrival is meaningless --
    # but the *variation* in that difference is not. The smallest value seen is
    # the best case the link achieved; everything above it is queueing delay
    # sitting somewhere between the encoder and this socket.
    lat_base = None
    lat_samples = []
    lat_window = []
    record_started = False

    try:
        sock.settimeout(5)
        while True:
            ptype, seq, pts_us, payload = read_packet(sock)

            if ptype == TYPE_CONFIG:
                have_config = True
                # New encoder session: the phone restarts both its timestamp
                # clock and its frame counter. A stale baseline would report the
                # age of the session as latency, and the counter returning to
                # zero as lost frames.
                lat_base = None
                lat_window = []
                expected_seq = None
                sink.write(payload)
                print(f"CONFIG {len(payload)} bytes -> {payload[:8].hex(' ')}")

            elif ptype in (TYPE_KEYFRAME, TYPE_DELTA):
                if not have_config:
                    continue        # nothing can decode this yet
                if expected_seq is not None and seq != expected_seq:
                    gaps += 1
                    # Loss means the phone shed frames; ask for a fresh IDR so the
                    # picture recovers now instead of at the next scheduled one.
                    send_control(sock, {"cmd": "idr"})
                expected_seq = seq + 1

                arrival = time.monotonic()
                delta_ms = arrival * 1000.0 - pts_us / 1000.0
                if lat_base is None or delta_ms < lat_base:
                    lat_base = delta_ms
                queued_ms = delta_ms - lat_base
                lat_samples.append(queued_ms)
                lat_window.append(queued_ms)

                sink.write(payload)
                frames += 1
                total_bytes += len(payload)

                # Start the take once video is arriving, so the frame counts
                # either side of it are comparable.
                if args.record and not record_started:
                    record_started = True
                    send_control(sock, {"cmd": "record", "action": "start"})
                if ptype == TYPE_KEYFRAME:
                    key_frames += 1

            elif ptype == TYPE_STATS:
                print(f"  phone: {payload.decode('utf-8')}")

            elif ptype == TYPE_ACK:
                print(f"  ack:   {payload.decode('utf-8')}")

            now = time.monotonic()
            if now - last_report >= 1.0:
                elapsed = now - started
                lat = ""
                if lat_window:
                    w = sorted(lat_window)
                    lat = (f", queue {w[len(w) // 2]:5.1f}ms med"
                           f" / {w[-1]:6.1f}ms max")
                    lat_window = []
                print(f"  local: {frames} frames ({key_frames} key), "
                      f"{frames / elapsed:5.1f} fps avg, "
                      f"{total_bytes * 8 / elapsed / 1e6:6.1f} Mbps, "
                      f"{gaps} gaps{lat}")
                last_report = now

            if args.seconds and now - started >= args.seconds:
                break

    except KeyboardInterrupt:
        print("\ninterrupted")
    except BrokenPipeError:
        print("ffplay exited -- run with --out instead to check the stream itself")
    except (ConnectionError, socket.timeout, ValueError) as e:
        # Distinguish "the phone hung up" from "the USB link went away", because
        # the fixes are completely different.
        print(f"stream ended: {e}")
        if frames and proc is None:
            print("  the phone closed the connection mid-stream; check that it is")
            print("  still on adb (adb devices) and that the app is in the foreground")
    finally:
        if record_started:
            # Close the take and wait for its ACK before hanging up: the
            # duration and byte count exist only there, and dropping the socket
            # first would leave the file open on the phone.
            try:
                send_control(sock, {"cmd": "record", "action": "stop"})
                sock.settimeout(5)
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    ptype, _seq, _pts, payload = read_packet(sock)
                    if ptype != TYPE_ACK:
                        continue
                    ack = json.loads(payload.decode("utf-8"))
                    if ack.get("cmd") == "record":
                        applied = ack.get("applied", {})
                        print(f"  recorded {applied.get('durationMs', 0) / 1000:.1f}s, "
                              f"{applied.get('bytes', 0) / 1e6:.0f} MB")
                        print(f"  -> {applied.get('file')}")
                        print("  collect it with: powershell tools/pull-recordings.ps1")
                        break
            except (OSError, ValueError, json.JSONDecodeError):
                pass

        try:
            send_control(sock, {"cmd": "stop"})
        except OSError:
            pass
        sock.close()
        if sink and sink is not sys.stdout.buffer:
            try:
                sink.close()
            except OSError:
                pass
        if proc:
            # On a timed run, closing stdin is not enough: ffplay keeps its window
            # open on the last frame and we would block here until someone closes
            # it by hand. Give it a moment to drain, then take it down.
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()

    elapsed = max(time.monotonic() - started, 1e-6)
    print(f"\n{frames} frames in {elapsed:.1f}s -> {frames / elapsed:.1f} fps, "
          f"{total_bytes / 1e6:.1f} MB, {total_bytes * 8 / elapsed / 1e6:.1f} Mbps, {gaps} gaps")

    if lat_samples:
        s = sorted(lat_samples)
        def pct(p):
            return s[min(len(s) - 1, int(len(s) * p))]
        print(f"capture-to-socket queueing above best case: "
              f"p50 {pct(0.50):.1f}ms  p90 {pct(0.90):.1f}ms  "
              f"p99 {pct(0.99):.1f}ms  max {s[-1]:.1f}ms")
        print("  (this covers phone encoder + send queue + USB only -- it does not")
        print("   include anything the player adds after we hand the frame over)")
    if args.out:
        print(f"wrote {args.out} -- play it with: ffplay {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
