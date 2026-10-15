#!/usr/bin/env python3
"""Sauvegarde de la partition LittleFS de la carte AVANT un flash qui change le format des fichiers.

La 1.1.0 réécrit /vn_prof.bin (v3) et /vn_hist.bin (v2) ; les 1.0.8 et 1.0.9 refusent ces versions :
revenir en arrière sans cette sauvegarde, c'est perdre les profils appris. Le flash lui-même ne
touche pas la partition (l'image fusionnée s'arrête bien avant 0xC90000).

    python3 tools/sauvegarde_littlefs.py sauver --port /dev/ttyUSB0 --sortie littlefs-avant.bin
    python3 tools/sauvegarde_littlefs.py verifier littlefs-avant.bin
    python3 tools/sauvegarde_littlefs.py restaurer --port /dev/ttyUSB0 littlefs-avant.bin   (retour arrière)

« sauver » ne fait qu'une LECTURE (esptool read_flash), puis vérifie le fichier : taille exacte de la
partition, superbloc LittleFS présent, pas une flash vierge. Code de sortie ≠ 0 si la sauvegarde n'est
pas exploitable : dans ce cas, ne pas flasher. « --a-blanc » affiche les commandes sans les lancer.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

# default_16MB.csv (platformio.ini : board_build.partitions) : spiffs, data, spiffs, 0xc90000, 0x360000
FS_OFFSET = 0xC90000
FS_SIZE = 0x360000
BLOCK = 4096                       # taille de bloc LittleFS de l'ESP32 (Arduino)
MAGIC = b"littlefs"                # nom du superbloc, à l'octet 8 du bloc 0 ou 1


def esptool_cmd():
    """esptool : celui du PATH, sinon le module Python, sinon celui fourni par PlatformIO."""
    for exe in ("esptool.py", "esptool"):
        if shutil.which(exe):
            return [exe]
    pio = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")
    if os.path.exists(pio):
        return [sys.executable, pio]
    return [sys.executable, "-m", "esptool"]


def verifier(chemin):
    """Renvoie (ok, message). Ne lit que le fichier."""
    try:
        taille = os.path.getsize(chemin)
    except OSError as e:
        return False, f"fichier illisible : {e}"
    if taille != FS_SIZE:
        return False, f"taille {taille} o, attendu {FS_SIZE} o (0x{FS_SIZE:X}) : lecture incomplète"
    with open(chemin, "rb") as f:
        tete = f.read(2 * BLOCK)
        f.seek(0)
        h = hashlib.sha256(f.read()).hexdigest()
    if tete.count(b"\xff") == len(tete) and tete:
        return False, "les deux premiers blocs sont vierges (0xFF) : pas de système de fichiers à sauver"
    if tete[8:16] != MAGIC and tete[BLOCK + 8:BLOCK + 16] != MAGIC:
        return False, "aucun superbloc LittleFS (« littlefs » à l'octet 8 du bloc 0 ou 1) : mauvaise adresse ?"
    return True, f"sauvegarde valide : {taille} o, superbloc LittleFS présent, sha256 {h}"


def lancer(cmd, a_blanc):
    print("$ " + " ".join(cmd))
    if a_blanc:
        return 0
    return subprocess.call(cmd)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sp = p.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("sauver", help="lire la partition LittleFS de la carte (lecture seule) et vérifier le fichier")
    s.add_argument("--port", required=True)
    s.add_argument("--sortie", default="littlefs-avant.bin")
    s.add_argument("--vitesse", default="460800")
    s.add_argument("--a-blanc", action="store_true", help="afficher la commande sans la lancer")
    v = sp.add_parser("verifier", help="vérifier une sauvegarde existante")
    v.add_argument("fichier")
    r = sp.add_parser("restaurer", help="réécrire une sauvegarde vérifiée (retour arrière, APRÈS avoir reflashé l'ancienne version)")
    r.add_argument("--port", required=True)
    r.add_argument("--vitesse", default="460800")
    r.add_argument("--a-blanc", action="store_true")
    r.add_argument("fichier")
    a = p.parse_args(argv)

    if a.cmd == "verifier":
        ok, msg = verifier(a.fichier)
        print(("OK : " if ok else "REFUS : ") + msg)
        return 0 if ok else 2

    base = esptool_cmd() + ["--chip", "esp32s3", "-p", a.port, "-b", a.vitesse]
    if a.cmd == "sauver":
        if os.path.exists(a.sortie) and not a.a_blanc:
            print(f"REFUS : {a.sortie} existe déjà (une sauvegarde ne s'écrase pas)")
            return 2
        x = lancer(base + ["read_flash", f"0x{FS_OFFSET:X}", f"0x{FS_SIZE:X}", a.sortie], a.a_blanc)
        if a.a_blanc:
            return 0
        if x != 0:
            print(f"REFUS : esptool a échoué (code {x}) — ne pas flasher")
            return 2
        ok, msg = verifier(a.sortie)
        print(("OK : " if ok else "REFUS : ") + msg + ("" if ok else " — ne pas flasher"))
        if ok:
            print(f"Retour arrière : reflasher l'ancienne version, puis\n"
                  f"  python3 tools/sauvegarde_littlefs.py restaurer --port {a.port} {a.sortie}")
        return 0 if ok else 2

    # restaurer
    if not a.a_blanc:
        ok, msg = verifier(a.fichier)
        if not ok:
            print("REFUS : " + msg)
            return 2
    return lancer(base + ["write_flash", f"0x{FS_OFFSET:X}", a.fichier], a.a_blanc)


if __name__ == "__main__":
    sys.exit(main())
