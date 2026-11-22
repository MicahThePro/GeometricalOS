#!/bin/bash
# Acceptance boots for the UEFI reservation and handoff fixes.
# Each QEMU is under timeout and is killed by PID if the script exits early.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SHOTS="${SHOTS:-/tmp/uefi-shots}"
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
    local img="$SHOTS/stick64.img"
    if [ -f "$img" ]; then
        echo "$img"
        return
    fi
    rm -f "$img"
    truncate -s 64G "$img"
    parted -s "$img" mklabel gpt mkpart ESP fat32 1MiB 100%
    parted -s "$img" set 1 esp on
    mkfs.fat -F 32 --offset=2048 "$img" >/dev/null
    mmd -i "$img@@1048576" ::/EFI ::/EFI/BOOT
    mcopy -i "$img@@1048576" "$ROOT/build/bootx64.efi" ::/EFI/BOOT/BOOTX64.EFI
    echo "$img"
}

run_qemu() {
    local name="$1"
    local token="$2"
    shift 2
    local log="$SHOTS/$name.log"
    local mon="$SHOTS/$name.sock"
    local ppm="$SHOTS/$name.ppm"
    local png="$SHOTS/$name.png"
    rm -f "$log" "$mon" "$ppm" "$png"
    timeout --foreground 100 "$@" -serial "file:$log" -monitor "unix:$mon,server,nowait" -display none &
    local pid=$!
    PIDS+=("$pid")
    echo "RUN $name pid $pid"
    if wait_log "$log" "$token" 80; then
        sleep 1
        screendump "$mon" "$ppm" || echo "screendump failed for $name"
        if [ -f "$ppm" ]; then
            ffmpeg -y -loglevel error -i "$ppm" "$png" || echo "png convert failed for $name"
        fi
        echo "BOOT ok $name"
    else
        echo "BOOT missing $name"
        tail -30 "$log" 2>/dev/null || true
    fi
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
STICK="$(make_stick)"

uefi() {
    local name="$1" mem="$2" xhci_mode="$3"
    local vars="$SHOTS/vars-$name.fd"
    cp "$VARS_SRC" "$vars"
    local -a devs
    if [ "$xhci_mode" = "one" ]; then
        devs=(
            -device qemu-xhci,id=xhci
            -device usb-kbd,bus=xhci.0
            -device usb-mouse,bus=xhci.0
            -drive if=none,id=stick,format=raw,file="$STICK"
            -device usb-storage,drive=stick,bus=xhci.0,bootindex=0
        )
    else
        devs=(
            -device qemu-xhci,id=xhci0
            -device qemu-xhci,id=xhci1
            -device usb-hub,bus=xhci1.0,port=1
            -device usb-kbd,bus=xhci1.0,port=1.1
            -device usb-mouse,bus=xhci1.0,port=1.2
            -drive if=none,id=stick,format=raw,file="$STICK"
            -device usb-storage,drive=stick,bus=xhci0.0,bootindex=0
        )
    fi
    run_qemu "$name" "DIAG: SHOW" \
        qemu-system-x86_64 -machine q35 -cpu EPYC -m "$mem" -vga std \
        -drive if=pflash,format=raw,readonly=on,file="$CODE" \
        -drive if=pflash,format=raw,file="$vars" \
        "${devs[@]}"
}

uefi a512 512M one
uefi a4 4G one
uefi a6 6G one
uefi b512 512M two
uefi b4 4G two
uefi b6 6G two

run_qemu bios64 "SETUP: WELCOME" \
    qemu-system-i386 -machine pc -m 64 -vga std \
    -cdrom "$ROOT/dist/GeometricalOS.iso" -boot order=d

run_qemu bios128 "SETUP: WELCOME" \
    qemu-system-i386 -machine pc -m 128 -vga std \
    -cdrom "$ROOT/dist/GeometricalOS.iso" -boot order=d

run_qemu ide128 "SETUP: WELCOME" \
    qemu-system-i386 -machine pc -m 128 -vga std \
    -drive file="$ROOT/dist/GeometricalOS.iso",format=raw,if=ide,media=disk -boot c

echo "SHOTS:"
ls -l "$SHOTS"/*.png 2>/dev/null || true
