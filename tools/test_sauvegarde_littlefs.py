#!/usr/bin/env python3
"""Tests de tools/sauvegarde_littlefs.py, sans carte.

Une vraie image LittleFS est fabriquée par mklittlefs (celui de PlatformIO) quand il est présent ;
sinon un superbloc minimal est écrit à la main (même octets « littlefs » à l'octet 8).
"""
import glob
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sauvegarde_littlefs as S  # noqa: E402

ok = fail = 0


def check(c, msg):
    global ok, fail
    if c:
        ok += 1
    else:
        fail += 1
        print("ÉCHEC : " + msg)


def image_reelle(dossier, sortie):
    mk = glob.glob(os.path.expanduser("~/.platformio/packages/tool-mklittlefs*/mklittlefs"))
    if not mk:
        return False
    subprocess.run([mk[0], "-c", dossier, "-b", str(S.BLOCK), "-p", "256", "-s", str(S.FS_SIZE), sortie],
                   check=True, capture_output=True)
    return True


with tempfile.TemporaryDirectory() as t:
    src = os.path.join(t, "fs")
    os.mkdir(src)
    for nom, n in (("vn_prof.bin", 40000), ("vn_hist.bin", 9000), ("vn_days.bin", 3000)):
        with open(os.path.join(src, nom), "wb") as f:
            f.write(bytes((i * 7) & 255 for i in range(n)))
    img = os.path.join(t, "img.bin")
    reelle = image_reelle(src, img)
    if not reelle:
        b = bytearray(b"\xff" * S.FS_SIZE)
        b[8:16] = b"littlefs"
        open(img, "wb").write(b)
    print("image : " + ("mklittlefs (réelle)" if reelle else "superbloc écrit à la main"))
    check(os.path.getsize(img) == S.FS_SIZE, "taille de l'image fabriquée")
    v, m = S.verifier(img)
    check(v, "image LittleFS valide acceptée : " + m)

    data = open(img, "rb").read()
    tronq = os.path.join(t, "tronq.bin")
    open(tronq, "wb").write(data[:-4096])
    check(not S.verifier(tronq)[0], "lecture incomplète refusée")
    vierge = os.path.join(t, "vierge.bin")
    open(vierge, "wb").write(b"\xff" * S.FS_SIZE)
    check(not S.verifier(vierge)[0], "flash vierge refusée")
    decale = os.path.join(t, "decale.bin")           # lu à la mauvaise adresse : code de l'application
    open(decale, "wb").write(bytes((i * 31 + 5) & 255 for i in range(S.FS_SIZE)))
    check(not S.verifier(decale)[0], "mauvaise adresse (pas de superbloc) refusée")
    check(not S.verifier(os.path.join(t, "absent.bin"))[0], "fichier absent refusé")

    # Une sauvegarde ne s'écrase pas
    check(S.main(["sauver", "--port", "/dev/null", "--sortie", img]) == 2, "sauvegarde existante jamais écrasée")
    check(open(img, "rb").read() == data, "le fichier existant est intact")
    # Restaurer une sauvegarde invalide : refusé avant esptool
    check(S.main(["restaurer", "--port", "/dev/null", vierge]) == 2, "restauration d'une image vierge refusée")

# La commande lit exactement la partition de la table de partitions de platformio.ini
csv = glob.glob(os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32*/tools/partitions/default_16MB.csv"))
if csv:
    lignes = [l.split(",") for l in open(csv[0]) if l.startswith("spiffs")]
    check(lignes and int(lignes[0][3], 16) == S.FS_OFFSET and int(lignes[0][4], 16) == S.FS_SIZE,
          "adresse et taille = default_16MB.csv")
ini = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "platformio.ini")).read()
check("board_build.partitions = default_16MB.csv" in ini, "platformio.ini utilise bien default_16MB.csv")

print(f"\n{ok} vérifications réussies, {fail} échec(s)")
sys.exit(1 if fail else 0)
