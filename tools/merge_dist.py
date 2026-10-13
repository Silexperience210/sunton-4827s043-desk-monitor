#!/usr/bin/env python3
"""Fusionne bootloader + table de partitions + boot_app0 + application en une
image unique, à flasher **à l'adresse 0x0** (`dist/…bin`).

Pourquoi le mode SPI est forcé à `dio`
--------------------------------------
Le ROM de l'ESP32-S3 lit l'image de 0x0 **avant** que la flash ne soit
configurée : une en-tête qui annonce QIO n'est pas lisible et le ROM abandonne
(`ets_loader.c 78`) puis la carte boucle sur un chien de garde
(`rst:0x7 TG0WDT_SYS_RST`). Le bootloader passe ensuite la flash en QIO lui-même,
à partir de ses propres en-têtes. D'où : en-tête `dio`, application en QIO.

Usage
-----
  python3 tools/merge_dist.py --build .pio/build/esp32-4827s043c \
      --out dist/velohnav-ops-esp32-4827s043c-v1.0.3.bin
"""
import argparse
import os
import subprocess
import sys

FLASH_ARGS = ["--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB"]


def pio_home():
    return os.path.expanduser(os.environ.get("PLATFORMIO_CORE_DIR", "~/.platformio"))


def merge(build, out_path):
    home = pio_home()
    esptool = os.path.join(home, "packages", "tool-esptoolpy", "esptool.py")
    boot_app0 = os.path.join(home, "packages", "framework-arduinoespressif32",
                             "tools", "partitions", "boot_app0.bin")
    pieces = [("bootloader.bin", "0x0"), ("partitions.bin", "0x8000"),
              ("firmware.bin", "0x10000")]
    for name, _ in pieces:
        p = os.path.join(build, name)
        if not os.path.exists(p):
            sys.exit("introuvable : %s (lancer d'abord `pio run -e <env>`)" % p)
    if not os.path.exists(boot_app0):
        sys.exit("introuvable : %s" % boot_app0)

    cmd = [sys.executable, esptool, "--chip", "esp32s3", "merge_bin", "-o", out_path] + FLASH_ARGS + [
        "0x0", os.path.join(build, "bootloader.bin"),
        "0x8000", os.path.join(build, "partitions.bin"),
        "0xe000", boot_app0,
        "0x10000", os.path.join(build, "firmware.bin"),
    ]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return out_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True, help="dossier .pio/build/<env>")
    ap.add_argument("--out", required=True, help="fichier .bin à écrire")
    a = ap.parse_args()
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    merge(a.build, a.out)
    print("%s : %d octets" % (a.out, os.path.getsize(a.out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
