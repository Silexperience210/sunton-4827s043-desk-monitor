#!/usr/bin/env python3
"""Test de démarrage du firmware dans l'émulateur QEMU ESP32-S3 (Espressif).

Démarre le binaire de l'environnement PlatformIO « test-qemu », lit le port série
émulé et vérifie que le démarrage va jusqu'à « [boot] prêt » sans plantage
(Guru Meditation, abort, assertion, chien de garde). Fait aussi des captures
de l'image affichée (tampon miroir publié par le firmware) au format PPM.

Usage :
  python3 tools/qemu_boot_test.py --qemu /chemin/qemu-system-xtensa \
      --build .pio/build/test-qemu --out qemu_out [--seconds 40]

QEMU : https://github.com/espressif/qemu/releases (paquet qemu-xtensa-softmmu).
"""
import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

FAIL_PATTERNS = ("Guru Meditation", "abort() was called", "assert failed", "Backtrace:",
                 "Task watchdog", "stack overflow", "Stack canary", "[boot] ERREUR FATALE")


def merge(build, out_bin):
    pio_home = os.path.expanduser(os.environ.get("PLATFORMIO_CORE_DIR", "~/.platformio"))
    esptool = os.path.join(pio_home, "packages", "tool-esptoolpy", "esptool.py")
    boot_app0 = os.path.join(pio_home, "packages", "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")
    cmd = [sys.executable, esptool, "--chip", "esp32s3", "merge_bin", "-o", out_bin, "--fill-flash-size", "16MB",
           "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB",
           "0x0", os.path.join(build, "bootloader.bin"), "0x8000", os.path.join(build, "partitions.bin"),
           "0xe000", boot_app0, "0x10000", os.path.join(build, "firmware.bin")]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def monitor_cmd(sock_path, cmd):
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock_path)
    s.settimeout(2)
    time.sleep(0.2)
    try:
        s.recv(65536)
    except socket.timeout:
        pass
    s.sendall((cmd + "\n").encode())
    time.sleep(1.0)
    s.close()


def to_ppm(raw_path, ppm_path):
    d = open(raw_path, "rb").read()
    px = struct.unpack("<%dH" % (len(d) // 2), d)
    out = bytearray(b"P6\n480 272\n255\n")
    for c in px:
        r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
        out += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    open(ppm_path, "wb").write(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", required=True)
    ap.add_argument("--build", default=".pio/build/test-qemu")
    ap.add_argument("--out", default="qemu_out")
    ap.add_argument("--seconds", type=int, default=40)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    flash = os.path.join(a.out, "flash.bin")
    serial = os.path.join(a.out, "serie.log")
    sock = os.path.join(tempfile.mkdtemp(prefix="vnq"), "mon.sock")   # chemin court (limite des sockets Unix)
    merge(a.build, flash)
    for p in (serial, sock):
        if os.path.exists(p):
            os.remove(p)
    q = subprocess.Popen([a.qemu, "-nographic", "-machine", "esp32s3", "-m", "4M",
                          "-drive", "file=%s,if=mtd,format=raw" % flash,
                          "-serial", "file:" + serial, "-monitor", "unix:%s,server,nowait" % sock],
                         stdout=subprocess.DEVNULL, stderr=open(os.path.join(a.out, "qemu.err"), "w"))
    fb, ready_at, shots, t0 = None, None, 0, time.time()
    log = ""
    try:
        while time.time() - t0 < a.seconds:
            time.sleep(1)
            log = open(serial, errors="replace").read() if os.path.exists(serial) else ""
            if fb is None:
                m = re.search(r"\[qemu\] fb=(0x[0-9a-f]+)", log)
                fb = m.group(1) if m else None
            if ready_at is None and "[boot] prêt en" in log:
                ready_at = time.time() - t0
            if any(p in log for p in FAIL_PATTERNS) or q.poll() is not None:
                break
            # captures : 5 s après « prêt », puis en fin de test
            if fb and ready_at is not None and shots == 0 and time.time() - t0 > ready_at + 5:
                raw = os.path.join(a.out, "ecran_1.raw")
                monitor_cmd(sock, "memsave %s 261120 \"%s\"" % (fb, raw))
                shots = 1
        if fb and shots >= 1:
            raw = os.path.join(a.out, "ecran_2.raw")
            monitor_cmd(sock, "memsave %s 261120 \"%s\"" % (fb, raw))
    finally:
        q.terminate()
        q.wait(timeout=10)
    for n in ("ecran_1", "ecran_2"):
        raw = os.path.join(a.out, n + ".raw")
        if os.path.exists(raw) and os.path.getsize(raw) == 261120:
            to_ppm(raw, os.path.join(a.out, n + ".ppm"))
            os.remove(raw)
    os.remove(flash)
    print(log)
    fails = [p for p in FAIL_PATTERNS if p in log]
    if fails:
        print("ÉCHEC : %s" % ", ".join(fails))
        return 1
    if ready_at is None:
        print("ÉCHEC : « [boot] prêt » jamais atteint en %d s" % a.seconds)
        return 1
    stack = re.findall(r"pile UI libre \(min\) (\d+) o", log)
    print("OK : démarrage complet en %.1f s (émulé)%s" % (ready_at, (", pile UI libre min %s o" % stack[-1]) if stack else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
