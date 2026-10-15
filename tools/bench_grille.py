#!/usr/bin/env python3
"""Grille de réglages du banc de prévision : une ligne par réglage, écarts appariés à une référence.

    python3 tools/bench_grille.py --jours 365 --ref ref \\
        --config "ref=ferie_dimanche=0 ferie_appris=0" --config "b+dimanche=" \\
        --cles ferie_mur60,ordinaire60,fausses_ferie_mur

    python3 tools/bench_grille.py --config "N=12=profil_n=12" --config "N=24=" --ref N=24 --absorption

Un réglage s'écrit « nom=leviers » (leviers séparés par des espaces, vide = réglages par défaut du
modèle). Chaque réglage tourne sur les MÊMES mondes (graines × effets météo, options --extra) : la
différence avec la référence est appariée. Les exécutions sont mises en cache (/tmp/vn_bench_cache.json)
pour qu'un tableau se recompose sans tout relancer.

--absorption : courbe de l'agrandissement 20 → 30 bornes, d'après le biais SIGNÉ du profil de la
station agrandie contre sa jumelle (moyenne des mondes) : jours jusqu'à 50 % et 90 % d'absorption, et la
demi-vie théorique de la décroissance (N·ln 2 relevés, 3 relevés par semaine et par créneau).
"""
import argparse
import concurrent.futures as cf
import json
import math
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_campagne import run, leviers  # noqa: E402

CACHE = "/tmp/vn_bench_cache.json"


def load_cache():
    try:
        with open(CACHE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--graines", default="1,2,3")
    p.add_argument("--effets", default="nul,moyen,fort")
    p.add_argument("--jours", default="168")
    p.add_argument("--extra", default="", help="options du banc communes à tous les réglages")
    p.add_argument("--config", action="append", required=True, help="nom=leviers")
    p.add_argument("--ref", help="nom du réglage de référence (défaut : le premier)")
    p.add_argument("--cles", default="mae60,ordinaire60,ferie_mur60,fausses_ordinaire,fausses_ferie_mur,rappel_mur")
    p.add_argument("--absorption", action="store_true")
    p.add_argument("--sans-cache", action="store_true")
    o = p.parse_args()
    seeds, effs = o.graines.split(","), o.effets.split(",")
    confs = []
    for c in o.config:
        name, _, lev = c.partition("=")
        confs.append((name, lev))
    ref = o.ref or confs[0][0]
    cache = {} if o.sans_cache else load_cache()
    jobs = {}
    with cf.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for name, lev in confs:
            for e in effs:
                for g in seeds:
                    args = ["--jours", o.jours, "--graine", g, "--effet", e, "--court"] + o.extra.split() + leviers(lev)
                    key = " ".join(args)
                    if key in cache:
                        continue
                    jobs[key] = ex.submit(run, args)
        done = 0
        for f in cf.as_completed(jobs.values()):      # cache écrit au fil de l'eau : une campagne coupée reprend
            key = next(k for k, v in jobs.items() if v is f)
            cache[key] = f.result()
            done += 1
            if done % 4 == 0 or done == len(jobs):
                tmp = CACHE + ".tmp"
                with open(tmp, "w") as fo:
                    json.dump(cache, fo)
                os.replace(tmp, CACHE)

    def res(lev, e, g):
        return cache[" ".join(["--jours", o.jours, "--graine", g, "--effet", e, "--court"] + o.extra.split() + leviers(lev))]

    runs = [(e, g) for e in effs for g in seeds]
    levOf = dict(confs)
    keys = o.cles.split(",")
    print(f"### grille — {o.jours} jours, graines {o.graines} × mondes {o.effets}"
          + (f", options « {o.extra} »" if o.extra else "") + f" ; écart apparié à « {ref} »\n")
    print("| réglage | " + " | ".join(keys) + " |")
    print("|---|" + "---|" * len(keys))
    for name, lev in confs:
        cells = []
        for k in keys:
            xs = [float(res(lev, e, g)[k]) for e, g in runs]
            rs = [float(res(levOf[ref], e, g)[k]) for e, g in runs]
            ok = [(x, r) for x, r in zip(xs, rs) if not (math.isnan(x) or math.isnan(r))]
            if not ok:
                cells.append("—")
                continue
            m = statistics.mean(x for x, _ in ok)
            nd = 4 if m < 10 else 1
            if name == ref:
                cells.append(f"{m:.{nd}f}")
            else:
                d = [x - r for x, r in ok]
                mr = statistics.mean(r for _, r in ok)
                sd = statistics.stdev(d) if len(d) > 1 else 0.0
                rel = 100 * statistics.mean(d) / mr if mr else 0.0
                cells.append(f"{m:.{nd}f} ({rel:+.1f} % ± {100 * sd / abs(mr) / math.sqrt(len(d)) if mr else 0:.1f})")
        print(f"| {name} | " + " | ".join(cells) + " |")
    print("\n(entre parenthèses : écart relatif apparié moyen ± son erreur-type sur les mondes)\n")

    if o.absorption:
        print("| réglage | biais à j0 | j+7 | j+14 | j+28 | j+56 | j+84 | moitié absorbée (j) | 90 % absorbés (j) | demi-vie théorique (j) |")
        print("|---|---|---|---|---|---|---|---|---|---|")
        for name, lev in confs:
            curve = []
            for a in range(0, 109):
                xs = [float(res(lev, e, g).get(f"biais_j{a}", "nan")) for e, g in runs]
                xs = [x for x in xs if not math.isnan(x)]
                curve.append(statistics.mean(xs) if xs else float("nan"))
            b0 = curve[0]

            def reach(frac):
                # premier jour où le biais moyen (lissé sur 7 jours) passe sous frac × b0, en valeur absolue
                for a in range(len(curve)):
                    w = [c for c in curve[max(0, a - 3):a + 4] if not math.isnan(c)]
                    if w and abs(statistics.mean(w)) <= frac * abs(b0):
                        return a
                return None
            h, n90 = reach(0.5), reach(0.1)
            nval = 24
            for tok in lev.split():
                if tok.startswith("profil_n="):
                    nval = int(tok.split("=")[1])
            theo = nval * math.log(2) / 3 * 7
            pts = " | ".join(f"{curve[a]:+.2f}" for a in (0, 7, 14, 28, 56, 84))
            print(f"| {name} | {pts} | {h if h is not None else '> 108'} | {n90 if n90 is not None else '> 108'} | {theo:.0f} |")
        print()


if __name__ == "__main__":
    sys.exit(main())
