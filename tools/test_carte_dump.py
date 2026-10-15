#!/usr/bin/env python3
"""Vérifie la chaîne « carte → liaison série → PC » SANS la carte.

L'émetteur simulé sim/dump_sim (même instantané, même émetteur, même lecteur de commandes que la
carte : src/core/dump.cpp) est branché sur un pseudo-terminal ; l'outil tools/carte_dump.py ouvre
l'autre côté avec pyserial, exactement comme il ouvrirait /dev/ttyUSB0. Ce qui n'est PAS vérifié
ici : le câble, le CH340, le circuit de réarmement de la carte, le débit réel.

  cd sim && make dump_sim && cd .. && .venv/bin/python tools/test_carte_dump.py
"""
import os
import subprocess
import sys
import tempfile
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(ROOT, "sim")
TOOL = os.path.join(ROOT, "tools", "carte_dump.py")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import carte_dump  # noqa: E402

ok = fail = 0


def check(cond, msg):
    global ok, fail
    if cond:
        ok += 1
    else:
        fail += 1
        print("ÉCHEC : " + msg)


def run_pty(sim_args, tool_args, tmp):
    """dump_sim sur le maître d'un pty, l'outil sur l'esclave ; rend (code, sortie+erreurs)."""
    master, slave = os.openpty()
    tty.setraw(slave)
    sim = subprocess.Popen([os.path.join(SIM, "dump_sim")] + sim_args, cwd=SIM, stdin=master, stdout=master,
                           stderr=subprocess.DEVNULL)
    os.close(master)
    try:
        r = subprocess.run([sys.executable, TOOL, "--port", os.ttyname(slave), "--sortie", tmp, "-q"] + tool_args,
                           capture_output=True, text=True, timeout=120)
    finally:
        sim.kill()
        sim.wait()
        os.close(slave)
    return r.returncode, r.stdout + r.stderr


def archives(out):
    d = os.path.join(out, "archive")
    return sorted(x for x in os.listdir(d) if not x.startswith(".")) if os.path.isdir(d) else []


def same_files(a, b, names):
    for n in names:
        pa, pb = os.path.join(a, n), os.path.join(b, n)
        if not (os.path.exists(pa) and os.path.exists(pb)):
            return False
        if open(pa, "rb").read() != open(pb, "rb").read():
            return False
    return True


NAMES = ["vn_hist.bin", "vn_prof.bin", "vn_days.bin", "vn_stations.csv"]


def main():
    if not os.path.exists(os.path.join(SIM, "dump_sim")):
        sys.exit("construire d'abord : cd sim && make dump_sim")
    tmp = tempfile.mkdtemp(prefix="vn_test_dump_")
    ref = os.path.join(tmp, "reference")

    # 1. Aller-retour octet pour octet, à travers un vrai terminal
    out = os.path.join(tmp, "o1")
    code, txt = run_pty(["--reference", ref], [], out)
    check(code == 0, "aller-retour : code %d (%s)" % (code, txt.strip()))
    a = archives(out)
    check(len(a) == 1, "aller-retour : un dossier d'archive (%s)" % a)
    if a:
        check(same_files(ref, os.path.join(out, "archive", a[0]), NAMES), "aller-retour : archive identique à l'instantané")
        check(same_files(ref, out, NAMES), "aller-retour : copie « dernier vidage » identique")
        check(os.path.exists(os.path.join(out, "archive", a[0], "manifeste.json")), "manifeste écrit")
        check(a[0] == "20261013-143041", "archive horodatée par la carte (%s)" % a[0])

    # 2. Le fichier de la carte relu puis revidé : mêmes octets que le fichier (la flash = le vidage)
    out2 = os.path.join(tmp, "o2")
    ref2 = os.path.join(tmp, "reference2")
    code, txt = run_pty(["--fs", ref, "--reference", ref2], [], out2)
    check(code == 0, "revidage : code %d (%s)" % (code, txt.strip()))
    #    (sauf savedAt, octets 32…39 de l'en-tête : l'instant du dernier relevé, 0 tant que le modèle relu
    #    n'a reçu aucun relevé — sur la carte, le vidage est pris d'un modèle qui tourne)
    for n in ["vn_hist.bin", "vn_prof.bin", "vn_days.bin"]:
        x = open(os.path.join(ref, n), "rb").read()
        y = open(os.path.join(ref2, n), "rb").read()
        check(len(x) == len(y) and x[:32] == y[:32] and x[40:] == y[40:],
              "%s relu (Model::load) puis revidé : octets identiques au fichier d'origine (hors savedAt)" % n)

    # 3. Journaux intercalés et collés devant des lignes VNDUMP : sans effet
    out3 = os.path.join(tmp, "o3")
    code, txt = run_pty(["--bruit"], [], out3)
    check(code == 0 and len(archives(out3)) == 1, "journaux intercalés : vidage juste (%d, %s)" % (code, txt.strip()))
    if archives(out3):
        check(same_files(ref, os.path.join(out3, "archive", archives(out3)[0]), NAMES), "journaux : octets identiques")

    # 4. Parasite sur une ligne : somme fausse ; un seul essai → échec propre, rien d'écrit
    out4 = os.path.join(tmp, "o4")
    code, txt = run_pty(["--corrompre", "700"], ["--essais", "1"], out4)
    check(code == 1, "somme fausse : code 1 (%d)" % code)
    check("somme fausse au bloc" in txt or "illisible" in txt, "somme fausse : message (%s)" % txt.strip())
    check(archives(out4) == [] and not os.path.exists(os.path.join(out4, "vn_prof.bin")), "somme fausse : aucun fichier écrit")
    check(os.path.isdir(os.path.join(out4, "echecs")) and os.listdir(os.path.join(out4, "echecs")),
          "somme fausse : capture de l'échec gardée")
    #    … deux essais : le second vidage est propre → réussi
    out4b = os.path.join(tmp, "o4b")
    code, txt = run_pty(["--corrompre", "700"], ["--essais", "2"], out4b)
    check(code == 0 and len(archives(out4b)) == 1, "somme fausse puis reprise : réussi (%d, %s)" % (code, txt.strip()))

    # 5. Ligne perdue : bloc manquant
    out5 = os.path.join(tmp, "o5")
    code, txt = run_pty(["--perdre", "1000"], ["--essais", "1"], out5)
    check(code == 1 and "manquant" in txt, "ligne perdue : bloc manquant (%d, %s)" % (code, txt.strip()))
    check(archives(out5) == [], "ligne perdue : aucun fichier écrit")

    # 6. Câble arraché au milieu
    out6 = os.path.join(tmp, "o6")
    code, txt = run_pty(["--couper-apres", "900"], ["--essais", "1"], out6)
    check(code == 1 and ("coupée" in txt or "interrompue" in txt), "coupure : message propre (%d, %s)" % (code, txt.strip()))
    check(archives(out6) == [], "coupure : aucun fichier écrit")

    # 7. Carte figée au milieu : silence
    out7 = os.path.join(tmp, "o7")
    code, txt = run_pty(["--silence-apres", "900"], ["--essais", "1", "--silence", "1.5"], out7)
    check(code == 1 and "interrompue au milieu de /vn_prof.bin" in txt, "silence : fichier et blocs nommés (%d, %s)" % (code, txt.strip()))

    # 8. Micrologiciel 1.0.8 : la commande n'existe pas
    out8 = os.path.join(tmp, "o8")
    code, txt = run_pty(["--ancien"], ["--attente-ping", "0.5"], out8)
    check(code == 3 and "ne répond pas" in txt, "v1.0.8 : code 3 et message (%d, %s)" % (code, txt.strip()))

    # 9. Redémarrage à l'ouverture du port : vidage juste, mais signalé
    out9 = os.path.join(tmp, "o9")
    code, txt = run_pty(["--reset"], [], out9)
    check(code == 0 and "redémarré" in txt, "redémarrage : signalé (%d, %s)" % (code, txt.strip()))

    # 10. Captures abîmées relues hors ligne (--depuis), sans pyserial
    cap = os.path.join(tmp, "capture.txt")
    p = subprocess.run([os.path.join(SIM, "dump_sim")], cwd=SIM, input=b"dump\n", capture_output=True, timeout=60)
    open(cap, "wb").write(p.stdout)
    lines = p.stdout.splitlines(keepends=True)

    def depuis(data, name):
        f = os.path.join(tmp, name)
        open(f, "wb").write(data)
        o = os.path.join(tmp, "out_" + name)
        r = subprocess.run([sys.executable, TOOL, "--depuis", f, "--sortie", o, "-q"], capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr, o

    code, txt, o = depuis(p.stdout, "entiere.txt")
    check(code == 0 and same_files(ref, os.path.join(o, "archive", archives(o)[0]) if archives(o) else o, NAMES),
          "capture entière : octets identiques (%d, %s)" % (code, txt.strip()))
    code, txt, o = depuis(b"".join(lines[: len(lines) // 2]), "moitie.txt")
    check(code == 1 and "interrompue au milieu de /vn_prof.bin" in txt, "capture coupée : (%d, %s)" % (code, txt.strip()))
    check(archives(o) == [], "capture coupée : rien écrit")
    half = lines[:]
    k = next(i for i, ln in enumerate(half) if b" B 1 50 " in ln)
    half[k] = half[k][:60] + b"\n"                     # ligne de bloc tronquée
    code, txt, o = depuis(b"".join(half), "bloc_tronque.txt")
    check(code == 1 and "/vn_prof.bin" in txt and ("tronqué" in txt or "illisible" in txt or "somme" in txt),
          "bloc tronqué : (%d, %s)" % (code, txt.strip()))
    sw = lines[:]
    k = next(i for i, ln in enumerate(sw) if b" B 0 10 " in ln)
    ln = sw[k].split(b" ")
    ln[-1] = b"00000000\n"                             # somme du bloc fausse, données justes
    sw[k] = b" ".join(ln)
    code, txt, o = depuis(b"".join(sw), "somme.txt")
    check(code == 1 and "somme fausse au bloc 10" in txt, "somme de bloc fausse : (%d, %s)" % (code, txt.strip()))
    ff = [ln for ln in lines if not ln.startswith(b"VNDUMP FIN 0")]
    code, txt, o = depuis(b"".join(ff), "sans_fin.txt")
    check(code == 1, "ligne FIN perdue : échec (%d, %s)" % (code, txt.strip()))
    fs = lines[:]
    k = next(i for i, ln in enumerate(fs) if ln.startswith(b"VNDUMP FIN 1"))
    parts = fs[k].split(b"crc=")
    fs[k] = parts[0] + b"crc=DEADBEEF\n"               # somme de FICHIER fausse
    code, txt, o = depuis(b"".join(fs), "somme_fichier.txt")
    check(code == 1 and "somme" in txt, "somme de fichier fausse : (%d, %s)" % (code, txt.strip()))
    # Somme du FICHIER fausse mais cohérente entre FICHIER et FIN, blocs justes un à un : seule la somme
    # du fichier entier le voit (revue, mutation BR2)
    fe = lines[:]
    for pre in (b"VNDUMP FICHIER 1 ", b"VNDUMP FIN 1 "):
        k = next(i for i, ln in enumerate(fe) if ln.startswith(pre))
        fe[k] = fe[k].split(b"crc=")[0] + b"crc=DEADBEEF\n"
    code, txt, o = depuis(b"".join(fe), "somme_entiere.txt")
    check(code == 1 and "somme du fichier fausse" in txt, "somme du fichier entier fausse, blocs justes : (%d, %s)" % (code, txt.strip()))
    check(archives(o) == [], "somme du fichier entier fausse : rien écrit")
    code, txt, o = depuis(b"[net] cycle 800 ms\n" * 10, "rien.txt")
    check(code == 1 and "aucun début" in txt, "aucun vidage dans la capture : (%d, %s)" % (code, txt.strip()))
    ab = lines[:200] + [b"VNDUMP ABANDON stop demande\n"]
    code, txt, o = depuis(b"".join(ab), "abandon.txt")
    check(code == 1 and "abandonné" in txt and "stop demande" in txt, "abandon par la carte : (%d, %s)" % (code, txt.strip()))
    pr = [ln.replace(b"proto=1", b"proto=9") for ln in lines]
    code, txt, o = depuis(b"".join(pr), "proto.txt")
    check(code == 1 and "protocole 9 inconnu" in txt, "protocole inconnu : (%d, %s)" % (code, txt.strip()))

    # 11. Décodeur seul : un vidage complet dans des fragments de lignes CR/LF mêlés
    dec = carte_dump.Decoder()
    for ln in lines:
        dec.feed(ln.replace(b"\n", b"\r\n"))
    files = dec.result()
    check(set(files) == {"/vn_hist.bin", "/vn_prof.bin", "/vn_days.bin", "/vn_stations.csv"}, "décodeur : 4 fichiers")
    check(files["/vn_stations.csv"].startswith(b"# vn_stations v1"), "liste des stations : en-tête")

    print("\n%d vérifications réussies, %d échec(s)" % (ok, fail))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
