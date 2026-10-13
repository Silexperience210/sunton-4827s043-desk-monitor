#!/usr/bin/env python3
"""Assemble docs/apercu_ecrans.png à partir des captures du simulateur (docs/captures/*.png).
Usage : python3 tools/planche.py <inter_dir> <jetbrains_mono_semibold.ttf>"""
import sys
from PIL import Image, ImageDraw, ImageFont

NAMES = {
    "01_reseau": "Réseau — tableau de bord", "04_carte": "Carte — temps réel",
    "06_carte_prevision_1h": "Carte — prévision +1 h", "09_fiche_station_suite": "Fiche station — prévision & lecture ops",
    "10_fiche_station_prevision": "Fiche station — 24 h / profil / +2 h", "12_stations_echeance": "Stations — tri par échéance",
    "13_alertes": "Alertes & plan de régulation", "14_transferts": "Transferts suggérés",
    "02_reseau_activite": "Réseau — activité & remplissage", "16_reglages_suite": "Réglages — seuils d'alerte",
}

def main():
    inter, mono = sys.argv[1], sys.argv[2]
    W, H, G, CAP, TOP, cols = 480, 272, 14, 26, 64, 2
    sel = list(NAMES)
    rows = (len(sel) + 1) // 2
    sheet = Image.new("RGB", (cols * W + (cols + 1) * G, TOP + rows * (H + CAP + G) + G), (7, 9, 11))
    d = ImageDraw.Draw(sheet)
    fb = ImageFont.truetype(f"{inter}/Inter-Bold.otf", 22)
    fm = ImageFont.truetype(mono, 13)
    fr = ImageFont.truetype(f"{inter}/Inter-Medium.otf", 15)
    d.text((G, 16), "VELOH", font=fb, fill=(236, 239, 244))
    w = d.textlength("VELOH", font=fb)
    d.text((G + w, 16), "NAV", font=fb, fill=(245, 130, 13))
    w2 = d.textlength("NAV", font=fb)
    d.text((G + w + w2 + 12, 22), "OPS · Sunton ESP32-4827S043 · 480×272 · rendu réel du firmware (simulateur LVGL)",
           font=fm, fill=(128, 138, 153))
    for k, n in enumerate(sel):
        r, c = divmod(k, cols)
        x, y = G + c * (W + G), TOP + r * (H + CAP + G)
        d.text((x + 2, y + 3), NAMES[n], font=fr, fill=(169, 177, 189))
        sheet.paste(Image.open(f"docs/captures/{n}.png"), (x, y + CAP))
        d.rectangle([x - 1, y + CAP - 1, x + W, y + CAP + H], outline=(40, 44, 50))
    sheet.save("docs/apercu_ecrans.png")
    print(sheet.size)

if __name__ == "__main__":
    main()
