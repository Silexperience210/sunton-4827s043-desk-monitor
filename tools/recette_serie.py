#!/usr/bin/env python3
"""Recette série après flash, tolérante aux décrochages du CH340.

Le port peut disparaître une seconde (renumérotation USB au redémarrage de la
carte) : on rouvre au lieu de mourir. But final : obtenir
`VNDUMP PRET proto=1 version=<X.Y.Z>` en réponse à `ping`, la preuve directe
de l'image réellement flashée.
"""
import glob
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else None


def trouver_port():
    """Le CH340 change de nom (ttyUSB0 → ttyUSB1) quand il renumère.

    Chercher le port à chaque ouverture plutôt que de garder un nom figé :
    c'est ce qui a fait croire à un décrochage alors que la carte était là.
    """
    if PORT:
        return PORT
    candidats = sorted(glob.glob('/dev/ttyUSB*'))
    return candidats[0] if candidats else None

ATTENTES = ('[boot]', '[wifi]', '[net]', '[fs]', '[touch]', '[ui]', 'VNDUMP',
            'restauré', 'prêt en')


def ouvrir(tentatives=6):
    for i in range(tentatives):
        try:
            port = trouver_port()
            if not port:
                raise RuntimeError('aucun /dev/ttyUSB*')
            s = serial.Serial(port, 115200, timeout=1)
            s.dtr = False
            s.rts = False
            return s
        except Exception as e:
            print(f"  (port indisponible, essai {i+1}/{tentatives} : {type(e).__name__})", flush=True)
            time.sleep(2)
    return None


def lire(s, duree, arret=None):
    """Lit pendant `duree` secondes ; renvoie tout le texte accumulé."""
    texte = ''
    debut = time.time()
    while time.time() - debut < duree:
        try:
            bloc = s.read(4096)
        except Exception:
            s2 = ouvrir(3)
            if s2 is None:
                return texte
            s = s2
            continue
        if bloc:
            texte += bloc.decode('utf-8', 'replace')
            if arret and arret in texte:
                break
    return texte


s = ouvrir()
if s is None:
    print("VERDICT : port série inaccessible")
    sys.exit(1)

print(f"--- journal de démarrage ({trouver_port()}, 40 s) ---", flush=True)
journal = lire(s, 40)
for ligne in journal.splitlines():
    if any(a in ligne for a in ATTENTES):
        print('  ' + ligne.strip()[:160], flush=True)

print("\n--- commande ping ---", flush=True)
try:
    s.write(b'ping\n')
    s.flush()
except Exception as e:
    print('  (écriture impossible, on rouvre)', type(e).__name__)
    s = ouvrir() or s
    try:
        s.write(b'ping\n')
    except Exception:
        pass

reponse = lire(s, 15, arret='VNDUMP PRET')
for ligne in reponse.splitlines():
    if 'VNDUMP' in ligne or 'PRET' in ligne:
        print('  ' + ligne.strip()[:200], flush=True)

print("\nVERDICT :", 'la carte a annoncé sa version' if 'VNDUMP PRET' in reponse else 'PAS DE REPONSE AU PING')
try:
    s.close()
except Exception:
    pass
