#!/bin/bash
# Prove the polled 8042 init types in QEMU, and that a missing 8042 does not hang.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SHOTS="${SHOTS:-/tmp/ps2-shots}"
mkdir -p "$SHOTS"
PIDS=()

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

shot() {
    local name="$1"
    local ppm="$SHOTS/$name.ppm" png="$SHOTS/$name.png"
    mon "$SHOTS/$name.sock" "screendump $ppm" || echo "screendump failed $name"
    if [ -f "$ppm" ]; then
        ffmpeg -y -loglevel error -i "$ppm" "$png" || echo "png failed $name"
    fi
}

IMG="$SHOTS/stick.img"
rm -f "$IMG"
mkfs.fat -F 32 -C "$IMG" 65536 --mbr=y >/dev/null
mmd -i "$IMG" ::/EFI ::/EFI/BOOT
mcopy -i "$IMG" "$ROOT/build/bootx64.efi" ::/EFI/BOOT/BOOTX64.EFI
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$SHOTS/vars.fd"
CODE=/usr/share/OVMF/OVMF_CODE_4M.fd

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

run() {
    local name="$1" port="$2"
    shift 2
    local log="$SHOTS/$name.log"
    rm -f "$log" "$SHOTS/$name.sock" "$SHOTS/$name.ppm" "$SHOTS/$name.png"
    timeout --foreground 100 "$@" \
        -serial "tcp:127.0.0.1:${port},server=on,wait=off" \
        -monitor "unix:$SHOTS/$name.sock,server,nowait" -display none &
    local pid=$!
    PIDS+=("$pid")
    serial_tee "$port" "$log"
    echo "RUN $name pid $pid" >&2
    LAST_PID=$pid
}

# PS/2 keyboard on q35 + OVMF. No USB keyboard, so sendkey hits the 8042.
run type 4545 qemu-system-x86_64 -machine q35 -cpu EPYC -m 4G -vga std \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file="$SHOTS/vars.fd" \
    -device qemu-xhci,id=xhci \
    -drive if=none,id=stick,format=raw,file="$IMG" \
    -device usb-storage,drive=stick,bus=xhci.0,bootindex=0
pid=$LAST_PID
if wait_log "$SHOTS/type.log" "SETUP: WELCOME" 70; then
    mon "$SHOTS/type.sock" "sendkey ret"
    if wait_log "$SHOTS/type.log" "SETUP: NAME" 15; then
        mon "$SHOTS/type.sock" "sendkey backspace"
        mon "$SHOTS/type.sock" "sendkey backspace"
        mon "$SHOTS/type.sock" "sendkey backspace"
        mon "$SHOTS/type.sock" "sendkey backspace"
        mon "$SHOTS/type.sock" "sendkey a"
        mon "$SHOTS/type.sock" "sendkey b"
        mon "$SHOTS/type.sock" "sendkey c"
        sleep 0.4
        shot type
        echo "TYPE reached name"
    else
        echo "TYPE no name field"
        tail -30 "$SHOTS/type.log"
    fi
else
    echo "TYPE no welcome"
    tail -30 "$SHOTS/type.log"
fi
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

# Missing 8042 must give up quickly and still reach Setup, then the DIAG page.
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$SHOTS/vars-off.fd"
run off 4546 qemu-system-x86_64 -machine q35,i8042=off -cpu EPYC -m 4G -vga std \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file="$SHOTS/vars-off.fd" \
    -device qemu-xhci,id=xhci \
    -drive if=none,id=stick,format=raw,file="$IMG" \
    -device usb-storage,drive=stick,bus=xhci.0,bootindex=0
pid=$LAST_PID
if wait_log "$SHOTS/off.log" "DIAG: SHOW" 70; then
    sleep 0.5
    shot off
    echo "OFF reached diag"
else
    echo "OFF no diag"
    tail -30 "$SHOTS/off.log"
fi
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

# Legacy BIOS, Pentium III, still types.
run bios 4547 qemu-system-i386 -machine pc -cpu pentium3 -m 128 -vga std \
    -cdrom "$ROOT/dist/GeometricalOS.iso" -boot order=d
pid=$LAST_PID
if wait_log "$SHOTS/bios.log" "SETUP: WELCOME" 40; then
    mon "$SHOTS/bios.sock" "sendkey ret"
    if wait_log "$SHOTS/bios.log" "SETUP: NAME" 15; then
        mon "$SHOTS/bios.sock" "sendkey backspace"
        mon "$SHOTS/bios.sock" "sendkey backspace"
        mon "$SHOTS/bios.sock" "sendkey backspace"
        mon "$SHOTS/bios.sock" "sendkey backspace"
        mon "$SHOTS/bios.sock" "sendkey a"
        mon "$SHOTS/bios.sock" "sendkey b"
        mon "$SHOTS/bios.sock" "sendkey c"
        sleep 0.4
        shot bios
        echo "BIOS reached name"
    else
        echo "BIOS no name field"
        tail -20 "$SHOTS/bios.log"
    fi
else
    echo "BIOS no welcome"
    tail -20 "$SHOTS/bios.log"
fi
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

echo "SHOTS:"
ls -l "$SHOTS"/*.png 2>/dev/null || true
