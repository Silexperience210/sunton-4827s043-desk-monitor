#!/usr/bin/env python3
"""Recopie sur le PC les données apprises par la carte (commande série « dump »).

La carte envoie, en lecture seule, un instantané de /vn_hist.bin (historique 5 min, 24 h),
/vn_prof.bin (profils appris), /vn_days.bin (résumés par jour) et /vn_stations.csv (capacités),
en base64, par blocs munis chacun d'une somme CRC32, chaque fichier ayant aussi la sienne
(protocole : src/core/dump.h). Cet outil envoie « ping » puis « dump », vérifie TOUT (ordre des
blocs, sommes, tailles, fin explicite) et n'écrit les fichiers que si tout est juste :

  <sortie>/archive/<AAAAMMJJ-HHMMSS>/vn_hist.bin …   (un dossier par vidage, horodaté par la carte)
  <sortie>/vn_hist.bin …                              (copie du dernier vidage réussi)

Le banc les rejoue : sim/bench_prevision --reel <sortie>/archive <sortie>/vn_prof.bin

Usage
-----
  .venv/bin/python tools/carte_dump.py                          # /dev/ttyUSB0, 115 200 bauds
  .venv/bin/python tools/carte_dump.py --port /dev/ttyUSB1 --sortie /tmp/donnees
  python3 tools/carte_dump.py --depuis capture.txt              # relit une capture (sans pyserial)

Codes de sortie : 0 réussi · 1 transmission fausse ou incomplète · 2 usage · 3 la carte ne répond pas.

Liaison série : NON VÉRIFIÉE sur la carte. Le port est ouvert sans toucher à DTR/RTS (sur la
Sunton, le CH340 et le circuit de réarmement automatique de l'ESP32 redémarrent la carte quand
l'un est levé sans l'autre) ; si la carte redémarre quand même (lignes « [boot] » reçues), l'outil
le dit. Un redémarrage n'efface rien : la carte relit ses profils (sauvegardés toutes les 30 min).
"""
import argparse
import base64
import binascii
import datetime
import json
import os
import shutil
import sys
import tempfile
import time
import zlib

PROTO = 1
REQUIRED = ("/vn_hist.bin", "/vn_prof.bin")
DEFAULT_OUT = os.path.expanduser("~/.hermes/cache/scratch/donnees-carte")


class DumpError(Exception):
    pass


class Decoder:
    """Machine à états du protocole. feed() une ligne reçue ; result() les fichiers vérifiés."""

    def __init__(self):
        self.reset_seen = False
        self.started = False
        self.finished = False
        self.version = None
        self.t = None
        self.nfiles = None
        self.files = {}          # nom → octets vérifiés
        self.errors = []         # premières erreurs, dans l'ordre
        self.cur = None          # fichier en cours : dict
        self.blocks_total = 0
        self.blocks_done = 0
        self.aborted = None

    def err(self, msg):
        if len(self.errors) < 20:
            self.errors.append(msg)

    def feed(self, raw):
        if isinstance(raw, bytes):
            raw = raw.decode("utf-8", "replace")
        line = raw.strip("\r\n")
        if "[boot]" in line or line.startswith("ESP-ROM") or line.startswith("rst:"):
            self.reset_seen = True
        k = line.find("VNDUMP ")
        if k < 0:
            return
        f = line[k:].split()
        kind = f[1] if len(f) > 1 else ""
        if kind == "DEBUT":
            kv = kvs(f[2:])
            reset_seen = self.reset_seen
            self.__init__()
            self.reset_seen = reset_seen
            self.started = True
            try:
                proto = int(kv.get("proto", "-1"))
            except ValueError:
                proto = -1
            if proto != PROTO:
                self.err("protocole %s inconnu (cet outil lit le protocole %d)" % (kv.get("proto"), PROTO))
            self.version = kv.get("version")
            self.t = int(kv["t"]) if kv.get("t", "").lstrip("-").isdigit() else None
            self.nfiles = int(kv["fichiers"]) if kv.get("fichiers", "").isdigit() else None
        elif not self.started:
            return
        elif kind == "FICHIER":
            if len(f) < 7:
                return self.err("ligne FICHIER incomplète : %r" % line[k:k + 80])
            kv = kvs(f[4:])
            try:
                self.cur = {"i": int(f[2]), "name": f[3], "size": int(kv["taille"]), "blocks": int(kv["blocs"]),
                            "crc": int(kv["crc"], 16), "next": 0, "data": bytearray(), "bad": False}
            except (KeyError, ValueError):
                self.cur = None
                return self.err("ligne FICHIER illisible : %r" % line[k:k + 80])
            self.blocks_total += self.cur["blocks"]
        elif kind == "B":
            c = self.cur
            if c is None:
                return self.err("bloc reçu hors fichier (ligne FICHIER perdue ?)")
            if c["bad"]:
                return
            if len(f) != 6:
                c["bad"] = True
                return self.err("%s : bloc %d tronqué (ligne incomplète)" % (c["name"], c["next"]))
            try:
                i, j = int(f[2]), int(f[3])
            except ValueError:
                c["bad"] = True
                return self.err("%s : numéro de bloc illisible" % c["name"])
            if i != c["i"]:
                c["bad"] = True
                return self.err("%s : bloc d'un autre fichier (%d) reçu au milieu" % (c["name"], i))
            if j != c["next"]:
                c["bad"] = True
                return self.err("%s : bloc %d manquant (reçu le bloc %d) — octets perdus sur la liaison"
                                % (c["name"], c["next"], j))
            try:
                data = base64.b64decode(f[4], validate=True)
                crc = int(f[5], 16)
            except (binascii.Error, ValueError):
                c["bad"] = True
                return self.err("%s : bloc %d illisible (base64 ou somme abîmés)" % (c["name"], j))
            if zlib.crc32(data) & 0xFFFFFFFF != crc:
                c["bad"] = True
                return self.err("%s : somme fausse au bloc %d (octets %d…%d)" % (c["name"], j, len(c["data"]),
                                                                               len(c["data"]) + len(data) - 1))
            c["data"] += data
            c["next"] += 1
            self.blocks_done += 1
        elif kind == "FIN":
            c = self.cur
            if c is None:
                return self.err("FIN reçue hors fichier")
            self.cur = None
            if c["bad"]:
                return
            kv = kvs(f[4:])
            if len(f) < 6 or f[2] != str(c["i"]) or f[3] != c["name"] or kv.get("blocs") != str(c["blocks"]) \
                    or kv.get("crc", "").upper() != "%08X" % c["crc"]:
                return self.err("%s : ligne FIN incohérente avec la ligne FICHIER (%r)" % (c["name"], line[k:k + 80]))
            if c["next"] != c["blocks"]:
                return self.err("%s : tronqué, %d blocs reçus sur %d" % (c["name"], c["next"], c["blocks"]))
            if len(c["data"]) != c["size"]:
                return self.err("%s : taille %d au lieu de %d" % (c["name"], len(c["data"]), c["size"]))
            if zlib.crc32(bytes(c["data"])) & 0xFFFFFFFF != c["crc"]:
                return self.err("%s : somme du fichier fausse (blocs justes un à un : réassemblage faux)" % c["name"])
            self.files[c["name"]] = bytes(c["data"])
        elif kind == "TERMINE":
            if self.cur is not None:
                self.err("%s : tronqué, pas de ligne FIN (%d blocs sur %d)" % (self.cur["name"], self.cur["next"],
                                                                            self.cur["blocks"]))
                self.cur = None
            self.finished = True
        elif kind == "ABANDON":
            self.aborted = " ".join(f[2:]) or "sans raison"
            self.finished = True

    def progress(self):
        return self.blocks_done, self.blocks_total

    def result(self):
        """Fichiers vérifiés ; DumpError avec un message clair sinon."""
        if not self.started:
            raise DumpError("aucun début de vidage reçu (VNDUMP DEBUT)")
        if self.aborted:
            raise DumpError("la carte a abandonné le vidage : %s" % self.aborted)
        if self.errors:
            raise DumpError("; ".join(self.errors))
        if not self.finished:
            if self.cur is not None:
                raise DumpError("liaison interrompue au milieu de %s : %d blocs reçus sur %d (pas de fin de transmission)"
                                % (self.cur["name"], self.cur["next"], self.cur["blocks"]))
            raise DumpError("liaison interrompue : pas de fin de transmission (VNDUMP TERMINE), %d fichier(s) complet(s)"
                            % len(self.files))
        missing = [n for n in REQUIRED if n not in self.files]
        if missing:
            raise DumpError("fichier(s) absent(s) du vidage : %s" % ", ".join(missing))
        if self.nfiles is not None and len(self.files) != self.nfiles:
            raise DumpError("%d fichier(s) reçu(s) sur %d annoncés" % (len(self.files), self.nfiles))
        return self.files


def kvs(fields):
    d = {}
    for x in fields:
        if "=" in x:
            k, v = x.split("=", 1)
            d[k] = v
    return d


def stamp_of(t):
    if t and t > 0:
        return datetime.datetime.fromtimestamp(t).strftime("%Y%m%d-%H%M%S")
    return datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + "-pc"


def save(files, dec, out_dir, meta):
    """Écrit d'abord dans un dossier temporaire, puis le renomme : jamais de vidage à moitié écrit."""
    arch = os.path.join(out_dir, "archive")
    os.makedirs(arch, exist_ok=True)
    stamp = stamp_of(dec.t)
    final = os.path.join(arch, stamp)
    n = 2
    while os.path.exists(final):
        final = os.path.join(arch, "%s-%d" % (stamp, n))
        n += 1
    tmp = tempfile.mkdtemp(prefix=".vidage-", dir=arch)
    for name, data in files.items():
        with open(os.path.join(tmp, os.path.basename(name)), "wb") as fp:
            fp.write(data)
    man = dict(meta)
    man.update({"version_carte": dec.version, "t_carte": dec.t, "redemarrage_vu": dec.reset_seen,
                "fichiers": {os.path.basename(k): {"taille": len(v), "crc32": "%08X" % (zlib.crc32(v) & 0xFFFFFFFF)}
                             for k, v in files.items()}})
    with open(os.path.join(tmp, "manifeste.json"), "w") as fp:
        json.dump(man, fp, indent=2, ensure_ascii=False)
    os.rename(tmp, final)
    for name in os.listdir(final):                 # copie « dernier vidage » à la racine
        if name.startswith("vn_"):
            shutil.copyfile(os.path.join(final, name), os.path.join(out_dir, name))
    return final


def save_failure(lines, out_dir, why):
    d = os.path.join(out_dir, "echecs")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + ".txt")
    with open(p, "wb") as fp:
        fp.write(("# vidage raté : %s\n" % why).encode())
        for ln in lines:
            fp.write(ln if ln.endswith(b"\n") else ln + b"\n")
    return p


def read_transcript(path):
    with open(path, "rb") as fp:
        return fp.read().splitlines()


def from_serial(a, log):
    try:
        import serial
    except ImportError:
        sys.exit("pyserial absent : lancer avec .venv/bin/python (PlatformIO l'installe) ou `pip install pyserial`")
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = a.port, a.baud, 0.25
    if a.dtr is not None:
        ser.dtr = bool(a.dtr)
    if a.rts is not None:
        ser.rts = bool(a.rts)
    try:
        ser.open()
    except (serial.SerialException, OSError) as e:
        raise DumpError("port %s inaccessible : %s" % (a.port, e))
    lines = []
    dec = Decoder()

    def readline():
        try:
            ln = ser.readline()
        except (serial.SerialException, OSError) as e:
            raise DumpError("liaison coupée : %s" % e)
        if ln:
            lines.append(ln)
            dec.feed(ln)
        return ln

    try:
        # ce qui arrive juste après l'ouverture est lu (et non jeté) : des lignes [boot] = la carte a redémarré
        t0 = time.time()
        while time.time() - t0 < 0.5:
            readline()
        # 1. ping : la carte connaît-elle la commande ?
        ok = False
        for _ in range(3):
            ser.write(b"\nping\n")
            t0 = time.time()
            while time.time() - t0 < a.attente_ping:
                ln = readline()
                if ln and b"VNDUMP PRET" in ln:
                    ok = True
                    break
            if ok:
                break
        if not ok:
            why = ("la carte ne répond pas à « ping » : micrologiciel sans la commande dump (v1.0.8 ?), "
                   "mauvais port ou mauvais débit")
            if dec.reset_seen:
                why += " — la carte a REDÉMARRÉ à l'ouverture du port (lignes [boot] reçues)"
            e = DumpError(why)
            e.code = 3
            e.lines = lines
            raise e
        # 2. dump
        ser.write(b"dump\n")
        last = time.time()
        t_start = last
        shown = -1
        while not dec.finished:
            ln = readline()
            now = time.time()
            if ln:
                last = now
            elif now - last > a.silence:
                break
            if now - t_start > a.duree_max:
                break
            d, tot = dec.progress()
            if tot and log and int(100 * d / tot) // 5 != shown:
                shown = int(100 * d / tot) // 5
                sys.stderr.write("\r  %3d %%  (%d / %d blocs, %.0f s)" % (100 * d // tot, d, tot, now - t_start))
                sys.stderr.flush()
        if log:
            sys.stderr.write("\n")
        if not dec.finished:
            ser.write(b"stop\n")
    except KeyboardInterrupt:
        try:
            ser.write(b"stop\n")
        except Exception:
            pass
        raise
    finally:
        try:
            ser.close()
        except Exception:
            pass
    return dec, lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--sortie", default=DEFAULT_OUT, help="dossier de sortie (défaut %(default)s)")
    ap.add_argument("--depuis", metavar="CAPTURE", help="relire une capture de la liaison au lieu du port série")
    ap.add_argument("--essais", type=int, default=2, help="vidages tentés si le précédent est faux (défaut 2)")
    ap.add_argument("--silence", type=float, default=15, help="abandon après N s sans rien recevoir (défaut 15)")
    ap.add_argument("--duree-max", type=float, default=900, help="durée maximale d'un vidage, s (défaut 900)")
    ap.add_argument("--attente-ping", type=float, default=2.0, help=argparse.SUPPRESS)
    ap.add_argument("--dtr", type=int, choices=(0, 1), help="forcer DTR à l'ouverture (défaut : inchangé)")
    ap.add_argument("--rts", type=int, choices=(0, 1), help="forcer RTS à l'ouverture (défaut : inchangé)")
    ap.add_argument("-q", "--silencieux", action="store_true")
    a = ap.parse_args(argv)
    out_dir = os.path.expanduser(a.sortie)
    log = not a.silencieux

    tries = 1 if a.depuis else max(1, a.essais)
    last_err = None
    for k in range(tries):
        lines = []
        try:
            if a.depuis:
                lines = read_transcript(a.depuis)
                dec = Decoder()
                for ln in lines:
                    dec.feed(ln)
            else:
                if log:
                    print("vidage %d/%d depuis %s (%d bauds)…" % (k + 1, tries, a.port, a.baud), file=sys.stderr)
                dec, lines = from_serial(a, log)
            files = dec.result()
        except DumpError as e:
            last_err = e
            lines = getattr(e, "lines", lines)
            code = getattr(e, "code", 1)
            p = save_failure(lines, out_dir, str(e)) if lines else None
            print("ÉCHEC : %s%s" % (e, (" (capture : %s)" % p) if p else ""), file=sys.stderr)
            if code == 3:
                return 3
            continue
        meta = {"source": a.depuis or a.port, "baud": None if a.depuis else a.baud,
                "recu_le": datetime.datetime.now().isoformat(timespec="seconds")}
        final = save(files, dec, out_dir, meta)
        if dec.reset_seen:
            print("ATTENTION : la carte a redémarré pendant la liaison (lignes [boot] reçues). Les données sont "
                  "justes (instantané vérifié), mais le dernier quart d'heure d'apprentissage non sauvegardé est perdu.",
                  file=sys.stderr)
        print("vidage vérifié : %s" % final)
        for n, d in sorted(files.items()):
            print("  %-16s %8d octets  crc %08X" % (os.path.basename(n), len(d), zlib.crc32(d) & 0xFFFFFFFF))
        return 0
    print("abandon après %d essai(s) : %s" % (tries, last_err), file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
