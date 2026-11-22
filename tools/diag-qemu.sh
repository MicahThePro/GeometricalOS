#!/bin/bash
# Boot the DIAG screen under OVMF and legacy BIOS. Each QEMU is under timeout
# and is killed by PID if the script exits early.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SHOTS="${SHOTS:-/tmp/diag-shots}"
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

ppm_to_png() {
    local ppm="$1" png="$2"
    ffmpeg -y -loglevel error -i "$ppm" "$png"
}

screendump() {
    local mon="$1" ppm="$2"
    python3 - "$mon" "$ppm" << 'PY'
import socket, sys, time
path, ppm = sys.argv[1], sys.argv[2]
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
s.settimeout(3)
s.sendall(("screendump %s\n" % ppm).encode())
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
print(buf.decode("utf-8", "replace")[-200:])
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

make_stick() {
    local img="$ROOT/build/stick.img"
    rm -f "$img"
    mkfs.fat -F 32 -C "$img" 65536 --mbr=y >/dev/null
    mmd -i "$img" ::/EFI ::/EFI/BOOT
    mcopy -i "$img" "$ROOT/build/bootx64.efi" ::/EFI/BOOT/BOOTX64.EFI
    echo "$img"
}

run_qemu() {
    local name="$1"
    shift
    local log="/tmp/diag-$name.log"
    local mon="/tmp/diag-$name.sock"
    local ppm="$SHOTS/$name.ppm"
    local png="$SHOTS/$name.png"
    rm -f "$log" "$mon" "$ppm" "$png"
    timeout --foreground 100 "$@" -serial "file:$log" -monitor "unix:$mon,server,nowait" -display none &
    local pid=$!
    PIDS+=("$pid")
    echo "RUN $name pid $pid"
    if wait_log "$log" "DIAG: SHOW" 80; then
        sleep 1
        screendump "$mon" "$ppm" || echo "screendump failed for $name"
        if [ -f "$ppm" ]; then
            ppm_to_png "$ppm" "$png" || echo "png convert failed for $name"
        fi
        echo "DIAG ok $name"
    else
        echo "DIAG missing $name"
        echo "----- log tail -----"
        tail -40 "$log" 2>/dev/null || true
    fi
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
STICK="$(make_stick)"

common_fw() {
    local vars="$1"
    cp "$VARS_SRC" "$vars"
    echo -drive "if=pflash,format=raw,readonly=on,file=$CODE" \
         -drive "if=pflash,format=raw,file=$vars"
}

# The caller expands firmware drives. Each function builds its argv.
vars_a="$SHOTS/vars-a.fd"
vars_b="$SHOTS/vars-b.fd"
vars_c="$SHOTS/vars-c.fd"
cp "$VARS_SRC" "$vars_a"
cp "$VARS_SRC" "$vars_b"
cp "$VARS_SRC" "$vars_c"

run_qemu a qemu-system-x86_64 -machine q35 -cpu EPYC -m 4G -vga std \
    -drive "if=pflash,format=raw,readonly=on,file=$CODE" \
    -drive "if=pflash,format=raw,file=$vars_a" \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-mouse,bus=xhci.0 \
    -drive "if=none,id=stick,format=raw,file=$STICK" \
    -device usb-storage,drive=stick,bus=xhci.0,bootindex=0

run_qemu b qemu-system-x86_64 -machine q35 -cpu EPYC -m 6G -vga std \
    -drive "if=pflash,format=raw,readonly=on,file=$CODE" \
    -drive "if=pflash,format=raw,file=$vars_b" \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-mouse,bus=xhci.0 \
    -drive "if=none,id=stick,format=raw,file=$STICK" \
    -device usb-storage,drive=stick,bus=xhci.0,bootindex=0

run_qemu c qemu-system-x86_64 -machine q35 -cpu EPYC -m 4G -vga std \
    -drive "if=pflash,format=raw,readonly=on,file=$CODE" \
    -drive "if=pflash,format=raw,file=$vars_c" \
    -device qemu-xhci,id=xhci0 \
    -device qemu-xhci,id=xhci1 \
    -device usb-hub,bus=xhci1.0,port=1 \
    -device usb-kbd,bus=xhci1.0,port=1.1 \
    -device usb-mouse,bus=xhci1.0,port=1.2 \
    -drive "if=none,id=stick,format=raw,file=$STICK" \
    -device usb-storage,drive=stick,bus=xhci0.0,bootindex=0

run_qemu d qemu-system-i386 -machine pc -m 128 -vga std \
    -cdrom "$ROOT/dist/GeometricalOS.iso" -boot order=d

echo "SHOTS:"
ls -l "$SHOTS"/*.png 2>/dev/null || true
