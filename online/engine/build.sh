#!/usr/bin/env bash
# ── Construit les deux modules WebAssembly de VelohNav Ops en ligne ───
#   vn-core.wasm : le cœur du firmware (src/core) + l'API du moteur — utilisé par le serveur ;
#   vn-twin.wasm : le même cœur + LVGL 8.3 + l'UI du firmware (src/ui) — le jumeau d'écran.
# Compilateur : clang ≥ 17 avec la cible wasm32-wasi ; bibliothèque C : wasi-sysroot (wasi-sdk 22),
# téléchargée dans online/engine/.toolchain au premier lancement. Dépendances du firmware :
# LVGL v8.3.11 et ArduinoJson v7 dans .deps/ (comme le simulateur).
#
#   online/engine/build.sh            # → online/public/vn-core.wasm, online/public/vn-twin.wasm
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
TC="$HERE/.toolchain"
OUT="$ROOT/online/public"
BUILD="$HERE/build"
CLANG="${CLANG:-clang}"
CLANGXX="${CLANGXX:-clang++}"
WASI_VER=22
mkdir -p "$TC" "$BUILD" "$OUT"

deps() {
  [ -d "$ROOT/.deps/lvgl" ] || git clone -q -b v8.3.11 --depth 1 https://github.com/lvgl/lvgl.git "$ROOT/.deps/lvgl"
  [ -d "$ROOT/.deps/ArduinoJson" ] || git clone -q -b v7.4.2 --depth 1 https://github.com/bblanchon/ArduinoJson.git "$ROOT/.deps/ArduinoJson"
}

toolchain() {
  if [ ! -d "$TC/wasi-sysroot" ]; then
    curl -sSL "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-$WASI_VER/wasi-sysroot-$WASI_VER.0.tar.gz" | tar xz -C "$TC"
  fi
  if [ ! -f "$TC/resource/lib/wasi/libclang_rt.builtins-wasm32.a" ]; then
    mkdir -p "$TC/resource"
    curl -sSL "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-$WASI_VER/libclang_rt.builtins-wasm32-wasi-$WASI_VER.0.tar.gz" | tar xz -C "$TC/resource"
    # en-têtes intégrés du compilateur (stddef.h, stdarg.h…) : ceux du clang installé
    ln -sfn "$("$CLANG" -print-resource-dir)/include" "$TC/resource/include"
  fi
}

CORE_SRC=( "$ROOT"/src/core/*.cpp "$HERE/platform_wasm.cpp" "$HERE/engine.cpp" )
UI_SRC=( "$ROOT"/src/ui/*.cpp "$ROOT"/src/ui/widgets/*.cpp "$ROOT/src/hal/touch_cal.cpp" "$HERE/twin.cpp" )
UI_C=( "$ROOT"/src/ui/fonts/*.c "$ROOT"/src/ui/icons/*.c )
INC=( -I"$ROOT/include" -I"$ROOT/.deps" -I"$ROOT/.deps/lvgl" -I"$ROOT/.deps/ArduinoJson/src" -I"$ROOT/src" )
DEFS=( -DLV_CONF_INCLUDE_SIMPLE -DVN_ONLINE=1 )
# Mêmes options pour le cœur dans les deux modules : le jumeau doit calculer au bit près comme le serveur.
COMMON=( -O2 -fno-exceptions -fno-rtti -fno-fast-math -ffp-contract=off -Wno-unused-parameter )

wasm() {
  deps; toolchain
  local W=( --target=wasm32-wasi --sysroot="$TC/wasi-sysroot" -resource-dir "$TC/resource" )
  local O="$BUILD/wasm"
  rm -rf "$O"; mkdir -p "$O"
  local JOBS; JOBS="$(nproc 2>/dev/null || echo 4)"
  local core=() ui=() lv=()
  objOf() { echo "$O/$(echo "$1" | sed "s|$ROOT/||; s|/|_|g").o"; }
  compile() {  # compile <src> <c|cxx>, en parallèle (JOBS à la fois)
    local obj; obj="$(objOf "$1")"
    if [ "$2" = c ]; then "$CLANG" "${W[@]}" "${COMMON[@]}" -w "${INC[@]}" "${DEFS[@]}" -c "$1" -o "$obj" &
    else "$CLANGXX" "${W[@]}" "${COMMON[@]}" -std=gnu++17 "${INC[@]}" "${DEFS[@]}" -c "$1" -o "$obj" & fi
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
  }
  for s in "${CORE_SRC[@]}"; do compile "$s" cxx; core+=( "$(objOf "$s")" ); done
  for s in "${UI_SRC[@]}"; do compile "$s" cxx; ui+=( "$(objOf "$s")" ); done
  for s in "${UI_C[@]}"; do compile "$s" c; ui+=( "$(objOf "$s")" ); done
  while IFS= read -r s; do compile "$s" c; lv+=( "$(objOf "$s")" ); done < <(find "$ROOT/.deps/lvgl/src" -name '*.c' | sort)
  while [ "$(jobs -rp | wc -l)" -gt 0 ]; do wait -n || { echo "échec de compilation" >&2; exit 1; }; done
  local LINK=( "${W[@]}" -mexec-model=reactor -Wl,-z,stack-size=1048576 -Wl,--initial-memory=33554432
               -Wl,--strip-debug -lc++ -lc++abi -lm )
  "$CLANGXX" "${core[@]}" "${LINK[@]}" -o "$OUT/vn-core.wasm"
  "$CLANGXX" "${core[@]}" "${ui[@]}" "${lv[@]}" "${LINK[@]}" -o "$OUT/vn-twin.wasm"
  ls -la "$OUT"/vn-*.wasm
}

wasm
