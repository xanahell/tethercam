#!/usr/bin/env bash
# End to end check of usbcam-recv-c without a phone. Needs cmake, a C compiler,
# python3, ffmpeg (with libx265) and ffprobe.
#
# A fake usbmuxd listens on loopback TCP, the same transport Windows uses
# (Apple Mobile Device Service, 127.0.0.1:27015), and the receiver reaches it
# through USBMUXD_SOCKET_ADDRESS. Behind it a fake iPhone serves real HEVC.
# Checks: device list filtering, the handshake, a clean decode of the dump, the
# dump matching the source frame for frame, and the error paths.
#
#   bash tools/recv-c/integration.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="$(mktemp -d)"
FAKE_PID=""
FAILS=0

stop_fake() {
    if [ -n "$FAKE_PID" ]; then
        kill "$FAKE_PID" 2>/dev/null || true
        wait "$FAKE_PID" 2>/dev/null || true
        FAKE_PID=""
    fi
}
cleanup() {
    stop_fake
    rm -rf "$WORK"
}
trap cleanup EXIT

pass() { echo "  ok    $1"; }
fail() { echo "  FAIL  $1"; FAILS=$((FAILS + 1)); }

# expect DESCRIPTION COMMAND...   passes when the command succeeds
expect() {
    local desc="$1"
    shift
    if "$@"; then pass "$desc"; else fail "$desc"; fi
}

for tool in cmake python3 ffmpeg ffprobe; do
    command -v "$tool" > /dev/null || { echo "missing tool: $tool"; exit 1; }
done

# Value of KEY=VALUE from an ffprobe default-format file; empty when absent.
probe_value() {
    local line
    line="$(grep "^$2=" "$1" || true)"
    echo "${line#*=}"
}

# N from the receiver's "VIDEO   frames=N ..." summary line; 0 when absent.
frames_of() {
    local v
    v="$(sed -n 's/^VIDEO   frames=\([0-9][0-9]*\).*/\1/p' "$1" | tail -n 1)"
    echo "${v:-0}"
}

absent() { ! grep -q "$@"; }   # absent PATTERN FILE
same_file() { [ -s "$1" ] && cmp -s "$1" "$2"; }

start_fake() { # SCENARIO
    rm -f "$WORK/port"
    python3 -I "$ROOT/tools/scripts/fake_usbmuxd.py" --hevc "$WORK/in.hevc" \
        --portfile "$WORK/port" --scenario "$1" > /dev/null 2> "$WORK/fake-$1.log" &
    FAKE_PID=$!
    local i
    for i in $(seq 1 100); do
        [ -s "$WORK/port" ] && break
        sleep 0.1
    done
    if [ ! -s "$WORK/port" ]; then
        echo "fake usbmuxd did not start:"
        cat "$WORK/fake-$1.log"
        exit 1
    fi
    USBMUXD_SOCKET_ADDRESS="127.0.0.1:$(cat "$WORK/port")"
    export USBMUXD_SOCKET_ADDRESS
}

echo "== build"
cmake -S "$HERE" -B "$WORK/build" > "$WORK/cmake.log" 2>&1 || { cat "$WORK/cmake.log"; exit 1; }
cmake --build "$WORK/build" > "$WORK/make.log" 2>&1 || { cat "$WORK/make.log"; exit 1; }
RECV="$WORK/build/usbcam-recv-c"
pass "usbcam-recv-c builds with warnings as errors"

echo "== test stream"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -t 4 \
    -c:v libx265 -preset ultrafast -pix_fmt yuv420p \
    -x265-params "bframes=0:keyint=30:min-keyint=30:repeat-headers=1:log-level=error" \
    -f hevc "$WORK/in.hevc"
pass "encoded 4 s of 1280x720 HEVC"

echo "== scenario: protocol 1.2 phone"
start_fake ok

rc=0
"$RECV" --list > "$WORK/list.out" 2> "$WORK/list.err" || rc=$?
expect "--list exits 0" [ "$rc" -eq 0 ]
devices="$(grep -c '^DeviceID' "$WORK/list.out" || true)"
expect "--list shows the USB device once, the Network twin is filtered" [ "$devices" = "1" ]

rc=0
"$RECV" --seconds 3 --dump "$WORK/out.hevc" > "$WORK/recv.out" 2> "$WORK/recv.err" || rc=$?
expect "capture exits 0" [ "$rc" -eq 0 ]
n="$(frames_of "$WORK/recv.out")"
expect "at least 80 frames in 3 s (got $n)" [ "$n" -ge 80 ]
expect "CONFIG parsed" grep -q '^CONFIG  1280x720@30' "$WORK/recv.out"
expect "STOP sent at the end" grep -q '^STOP    sent' "$WORK/recv.out"

ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=codec_name,width,height,nb_read_frames -of default=nw=1 \
    "$WORK/out.hevc" > "$WORK/probe.out" 2>&1 || true
size="$(probe_value "$WORK/probe.out" width)x$(probe_value "$WORK/probe.out" height)"
expect "dump is HEVC" [ "$(probe_value "$WORK/probe.out" codec_name)" = "hevc" ]
expect "dump is 1280x720" [ "$size" = "1280x720" ]
expect "dump holds every received frame" [ "$(probe_value "$WORK/probe.out" nb_read_frames)" = "$n" ]

ffmpeg -v error -i "$WORK/out.hevc" -f null - 2> "$WORK/decode.err" || true
expect "dump decodes without errors" [ ! -s "$WORK/decode.err" ]

ffmpeg -v error -i "$WORK/in.hevc" -frames:v 60 -f md5 - > "$WORK/src.md5" 2>&1 || true
ffmpeg -v error -i "$WORK/out.hevc" -frames:v 60 -f md5 - > "$WORK/dump.md5" 2>&1 || true
expect "first 60 decoded frames match the source" same_file "$WORK/src.md5" "$WORK/dump.md5"

expect "phone received CLIENT_INFO" grep -q 'CLIENT_INFO kind=3' "$WORK/fake-ok.log"
expect "phone received the 11-byte START" grep -q 'START .* len=11 flags=0' "$WORK/fake-ok.log"
expect "no protocol violation seen by the phone" absent '^VIOLATION' "$WORK/fake-ok.log"

echo "== scenario: refused port"
rc=0
"$RECV" --port 9999 > "$WORK/refused.out" 2> "$WORK/refused.err" || rc=$?
expect "wrong port exits 1" [ "$rc" -eq 1 ]
expect "reports usbmuxd Number 3 with a hint" grep -q 'Number 3' "$WORK/refused.err"
stop_fake

echo "== scenario: phone busy"
start_fake busy
rc=0
"$RECV" --seconds 2 > "$WORK/busy.out" 2> "$WORK/busy.err" || rc=$?
expect "BUSY exits 1" [ "$rc" -eq 1 ]
expect "reports ERROR 1 BUSY" grep -q 'ERROR 1 from the phone' "$WORK/busy.err"
stop_fake

echo "== scenario: protocol 1.0 phone"
start_fake no-client-info
rc=0
"$RECV" --seconds 2 > "$WORK/old.out" 2> "$WORK/old.err" || rc=$?
expect "capture from a 1.0 phone exits 0" [ "$rc" -eq 0 ]
expect "no CLIENT_INFO sent to a 1.0 phone" absent 'CLIENT_INFO' "$WORK/fake-no-client-info.log"
expect "no protocol violation seen by the 1.0 phone" absent '^VIOLATION' \
    "$WORK/fake-no-client-info.log"
stop_fake

echo "== scenario: no usbmuxd"
rc=0
USBMUXD_SOCKET_ADDRESS="127.0.0.1:1" "$RECV" --list > "$WORK/none.out" 2> "$WORK/none.err" || rc=$?
expect "unreachable usbmuxd exits 1" [ "$rc" -eq 1 ]
expect "says so" grep -q 'cannot query usbmuxd' "$WORK/none.err"

echo
if [ "$FAILS" -eq 0 ]; then
    echo "all checks passed"
else
    echo "$FAILS check(s) failed"
    exit 1
fi
