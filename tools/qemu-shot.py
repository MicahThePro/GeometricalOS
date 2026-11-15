#!/usr/bin/env python3
"""Drive GeometricalOS in QEMU and save screenshots. Always exits."""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOTS = os.environ.get("SHOTS", "/tmp/shots")
MON = "/tmp/gos-mon.sock"
LOG = "/tmp/gos-serial.log"
DISK = os.path.join(ROOT, "build", "disk.img")
ISO = os.path.join(ROOT, "dist", "GeometricalOS.iso")


class Monitor:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(2.0)
        deadline = time.time() + 5
        last = None
        while time.time() < deadline:
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError) as exc:
                last = exc
                time.sleep(0.05)
        else:
            raise SystemExit("monitor connect failed: %s" % last)
        self._drain()

    def _drain(self):
        self.sock.settimeout(0.2)
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
        except (socket.timeout, BlockingIOError, OSError):
            pass
        self.sock.settimeout(2.0)

    def cmd(self, line, pause=0.05):
        self.sock.sendall((line + "\n").encode())
        if pause:
            time.sleep(pause)
        self.sock.settimeout(0.4)
        buf = b""
        deadline = time.time() + 3.0
        while time.time() < deadline:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                if b"(qemu)" in buf:
                    break
                continue
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            if buf.rstrip().endswith(b"(qemu)"):
                break
        return buf.decode("utf-8", "replace")

    def close(self):
        try:
            self.sock.settimeout(1.0)
            self.sock.sendall(b"quit\n")
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


def wait_log(token, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(LOG):
            text = open(LOG, "rb").read().decode("utf-8", "replace")
            if token in text:
                return text
        time.sleep(0.1)
    if os.path.exists(LOG):
        return open(LOG, "rb").read().decode("utf-8", "replace")
    return ""


def main():
    os.makedirs(SHOTS, exist_ok=True)
    mode = sys.argv[1] if len(sys.argv) > 1 else "hdd"
    if os.path.exists(MON):
        os.remove(MON)
    if os.path.exists(LOG):
        os.remove(LOG)
    if mode == "install":
        argv = [
            "qemu-system-i386", "-m", "64", "-vga", "std",
            "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % DISK,
            "-drive", "file=%s,format=raw,if=ide,index=2,media=cdrom" % ISO,
            "-boot", "order=dc",
        ]
    else:
        argv = [
            "qemu-system-i386", "-m", "64", "-vga", "std",
            "-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % DISK,
            "-boot", "c",
        ]
    argv += [
        "-serial", "file:%s" % LOG,
        "-monitor", "unix:%s,server,nowait" % MON,
        "-display", "none",
        "-rtc", "base=localtime",
        "-no-shutdown",
    ]
    proc = subprocess.Popen(argv)
    mon = None
    try:
        mon = Monitor(MON)
        if mode == "install":
            text = wait_log("SETUP: WELCOME", 15)
            print("WELCOME", "SETUP: WELCOME" in text)
            mon.cmd("screendump %s/setup.ppm" % SHOTS, 0.4)
            mon.cmd("sendkey ret", 0.2)
            text = wait_log("SETUP: NAME", 8)
            print("NAME", "SETUP: NAME" in text)
            mon.cmd("screendump %s/login-setup.ppm" % SHOTS, 0.3)
            mon.cmd("sendkey ret", 0.2)
            text = wait_log("SETUP: PASS", 8)
            print("PASS", "SETUP: PASS" in text)
            mon.cmd("sendkey ret", 0.2)
            text = wait_log("SETUP: CONFIRM", 8)
            print("CONFIRM", "SETUP: CONFIRM" in text)
            mon.cmd("screendump %s/confirm.ppm" % SHOTS, 0.3)
            mon.cmd("sendkey ret", 0.2)
            text = wait_log("SETUP: DONE", 20)
            print("DONE", "SETUP: DONE" in text)
            mon.cmd("screendump %s/done.ppm" % SHOTS, 0.3)
        else:
            text = wait_log("LOGIN: READY", 15)
            print("LOGIN", "LOGIN: READY" in text)
            if "LOGIN: READY" in text:
                time.sleep(0.2)
                mon.cmd("screendump %s/login.ppm" % SHOTS, 0.4)
                mon.cmd("sendkey ret", 0.2)
            text = wait_log("DESKTOP: READY", 12)
            print("READY", "DESKTOP: READY" in text)
            if "DESKTOP: READY" not in text:
                print(text[-800:])
                return 1
            time.sleep(0.2)
            mon.cmd("screendump %s/desktop.ppm" % SHOTS, 0.4)
            for key, name in (
                ("f3", "notepad"),
                ("f4", "calc"),
                ("f5", "clock"),
                ("f6", "paint"),
                ("f7", "mines"),
                ("f2", "files"),
                ("f8", "terminal"),
                ("f9", "solitaire"),
            ):
                mon.cmd("sendkey " + key, 0.25)
                time.sleep(0.2)
                mon.cmd("screendump %s/%s.ppm" % (SHOTS, name), 0.4)
            mon.cmd("sendkey f3", 0.2)
            for ch in "hello":
                mon.cmd("sendkey " + ch, 0.05)
            mon.cmd("screendump %s/notepad-typed.ppm" % SHOTS, 0.4)
        print(open(LOG, "rb").read().decode("utf-8", "replace")[-1000:])
        return 0
    finally:
        if mon:
            mon.close()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass


if __name__ == "__main__":
    sys.exit(main())
