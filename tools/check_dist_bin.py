#!/usr/bin/env python3
"""Contrôle structurel d'une image fusionnée `dist/…bin` avant publication.

Vérifie ce qu'un flash à 0x0 exige réellement :

  0x0      image bootloader — en-tête valide, **mode SPI = dio**, taille 16 Mo
  0x8000   table de partitions (magie 0xAA50)
  0x10000  image application — en-tête valide, puce ESP32-S3 (id 9)

Le contrôle du mode SPI est le garde-fou : publiée en `qio`, l'image ne démarre
pas (le ROM ne sait pas la lire) et la carte boucle — panne déjà constatée.

Usage
-----
  python3 tools/check_dist_bin.py dist/velohnav-ops-esp32-4827s043c-v1.0.3.bin [autre.bin …]
Sortie : une ligne par fichier, puis « OK » ou « ÉCHEC » ; code 1 si échec.
"""
import struct
import sys

SPI_MODES = {0x00: "qio", 0x01: "qout", 0x02: "dio", 0x03: "dout"}
SPI_SIZES = {0: "1MB", 1: "2MB", 2: "4MB", 3: "8MB", 4: "16MB"}
CHIP_IDS = {9: "ESP32-S3", 12: "ESP32-S2", 5: "ESP32-C3"}


def header(blob, name):
    """Décode l'en-tête d'image ESP (24 octets) à l'offset 0 du bloc."""
    if len(blob) < 24:
        return None, "%s : bloc trop court (%d octets)" % (name, len(blob))
    magic, segs, mode, szfreq = blob[0], blob[1], blob[2], blob[3]
    entry = struct.unpack("<I", blob[4:8])[0]
    chip = struct.unpack("<H", blob[0x0C:0x0E])[0]
    if magic != 0xE9:
        return None, "%s : magie 0x%02x au lieu de 0xE9" % (name, magic)
    if not 1 <= segs <= 16:
        return None, "%s : nombre de segments invalide (%d)" % (name, segs)
    return {"segs": segs, "mode": mode, "size": SPI_SIZES.get(szfreq >> 4, "?"),
            "freq": szfreq & 0x0F, "entry": entry, "chip": chip}, None


def check(path):
    errs = []
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < 0x10000 + 24:
        return ["%s : %d octets, trop court pour une image fusionnée" % (path, len(blob))]

    boot, boot_err = header(blob[0x0:], "bootloader")
    if boot_err:
        errs.append(boot_err)
    elif boot is not None:
        if boot["mode"] != 0x02:
            errs.append("bootloader : mode SPI « %s » — attendu « dio » ; ce fichier ne "
                        "démarrera pas flashé à 0x0" % SPI_MODES.get(boot["mode"], boot["mode"]))
        if boot["size"] != "16MB":
            errs.append("bootloader : taille de flash annoncée %s (attendu 16MB)" % boot["size"])

    if blob[0x8000:0x8002] != b"\xaa\x50":
        errs.append("0x8000 : pas de table de partitions (magie 0x%02x%02x)"
                    % (blob[0x8000], blob[0x8001]))

    app, app_err = header(blob[0x10000:], "application")
    if app_err:
        errs.append(app_err)
    elif app is not None:
        if app["chip"] != 9:
            errs.append("application : identifiant de puce %d (%s) — attendu 9 (ESP32-S3)"
                        % (app["chip"], CHIP_IDS.get(app["chip"], "inconnue")))

    if not errs and boot is not None and app is not None:
        print("%s : OK — bootloader %s %s, app %s, puce %s, %d octets"
              % (path, SPI_MODES.get(boot["mode"], boot["mode"]), boot["size"],
                 SPI_MODES.get(app["mode"], app["mode"]),
                 CHIP_IDS.get(app["chip"], app["chip"]), len(blob)))
    return errs


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    problems = []
    for path in argv:
        try:
            problems += check(path)
        except OSError as e:
            problems.append("%s : %s" % (path, e))
    if problems:
        for p in problems:
            print("ÉCHEC : " + p)
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
