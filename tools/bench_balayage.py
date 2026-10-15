#!/usr/bin/env python3
"""Balayage d'un réglage de l'alerte « inhabituel » : une ligne par valeur, moyenne sur graines × mondes.

    python3 tools/bench_balayage.py --base "" --var "alerte_disp=1 alerte_k={}" --valeurs 1,1.5,2,3
    python3 tools/bench_balayage.py --var "inertie={}" --valeurs 1,2,3,4

Chaque valeur est comparée à la référence (--base) sur les MÊMES mondes (écarts appariés). Les
mesures de l'alerte sont rapportées sur la période mûre (j ≥ 57), comme les fausses alertes ;
« rappel » est aussi donné sur toute la période (définition historique du banc).
"""
import argparse
import concurrent.futures as cf
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_campagne import run, leviers  # noqa: E402

COLS = [("fausses", "fausses/j", 1), ("fausses_pluie", "pluie", 1), ("fausses_sec", "sec", 1),
        ("fausses_ferie", "férié", 1), ("vraies", "vraies/j", 2), ("rappel", "rappel (tout) %", 1),
        ("rappel_mur", "rappel %", 1), ("rappel_mur_pluie", "rappel pluie %", 1), ("rappel_pluie", "rappel pluie (tout) %", 1),
        ("latence", "retard moy. (min)", 1), ("latence_p90", "retard p90", 0), ("mae60", "MAE +60", 4)]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--graines", default="1,2,3")
    p.add_argument("--effets", default="nul,moyen,fort")
    p.add_argument("--jours", default="168")
    p.add_argument("--base", default="")
    p.add_argument("--var", required=True, help="leviers, {} = la valeur balayée")
    p.add_argument("--valeurs", required=True)
    o = p.parse_args()
    seeds, effs, vals = o.graines.split(","), o.effets.split(","), o.valeurs.split(",")
    jobs = {}
    with cf.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for e in effs:
            for g in seeds:
                common = ["--jours", o.jours, "--graine", g, "--effet", e, "--court"] + leviers(o.base)
                jobs[("ref", e, g)] = ex.submit(run, common)
                for v in vals:
                    jobs[(v, e, g)] = ex.submit(run, common + leviers(o.var.replace("{}", v)))
        res = {k: f.result() for k, f in jobs.items()}
    runs = [(e, g) for e in effs for g in seeds]
    print(f"### balayage [{o.var}] — référence [{o.base or 'défaut'}], {o.jours} jours, graines {o.graines} × mondes {o.effets}\n")
    print("| réglage | " + " | ".join(c[1] for c in COLS) + " |")
    print("|---|" + "---|" * len(COLS))
    for v in ["ref"] + vals:
        cells = []
        for key, _, nd in COLS:
            xs = [float(res[(v, e, g)][key]) for e, g in runs]
            cells.append(f"{statistics.mean(xs):.{nd}f}")
        print(f"| {'référence' if v == 'ref' else o.var.replace('{}', v)} | " + " | ".join(cells) + " |")
    print()
    for e in effs:   # détail par monde : fausses/j et rappel mûr
        line = [f"{e}:"]
        for v in ["ref"] + vals:
            f = statistics.mean(float(res[(v, e, g)]["fausses"]) for g in seeds)
            r = statistics.mean(float(res[(v, e, g)]["rappel_mur"]) for g in seeds)
            line.append(f"{'réf' if v == 'ref' else v} {f:.1f}/{r:.1f}%")
        print("  ".join(line))


if __name__ == "__main__":
    sys.exit(main())
