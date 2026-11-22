#!/bin/bash
# USB keyboard and mouse through qemu-xhci, a hub, a second controller,
# a tablet, and nec-usb-xhci. Also time a boot with no HID device.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SHOTS="${SHOTS:-/tmp/usb-shots}"
mkdir -p "$SHOTS"
PIDS=()
PORT=4600

cleanup() {
    local pid child
    for pid in "${PIDS[@]:-}"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            for child in $(pgrep -P "$pid" 2>/dev/null || true); do
                kill "$child" 2>/dev/null || true
            done
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
}
trap cleanup EXIT

mon() {
    python3 - "$1" "$2" << 'PY'
import socket, sys, time
path, cmd = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(2)
deadline = time.time() + 8
while True:
    try:
        s.connect(path)
        break
    except OSError:
        if time.time() > deadline:
            raise SystemExit("monitor connect failed")
        time.sleep(0.05)
s.settimeout(0.2)
try:
    while s.recv(4096):
        pass
except OSError:
    pass
s.settimeout(5)
s.sendall((cmd + "\n").encode())
buf = b""
end = time.time() + 8
while time.time() < end:
    try:
        chunk = s.recv(4096)
    except socket.timeout:
        if b"(qemu)" in buf:
            break
        continue
    if not chunk:
        break
    buf += chunk
    if buf.rstrip().endswith(b"(qemu)"):
        break
s.close()
PY
}

wait_log() {
    local log="$1" token="$2" secs="$3"
    local deadline=$((SECONDS + secs))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ -f "$log" ] && grep -q "$token" "$log"; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

serial_tee() {
    local port="$1" log="$2"
    python3 - "$port" "$log" << 'PY' &
import socket, sys, time
port, path = int(sys.argv[1]), sys.argv[2]
deadline = time.time() + 20
s = None
while time.time() < deadline:
    try:
        s = socket.create_connection(("127.0.0.1", port), 1)
        break
    except OSError:
        time.sleep(0.05)
if not s:
    raise SystemExit("serial connect failed")
s.settimeout(None)
with open(path, "wb", buffering=0) as out:
    while True:
        data = s.recv(4096)
        if not data:
            break
        out.write(data)
PY
    PIDS+=("$!")
}

IMG="$SHOTS/stick.img"
rm -f "$IMG"
mkfs.fat -F 32 -C "$IMG" 65536 --mbr=y >/dev/null
mmd -i "$IMG" ::/EFI ::/EFI/BOOT
mcopy -i "$IMG" "$ROOT/build/bootx64.efi" ::/EFI/BOOT/BOOTX64.EFI
CODE=/usr/share/OVMF/OVMF_CODE_4M.fd

boot() {
    local name="$1"
    shift
    local log="$SHOTS/$name.log"
    local port=$PORT
    PORT=$((PORT + 1))
    rm -f "$log" "$SHOTS/$name.sock" "$SHOTS/$name.png" "$SHOTS/$name.ppm"
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$SHOTS/vars-$name.fd"
    timeout --foreground 100 qemu-system-x86_64 -machine q35 -cpu EPYC -m 4G -vga std \
        -drive if=pflash,format=raw,readonly=on,file="$CODE" \
        -drive if=pflash,format=raw,file="$SHOTS/vars-$name.fd" \
        "$@" \
        -serial "tcp:127.0.0.1:${port},server=on,wait=off" \
        -monitor "unix:$SHOTS/$name.sock,server,nowait" -display none &
    local pid=$!
    PIDS+=("$pid")
    serial_tee "$port" "$log"
    echo "RUN $name pid $pid"
    if ! wait_log "$log" "SETUP: WELCOME" 70; then
        echo "NO WELCOME $name"
        tail -25 "$log" || true
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        return 1
    fi
    mon "$SHOTS/$name.sock" "sendkey ret"
    if ! wait_log "$log" "SETUP: NAME" 15; then
        echo "NO NAME $name"
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        return 1
    fi
    mon "$SHOTS/$name.sock" "sendkey backspace"
    mon "$SHOTS/$name.sock" "sendkey backspace"
    mon "$SHOTS/$name.sock" "sendkey backspace"
    mon "$SHOTS/$name.sock" "sendkey backspace"
    mon "$SHOTS/$name.sock" "sendkey a"
    mon "$SHOTS/$name.sock" "sendkey b"
    mon "$SHOTS/$name.sock" "sendkey c"
    mon "$SHOTS/$name.sock" "mouse_move 40 20"
    mon "$SHOTS/$name.sock" "mouse_move 40 20"
    mon "$SHOTS/$name.sock" "mouse_move 40 30"
    sleep 0.3
    mon "$SHOTS/$name.sock" "screendump $SHOTS/$name.ppm" || echo "shot failed $name"
    if [ -f "$SHOTS/$name.ppm" ]; then
        ffmpeg -y -loglevel error -i "$SHOTS/$name.ppm" "$SHOTS/$name.png" || true
    fi
    mon "$SHOTS/$name.sock" "mouse_button 1"
    sleep 0.2
    mon "$SHOTS/$name.sock" "mouse_button 0"
    sleep 0.4
    if grep -q "SETUP: PASS" "$log"; then
        echo "CLICK $name"
    else
        echo "NO CLICK $name"
    fi
    echo "OK $name"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

STICK=(-drive if=none,id=stick,format=raw,file="$IMG")

boot plain \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-mouse,bus=xhci.0 \
    "${STICK[@]}" \
    -device usb-storage,drive=stick,bus=xhci.0,bootindex=0

boot hub \
    -device qemu-xhci,id=xhci \
    -device usb-hub,bus=xhci.0,port=1 \
    -device usb-kbd,bus=xhci.0,port=1.1 \
    -device usb-mouse,bus=xhci.0,port=1.2 \
    "${STICK[@]}" \
    -device usb-storage,drive=stick,bus=xhci.0,port=2,bootindex=0

boot two \
    -device qemu-xhci,id=xhci0 \
    -device qemu-xhci,id=xhci1 \
    "${STICK[@]}" \
    -device usb-storage,drive=stick,bus=xhci0.0,bootindex=0 \
    -device usb-kbd,bus=xhci1.0 \
    -device usb-mouse,bus=xhci1.0

boot tablet \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-tablet,bus=xhci.0 \
    "${STICK[@]}" \
    -device usb-storage,drive=stick,bus=xhci.0,bootindex=0

boot nec \
    -device nec-usb-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-mouse,bus=xhci.0 \
    "${STICK[@]}" \
    -device usb-storage,drive=stick,bus=xhci.0,bootindex=0

# No HID device: init time only.
name=empty
log="$SHOTS/empty.log"
port=$PORT
rm -f "$log"
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$SHOTS/vars-empty.fd"
timeout --foreground 80 qemu-system-x86_64 -machine q35 -cpu EPYC -m 4G -vga std \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file="$SHOTS/vars-empty.fd" \
    -device qemu-xhci,id=xhci \
    -drive if=none,id=stick,format=raw,file="$IMG" \
    -device usb-storage,drive=stick,bus=xhci.0,bootindex=0 \
    -serial "tcp:127.0.0.1:${port},server=on,wait=off" -display none &
pid=$!
PIDS+=("$pid")
serial_tee "$port" "$log"
echo "RUN empty pid $pid"
if wait_log "$log" "XHCI: init ms" 70; then
    echo "EMPTY timed"
else
    echo "EMPTY no timing"
    tail -15 "$log" || true
fi
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

echo "SHOTS:"
ls -l "$SHOTS"/*.png 2>/dev/null || true
