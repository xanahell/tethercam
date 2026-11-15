# usbcam-recv-c

A portable IUCM receiver in C11 for **Windows, macOS and Linux**. It finds the
iPhone through usbmuxd, runs the handshake of [`protocol/PROTOCOL.md`](../../protocol/PROTOCOL.md)
and writes the HEVC stream to a file as Annex-B. It is the C counterpart of
[`../scripts/usbmux_recv.py`](../scripts/usbmux_recv.py), built only on [`shared/`](../../shared)
(MIT), and the first step towards a Windows 11 receiver: it proves the link to the
phone before any decoder or virtual camera is written.

## Windows

The phone is reached through **Apple Mobile Device Service**, which serves usbmuxd as a
loopback TCP service on `127.0.0.1:27015`. It is installed with **iTunes** or the
**Apple Devices** app from the Microsoft Store; without one of them there is no
usbmuxd on Windows. Plug the phone in, tap **Trust**, open TetherCam on it and leave it in
the foreground.

Build with Visual Studio 2022 (C++ workload) and CMake, from a Developer PowerShell:

```powershell
cmake -S tools/recv-c -B tools/recv-c/build
cmake --build tools/recv-c/build --config Release
tools\recv-c\build\Release\usbcam-recv-c.exe --list
tools\recv-c\build\Release\usbcam-recv-c.exe --seconds 5 --dump out.hevc
```

**Status:** the Windows path of `shared/usbmux.c` (Winsock, `WSAPoll`, one-time
`WSAStartup`) has not been compiled or run on a real Windows machine yet. Everything
else, including the loopback TCP transport it uses, is covered by
[`integration.sh`](integration.sh) on Linux.

## Usage

```
usbcam-recv-c --list
usbcam-recv-c [--serial UDID] [--port 7878] [--seconds 5] [--dump out.hevc]
              [--size 1920x1080] [--fps 30] [--bitrate 12000] [--camera 0]
              [--tcp HOST:PORT] [--timeout 5]
```

| Option | Meaning |
|---|---|
| `--list` | List the attached USB devices and exit. The Network twin of a phone is filtered out |
| `--serial` | Pick a device by serial number; default is the first USB device |
| `--dump` | Write the video as Annex-B HEVC (parameter sets in front of every keyframe) |
| `--tcp HOST:PORT` | Skip usbmuxd and connect to a sender directly, e.g. `usbcam-sim` |
| `--seconds` | How long to capture once the first CONFIG has arrived |

The exit code is 0 when frames arrived, 1 on a protocol or link error, 2 on a usage error.
Check a dump with `ffprobe out.hevc` or play it with `ffplay out.hevc`.

### Pointing it at another usbmuxd

`USBMUXD_SOCKET_ADDRESS` overrides where the receiver looks for usbmuxd, with the same
convention as libimobiledevice: `HOST:PORT`, or `UNIX:/path` on macOS and Linux.

## Test without a phone

```sh
bash tools/recv-c/integration.sh
```

Needs cmake, a C compiler, python3 and ffmpeg with libx265. It starts
[`../scripts/fake_usbmuxd.py`](../scripts/fake_usbmuxd.py), a fake usbmuxd on loopback TCP
with a fake iPhone behind it that serves real HEVC, and checks that the dump decodes
cleanly and matches the source frame for frame. It also covers the error paths: wrong
port, a busy phone, a 1.0 phone and an unreachable usbmuxd.
