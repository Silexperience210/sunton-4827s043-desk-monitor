// Génère src/ui/icons/vn_icons.c|.h depuis le jeu d'icônes VelohNav
// (src/ui/icons.jsx du dépôt d'origine : grille 24, trait 1.75, bouts ronds)
// + quelques icônes « ops » dessinées dans le même style.
// Sortie : images LVGL 8 LV_IMG_CF_ALPHA_8BIT (recolorables via img_recolor).
//
// Usage : node tools/gen_icons.mjs <chemin/vers/velohnav/src/ui/icons.jsx> <dossier_sortie>
import { readFileSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
const require = createRequire(process.env.RESVG_FROM ?? import.meta.url);
const { Resvg } = require("@resvg/resvg-js");

const [, , jsxPath, outDir] = process.argv;
const jsx = readFileSync(jsxPath, "utf8");

// ── Extraction des icônes du dépôt (JSX → SVG) ─────────────────────
const body = jsx.slice(jsx.indexOf("const P = {"), jsx.indexOf("};", jsx.indexOf("const P = {")));
const icons = {};
for (const line of body.split("\n")) {
  const m = line.match(/^\s{2}([a-zA-Z]+):\s*(<.*>),\s*$/);
  if (!m) continue;
  let svg = m[2].replace(/^<>/, "").replace(/<\/>$/, "");
  svg = svg.replace(/strokeWidth=/g, "stroke-width=").replace(/strokeDasharray=/g, "stroke-dasharray=")
           .replace(/strokeLinecap=/g, "stroke-linecap=").replace(/strokeLinejoin=/g, "stroke-linejoin=");
  icons[m[1]] = svg;
}

// ── Icônes « ops » (même grammaire graphique) ──────────────────────
Object.assign(icons, {
  dashboard: `<rect x="3.5" y="3.5" width="7" height="9" rx="1.5"/><rect x="13.5" y="3.5" width="7" height="5" rx="1.5"/><rect x="13.5" y="11.5" width="7" height="9" rx="1.5"/><rect x="3.5" y="15.5" width="7" height="5" rx="1.5"/>`,
  activity:  `<path d="M3 12h4l3-7 4 14 3-7h4"/>`,
  wrench:    `<path d="M14.5 4.2a4.8 4.8 0 0 0-5.6 6.4L3.8 15.7a2 2 0 0 0 2.8 2.8l5.1-5.1a4.8 4.8 0 0 0 6.4-5.6l-2.9 2.9-2.4-.4-.4-2.4 2.1-3.7Z"/>`,
  truck:     `<path d="M3 6.5h11v10H3zM14 10h3.8l3.2 3.4v3.1h-7"/><circle cx="7" cy="17.5" r="1.8"/><circle cx="17.5" cy="17.5" r="1.8"/>`,
  list:      `<path d="M9 6h11M9 12h11M9 18h11M4.5 6h.01M4.5 12h.01M4.5 18h.01"/>`,
  bell:      `<path d="M6 16v-5a6 6 0 0 1 12 0v5l1.5 2h-15L6 16Z"/><path d="M10 20.5a2 2 0 0 0 4 0"/>`,
  trendUp:   `<path d="m3 17 6-6 4 4 8-8M15 7h6v6"/>`,
  trendDown: `<path d="m3 7 6 6 4-4 8 8M15 17h6v-6"/>`,
  power:     `<path d="M12 3v8M6.3 6.8a8 8 0 1 0 11.4 0"/>`,
  moon:      `<path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5Z"/>`,
  lock:      `<rect x="5" y="10.5" width="14" height="10" rx="2"/><path d="M8 10.5v-3a4 4 0 0 1 8 0v3"/>`,
  gauge:     `<path d="M4.5 17a8 8 0 1 1 15 0"/><path d="m12 13 3.5-4"/><circle cx="12" cy="13" r="1.2"/>`,
  sort:      `<path d="M7 4v16m0 0-3-3m3 3 3-3M17 20V4m0 0-3 3m3-3 3 3"/>`,
  target:    `<circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="4.5"/><circle cx="12" cy="12" r=".8"/>`,
  history:   `<path d="M3.5 12a8.5 8.5 0 1 0 2.6-6.1L3.5 8.5M3.5 4v4.5H8"/><path d="M12 7.5V12l3 2"/>`,
  grid:      `<path d="M4 4h16v16H4zM4 12h16M12 4v16"/>`,
  eye:       `<path d="M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12Z"/><circle cx="12" cy="12" r="3"/>`,
  ban:       `<circle cx="12" cy="12" r="8.5"/><path d="m6 6 12 12"/>`,
});

const SIZES = [12, 16, 20];
const NAMES = Object.keys(icons).sort();

function render(name, size) {
  const sw = size <= 12 ? 2.2 : size <= 16 ? 2 : 1.85;
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="${sw}" stroke-linecap="round" stroke-linejoin="round">${icons[name]}</svg>`;
  const img = new Resvg(svg, { fitTo: { mode: "width", value: size }, shapeRendering: 2 }).render();
  const px = img.pixels; // RGBA
  const a = new Uint8Array(size * size);
  for (let i = 0; i < size * size; i++) a[i] = px[i * 4 + 3];
  return a;
}

const cName = n => n.replace(/[A-Z]/g, c => "_" + c.toLowerCase());
let c = `// Généré par tools/gen_icons.mjs — ne pas éditer à la main.\n// Icônes : VelohNav (src/ui/icons.jsx) + ops. Format LV_IMG_CF_ALPHA_8BIT.\n#include "lvgl.h"\n\n`;
let h = `// Généré par tools/gen_icons.mjs — ne pas éditer à la main.\n#pragma once\n#include "lvgl.h"\n\n#ifdef __cplusplus\nextern "C" {\n#endif\n\n`;
for (const n of NAMES) {
  for (const s of SIZES) {
    const a = render(n, s);
    const id = `ic_${cName(n)}_${s}`;
    const bytes = Array.from(a, v => "0x" + v.toString(16).padStart(2, "0"));
    const rows = [];
    for (let i = 0; i < bytes.length; i += s) rows.push("  " + bytes.slice(i, i + s).join(","));
    c += `static const uint8_t ${id}_map[] = {\n${rows.join(",\n")}\n};\n`;
    c += `const lv_img_dsc_t ${id} = {\n  .header = { .cf = LV_IMG_CF_ALPHA_8BIT, .always_zero = 0, .reserved = 0, .w = ${s}, .h = ${s} },\n  .data_size = ${s * s},\n  .data = ${id}_map,\n};\n\n`;
    h += `extern const lv_img_dsc_t ${id};\n`;
  }
}

// ── Logo VelohNav (roue + éclair en dégradé) — vraie couleur + alpha ──
const LOGO = `<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#FFB04A"/><stop offset="1" stop-color="#FF4A1C"/></linearGradient></defs><circle cx="12" cy="12" r="9.25" fill="none" stroke="url(#g)" stroke-width="2"/><path d="M13.2 5.5 8 13h3.6l-.8 5.5L16 11h-3.6l.8-5.5Z" fill="url(#g)"/>`;
for (const s of [22, 56]) {
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${s}" height="${s}" viewBox="0 0 24 24">${LOGO}</svg>`;
  const px = new Resvg(svg, { fitTo: { mode: "width", value: s } }).render().pixels;
  const out = [];
  for (let i = 0; i < s * s; i++) {
    const r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2], al = px[i * 4 + 3];
    // resvg rend en alpha prémultiplié : on dé-prémultiplie avant la conversion RGB565
    const un = v => (al ? Math.min(255, Math.round(v * 255 / al)) : 0);
    const c565 = ((un(r) >> 3) << 11) | ((un(g) >> 2) << 5) | (un(b) >> 3);
    out.push(c565 & 0xff, c565 >> 8, al);
  }
  const id = `img_logo_${s}`;
  const rows = [];
  for (let i = 0; i < out.length; i += 24) rows.push("  " + out.slice(i, i + 24).map(v => "0x" + v.toString(16).padStart(2, "0")).join(","));
  c += `#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP != 0\n#error "logo généré pour RGB565 non permuté"\n#endif\n`;
  c += `static const uint8_t ${id}_map[] = {\n${rows.join(",\n")}\n};\n`;
  c += `const lv_img_dsc_t ${id} = {\n  .header = { .cf = LV_IMG_CF_TRUE_COLOR_ALPHA, .always_zero = 0, .reserved = 0, .w = ${s}, .h = ${s} },\n  .data_size = ${s * s * 3},\n  .data = ${id}_map,\n};\n\n`;
  h += `extern const lv_img_dsc_t ${id};\n`;
}

h += `\n#ifdef __cplusplus\n}\n#endif\n`;
writeFileSync(`${outDir}/vn_icons.c`, c);
writeFileSync(`${outDir}/vn_icons.h`, h);
console.log(`${NAMES.length} icônes × ${SIZES.length} tailles + logo → ${outDir}`);
console.log(NAMES.join(" "));
