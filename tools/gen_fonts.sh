#!/usr/bin/env bash
# Génère les polices LVGL (src/ui/fonts/*.c) avec lv_font_conv.
# Typographie VelohNav : sans = Inter (system-ui), mono = JetBrains Mono.
# Usage : INTER_DIR=/chemin/inter JBM_DIR=/chemin/jetbrains ./tools/gen_fonts.sh
set -euo pipefail
OUT="$(cd "$(dirname "$0")/.." && pwd)/src/ui/fonts"
CONV="${LV_FONT_CONV:-lv_font_conv}"
INTER_DIR="${INTER_DIR:-/usr/share/fonts/opentype/inter}"
JBM_DIR="${JBM_DIR:-./fonts}"
mkdir -p "$OUT"

# Latin de base + Latin-1 (accents FR) + œ + ponctuation typographique,
# flèches, ≈ ≤ ≥ − €, puces / triangles.
COMMON="0x20-0x7E,0xA0-0xFF,0x152-0x153,0x2013-0x2014,0x2019,0x201C-0x201D,0x2022,0x2026,0x2190-0x2193,0x2197-0x2198,0x2212,0x2248,0x2264-0x2265,0x25B2,0x25BC,0x25CF,0x20AC"
SANS_RANGE="$COMMON,0x202F"   # espace fine insécable (séparateur de milliers FR)
MONO_RANGE="$COMMON"

gen() { # nom fichier taille plage
  "$CONV" --no-compress --no-prefilter --bpp 4 --size "$3" --font "$2" -r "$4" \
    --format lvgl --lv-include lvgl.h --lv-font-name "$1" -o "$OUT/$1.c"
  echo "  $1 ($3 px)"
}

echo "Polices → $OUT"
gen vn_inter_11   "$INTER_DIR/Inter-Medium.otf"     11 "$SANS_RANGE"
gen vn_inter_12   "$INTER_DIR/Inter-Regular.otf"    12 "$SANS_RANGE"
gen vn_inter_13s  "$INTER_DIR/Inter-SemiBold.otf"   13 "$SANS_RANGE"
gen vn_inter_15b  "$INTER_DIR/Inter-Bold.otf"       15 "$SANS_RANGE"
gen vn_inter_20b  "$INTER_DIR/Inter-Bold.otf"       20 "$SANS_RANGE"
gen vn_inter_26x  "$INTER_DIR/Inter-ExtraBold.otf"  26 "$SANS_RANGE"
gen vn_mono_10    "$JBM_DIR/JetBrainsMono-SemiBold.ttf" 10 "$MONO_RANGE"
gen vn_mono_12b   "$JBM_DIR/JetBrainsMono-Bold.ttf"     12 "$MONO_RANGE"
gen vn_mono_20b   "$JBM_DIR/JetBrainsMono-ExtraBold.ttf" 20 "$MONO_RANGE"
