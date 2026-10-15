#!/usr/bin/env python3
"""Vérifie le mode « données réelles » du banc AVANT d'aller sur la carte.

Une « carte simulée » (le monde du banc, vidé chaque jour à 6 h avec le MÊME instantané que la
commande « dump » : bench_prevision --exporter) fournit un historique « réel » de 9 semaines,
avec sa vérité (incidents injectés, types de stations). Le mode --reel le rejoue et doit :
  - reproduire le banc simulé sur la même période (même modèle, mêmes données) ;
  - dire « trop court pour conclure » sur une journée, et signaler la fuite d'un vidage unique ;
  - signaler un trou, ignorer proprement un fichier tronqué ou faux ;
  - retrouver les incidents injectés (vérité construite) et la forme des profils (piège compris).

  cd sim && make bench_prevision && cd .. && python3 tools/test_bench_reel.py   (~2 min)
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(ROOT, "sim")
BENCH = os.path.join(SIM, "bench_prevision")
ok = fail = 0


def check(cond, msg):
    global ok, fail
    if cond:
        ok += 1
    else:
        fail += 1
        print("ÉCHEC : " + msg)


def run(args):
    r = subprocess.run([BENCH] + args, cwd=SIM, capture_output=True, text=True, timeout=600)
    return r.returncode, r.stdout + r.stderr


def synth(txt, tag="SYNTHESE_REEL"):
    m = re.search(tag + r" (.*)", txt)
    if not m:
        return {}
    d = {}
    for kv in m.group(1).split():
        k, v = kv.split("=", 1)
        try:
            d[k] = float(v)
        except ValueError:
            d[k] = v
    return d


def main():
    if not os.path.exists(BENCH):
        sys.exit("construire d'abord : cd sim && make bench_prevision")
    tmp = tempfile.mkdtemp(prefix="vn_test_reel_")
    exp = os.path.join(tmp, "carte")
    code, txt_sim = run(["--jours", "63", "--court", "--exporter", exp])
    check(code == 0, "export de la carte simulée (%d)" % code)
    S = synth(txt_sim, "SYNTHESE")
    arch = os.path.join(exp, "archive")
    dumps = sorted(os.listdir(arch))
    check(len(dumps) == 63 and all(os.path.exists(os.path.join(arch, d, f)) for d in dumps
                                   for f in ("vn_hist.bin", "vn_prof.bin", "vn_days.bin", "vn_stations.csv")),
          "63 vidages quotidiens complets (%d)" % len(dumps))
    first_prof = os.path.join(arch, dumps[0], "vn_prof.bin")
    verite = os.path.join(exp, "verite")

    # 1. Plusieurs semaines : toute la chaîne, contre le banc simulé et contre la vérité
    code, txt = run(["--reel", arch, first_prof, "--verite", verite, "--mur", "31"])
    R = synth(txt)
    check(code == 0 and R, "plusieurs semaines : rapport (%d)" % code)
    check(R.get("jours", 0) >= 60 and R.get("trous") == 0 and R.get("fuite") == 0, "plusieurs semaines : 62 jours, aucun trou, aucune fuite (%s)" % R)
    if S and R:
        rel = abs(R["mae60_mur"] - S["mae60"]) / S["mae60"]
        check(rel < 0.015, "le rejeu reproduit le banc simulé : MAE +60 mûre %.4f contre %.4f (%.1f %%)" % (R["mae60_mur"], S["mae60"], 100 * rel))
        check(R["prof60"] < R["mae60"], "la tendance coûte aussi au rejeu (profil seul %.4f < modèle %.4f), comme au banc" % (R["prof60"], R["mae60"]))
    check("écart **significatif**" in txt and "jours, 2 erreurs-types" in txt, "plusieurs semaines : verdicts tranchés")
    check("trop court pour conclure" not in txt.split("## 2.")[1].split("## 3.")[0], "plusieurs semaines : pas de « trop court » sur les comparaisons")
    check(R.get("verite_rappel", 0) >= 85, "vérité construite : ≥ 85 %% des incidents injectés retrouvés (%.1f)" % R.get("verite_rappel", -1))
    check(R.get("formes_ok", 0) >= 90, "forme des profils : ≥ 90 %% conformes au type vrai (%.1f)" % R.get("formes_ok", -1))
    check(R.get("piege") == 1, "station piège (résidentielle nommée « Parc ») signalée")
    check("20 → 30 bornes" in txt, "agrandissement repéré dans les listes des stations")
    check(R.get("jours_feries") == 2, "deux jours fériés évalués (lundi de Pâques, 1er mai)")
    check("| 10 (mer 11/03) | 100 % |" in txt, "maturation : couverture complète au 10e jour")

    # 2. Une journée, profil du même vidage : fuite signalée, trop court
    d = os.path.join(arch, dumps[44])
    code, txt1 = run(["--reel", os.path.join(d, "vn_hist.bin"), os.path.join(d, "vn_prof.bin")])
    R1 = synth(txt1)
    check(code == 0 and R1.get("jours") == 1 and R1.get("fuite") == 1 and "**FUITE**" in txt1, "une journée : fuite signalée (%s)" % R1)
    check(txt1.count("trop court pour conclure") >= 3 and "Historique trop court pour conclure" in txt1, "une journée : « trop court pour conclure »")
    #    … même journée, profil du vidage précédent : mesure propre, plus mauvaise (la fuite flattait)
    code, txt2 = run(["--reel", os.path.join(d, "vn_hist.bin"), os.path.join(arch, dumps[43], "vn_prof.bin")])
    R2 = synth(txt2)
    check(code == 0 and R2.get("fuite") == 0 and "FUITE" not in txt2, "une journée propre : pas de fuite")
    if R1 and R2:
        check(R2["mae60"] > R1["mae60"] * 1.03, "la fuite flatte la MAE +60 (%.4f propre contre %.4f)" % (R2["mae60"], R1["mae60"]))

    # 3. Un trou de deux jours (deux vidages oubliés) et des fichiers abîmés
    holes = os.path.join(tmp, "trou")
    shutil.copytree(arch, holes, ignore=lambda dd, names: [n for n in names if n in (dumps[20], dumps[21])])
    bad = os.path.join(holes, dumps[30], "vn_hist.bin")
    data = open(bad, "rb").read()
    open(bad, "wb").write(data[: len(data) // 2])              # tronqué : refusé par Model::load
    open(os.path.join(holes, dumps[31], "vn_hist.bin"), "wb").write(b"pas un historique" * 10)
    code, txt3 = run(["--reel", holes, first_prof])
    R3 = synth(txt3)
    check(code == 0 and R3.get("trous") == 2, "trous : deux trous signalés (vidages oubliés, fichiers refusés) (%s)" % R3.get("trous"))
    check("2 refusé(s)" in txt3 and "tronqué" in txt3 and "pas un /vn_hist.bin" in txt3, "fichiers abîmés : refusés et nommés")
    check(re.search(r"\(48\.\d h\)", txt3) is not None, "trou de 48 h chiffré")
    check(R3.get("jours", 0) >= 55, "après les trous, le rejeu continue (%s jours)" % R3.get("jours"))

    # 4. Profil illisible : départ de zéro, dit en clair
    badp = os.path.join(tmp, "prof_faux.bin")
    open(badp, "wb").write(open(first_prof, "rb").read()[:1000])
    code, txt4 = run(["--reel", os.path.join(d, "vn_hist.bin"), badp])
    check(code == 0 and "profil refusé" in txt4 and "de zéro" in txt4, "profil illisible : départ de zéro signalé")

    # 5. Rien d'exploitable
    empty = os.path.join(tmp, "vide")
    os.makedirs(empty)
    code, txt5 = run(["--reel", empty, "-"])
    check(code == 1 and "aucun vn_hist.bin" in txt5, "dossier vide : message clair (%d)" % code)
    junk = os.path.join(tmp, "faux.bin")
    open(junk, "wb").write(b"x" * 100)
    code, txt6 = run(["--reel", junk, "-"])
    check(code == 1 and "AUCUNE donnée exploitable" in txt6, "historique faux seul : message clair (%d)" % code)

    # 6. Comparaison appariée d'une variante sur les mêmes données
    code, txt7 = run(["--reel", arch, first_prof, "--levier", "profil_n=12"])
    check(code == 0 and "Variante « profil_n=12 »" in txt7 and "fausses alertes / jour" in txt7, "variante appariée : section présente")

    # 7. Toute la chaîne avec les VRAIES stations (docs/donnees) : émetteur simulé → outil PC → banc
    dsim = os.path.join(SIM, "dump_sim")
    if os.path.exists(dsim):
        p = subprocess.run([dsim], cwd=SIM, input=b"dump\n", capture_output=True, timeout=60)
        cap = os.path.join(tmp, "capture.txt")
        open(cap, "wb").write(p.stdout)
        out = os.path.join(tmp, "donnees-carte")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "carte_dump.py"), "--depuis", cap, "--sortie", out, "-q"],
                           capture_output=True, text=True)
        check(r.returncode == 0, "chaîne complète : vidage relu (%s)" % (r.stdout + r.stderr).strip())
        code, txt8 = run(["--reel", os.path.join(out, "archive"), os.path.join(out, "vn_prof.bin")])
        check(code == 0 and "listes des stations du vidage" in txt8 and "Historique trop court pour conclure" in txt8
              and re.search(r"- 14\d stations", txt8) is not None, "chaîne complète : banc sur les vraies stations, « trop court » (%d)" % code)
    else:
        print("(sim/dump_sim absent : chaîne complète non vérifiée)")

    print("\n%d vérifications réussies, %d échec(s)" % (ok, fail))
    shutil.rmtree(tmp, ignore_errors=True)
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
