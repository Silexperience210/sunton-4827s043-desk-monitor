#!/usr/bin/env python3
"""Campagne du banc de prévision : graines × intensités de l'effet météo, en parallèle.

    python3 tools/bench_campagne.py                         # référence (réglages par défaut du modèle)
    python3 tools/bench_campagne.py --levier tol_meteo=1    # un levier, comparé apparié à la référence
    python3 tools/bench_campagne.py --base "tol_meteo=0" --levier "tol_meteo=1"

Chaque exécution de sim/bench_prevision imprime une ligne « SYNTHESE clé=valeur… ».
À graine et effet égaux, la vérité simulée est identique : la différence levier − référence
est appariée (même monde, seul le modèle change), d'où des écarts lisibles même petits.
"""
import argparse
import concurrent.futures as cf
import os
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.join(HERE, "..", "sim", "bench_prevision")

KEYS = [
    ("mae15", "MAE +15", 3), ("mae30", "MAE +30", 3), ("mae60", "MAE +60", 3), ("mae120", "MAE +120", 3),
    ("mae60_sec", "MAE +60 sec", 3), ("mae60_pluie", "MAE +60 pluie", 3), ("mae120_pluie", "MAE +120 pluie", 3),
    ("orc60_pluie", "oracle +60 pluie", 3), ("orc60", "oracle +60", 3), ("per60", "persistance +60", 3), ("ferie60", "MAE +60 fériés", 3),
    ("fausses_pluie", "fausses/j pluie", 1), ("fausses_sec", "fausses/j sec", 1), ("rappel", "rappel incidents %", 1),
    ("rappel_pluie", "rappel jours de pluie %", 1), ("rappel_sec", "rappel jours secs %", 1),
    ("annonce_vide", "ruptures annoncées %", 1), ("annonce_pleine", "saturations annoncées %", 1),
    ("annonces_pluie_j", "annonces rupture+saturation / j de pluie", 1),
    ("precision_pluie", "précision des annonces, pluie %", 1), ("precision_sec", "précision des annonces, sec %", 1),
]


def run(args):
    out = subprocess.run([BENCH] + args, capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        if line.startswith("SYNTHESE"):
            return {k: v for k, v in (kv.split("=", 1) for kv in line.split()[1:])}
    raise RuntimeError("pas de ligne SYNTHESE :\n" + out[-2000:])


def leviers(spec):
    a = []
    for x in spec.split():
        a += ["--levier", x]
    return a


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--graines", default="1,2,3")
    p.add_argument("--effets", default="nul,moyen,fort")
    p.add_argument("--jours", default="168")
    p.add_argument("--base", default="", help="leviers de la référence (« a=1 b=0 »)")
    p.add_argument("--levier", default="", help="leviers comparés (« a=1 »)")
    p.add_argument("--extra", default="", help="options supplémentaires passées aux deux côtés")
    o = p.parse_args()
    seeds = o.graines.split(",")
    effs = o.effets.split(",")
    extra = o.extra.split()
    jobs = {}
    with cf.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for e in effs:
            for g in seeds:
                common = ["--jours", o.jours, "--graine", g, "--effet", e, "--court"] + extra
                jobs[("base", e, g)] = ex.submit(run, common + leviers(o.base))
                if o.levier:
                    jobs[("lev", e, g)] = ex.submit(run, common + leviers(o.base) + leviers(o.levier))
        res = {k: f.result() for k, f in jobs.items()}

    title = f"référence [{o.base or 'défaut'}]" + (f" → levier [{o.levier}]" if o.levier else "")
    print(f"### {title} — {o.jours} jours, graines {o.graines}\n")
    for e in effs:
        print(f"**Effet météo « {e} »**\n")
        if o.levier:
            print("| mesure | référence | levier | écart apparié (moyenne ± écart-type sur les graines) |")
            print("|---|---|---|---|")
        else:
            print("| mesure | moyenne | min | max |")
            print("|---|---|---|---|")
        for key, name, nd in KEYS:
            b = [float(res[("base", e, g)][key]) for g in seeds]
            if o.levier:
                l = [float(res[("lev", e, g)][key]) for g in seeds]
                d = [y - x for x, y in zip(b, l)]
                sd = statistics.stdev(d) if len(d) > 1 else 0.0
                rel = (100 * statistics.mean(d) / statistics.mean(b)) if statistics.mean(b) else 0.0
                print(f"| {name} | {statistics.mean(b):.{nd}f} | {statistics.mean(l):.{nd}f} | "
                      f"{statistics.mean(d):+.{nd}f} ± {sd:.{nd}f} ({rel:+.1f} %) |")
            else:
                print(f"| {name} | {statistics.mean(b):.{nd}f} | {min(b):.{nd}f} | {max(b):.{nd}f} |")
        print()


if __name__ == "__main__":
    sys.exit(main())
