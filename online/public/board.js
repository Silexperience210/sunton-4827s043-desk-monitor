// ── Tableau de bord : historique long, erreur des prévisions, notifications ─
// Données : /api/history/*, /api/forecast, /api/bench, /api/notify (voir server.mjs). Dans l'aperçu
// autonome, window.VN_DEMO.api(chemin) répond à la place du serveur, avec les mêmes formes.
// Graphiques en SVG faits main (aucune dépendance) : traits fins, grille discrète, réticule et infobulle,
// légende dès deux séries, tableau équivalent sous chaque graphique.

const $ = (id) => document.getElementById(id);
const store = {
  get: (k) => { try { return localStorage.getItem(k); } catch { return null; } },
  set: (k, v) => { try { localStorage.setItem(k, v); } catch { /* navigation privée */ } },
};
const DEMO = typeof window !== 'undefined' && window.VN_DEMO && window.VN_DEMO.api;
async function api(path, opts = {}) {
  if (DEMO) return window.VN_DEMO.api(path, opts);
  const token = store.get('vn.token') || new URLSearchParams(location.search).get('token') || '';
  const r = await fetch(path, { ...opts, headers: { ...(token ? { Authorization: `Bearer ${token}` } : {}), ...(opts.headers || {}) } });
  if (r.status === 401) { const c = $('tokenCard'); if (c) c.hidden = false; throw new Error('jeton requis'); }
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || `HTTP ${r.status}`);
  return j;
}

// ── Formats (français, heure de Luxembourg) ──
const TZ = 'Europe/Luxembourg';
const nf0 = new Intl.NumberFormat('fr-FR', { maximumFractionDigits: 0 });
const nf1 = new Intl.NumberFormat('fr-FR', { minimumFractionDigits: 1, maximumFractionDigits: 1 });
const nf2 = new Intl.NumberFormat('fr-FR', { minimumFractionDigits: 2, maximumFractionDigits: 2 });
const fHour = new Intl.DateTimeFormat('fr-FR', { timeZone: TZ, hour: '2-digit', minute: '2-digit' });
const fDay = new Intl.DateTimeFormat('fr-FR', { timeZone: TZ, weekday: 'short', day: 'numeric', month: 'numeric' });
const fDayShort = new Intl.DateTimeFormat('fr-FR', { timeZone: TZ, day: 'numeric', month: 'numeric' });
const fFull = new Intl.DateTimeFormat('fr-FR', { timeZone: TZ, weekday: 'short', day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' });
const fParts = new Intl.DateTimeFormat('fr-FR', { timeZone: TZ, hour: 'numeric', minute: 'numeric', day: 'numeric', hourCycle: 'h23' });
const lux = (t) => { const p = Object.fromEntries(fParts.formatToParts(new Date(t * 1000)).map((x) => [x.type, x.value])); return { h: +p.hour, mi: +p.minute, d: +p.day }; };
const fmtNum = (v, d = 0) => (v === null || v === undefined || !Number.isFinite(v) ? '—' : (d === 2 ? nf2 : d === 1 ? nf1 : nf0).format(v));
const dayLabel = (iso) => fDay.format(new Date(`${iso}T12:00:00Z`));
const ago = (t) => {
  const s = Math.max(0, Math.floor(Date.now() / 1000 - t));
  if (s < 90) return 'à l\'instant';
  if (s < 3600) return `il y a ${Math.round(s / 60)} min`;
  if (s < 86400) return `il y a ${Math.round(s / 3600)} h`;
  return `il y a ${Math.round(s / 86400)} j`;
};
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };
const SVGNS = 'http://www.w3.org/2000/svg';
const svgEl = (tag, attrs = {}) => { const e = document.createElementNS(SVGNS, tag); for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v); return e; };
const cssVar = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

// ── Échelles ──
function niceTicks(min, max, count = 4) {
  if (!(max > min)) { max = min + 1; }
  const span = max - min;
  const raw = span / count;
  const mag = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((s) => span / s <= count) || 10 * mag;
  const lo = Math.floor(min / step) * step, hi = Math.ceil(max / step) * step;
  const out = [];
  for (let v = lo; v <= hi + step / 1e6; v += step) out.push(+v.toFixed(10));
  return out;
}
// largeur approximative d'un libellé d'axe (11 px, Inter) + marge
const labelWidth = (labels) => Math.max(...labels.map((l) => String(l).length)) * 6.2 + 12;
// décimales des graduations : assez pour que deux graduations ne s'écrivent jamais pareil
const tickDecimals = (yt) => { const st = yt.length > 1 ? yt[1] - yt[0] : 1; return st >= 1 ? 0 : st >= 0.1 ? 1 : 2; };
// graduations de temps en heure locale : toutes les k heures ou tous les k jours, ~6 au plus
function timeTicks(t0, t1, maxTicks) {
  const span = t1 - t0;
  const cands = [[3600, 1], [3600, 2], [3600, 3], [3600, 6], [3600, 12], [86400, 1], [86400, 2], [86400, 7], [86400, 14], [86400, 28]];
  const [unit, k] = cands.find(([u, kk]) => span / (u * kk) <= maxTicks) || cands.at(-1);
  const out = [];
  let t = Math.ceil(t0 / 3600) * 3600;
  let n = 0;
  while (t <= t1 && n++ < 2000) {
    const p = lux(t);
    if (p.mi === 0 && (unit === 3600 ? p.h % k === 0 : p.h === 0 && (Math.round(t / 86400) % k === 0 || k === 1))) out.push(t);
    t += 3600;
  }
  return { ticks: out, daily: unit === 86400 };
}

// ── Infobulle partagée ──
let tipEl = null;
function tip() {
  if (!tipEl) { tipEl = el('div', 'vtip'); tipEl.setAttribute('role', 'status'); document.body.appendChild(tipEl); }
  return tipEl;
}
function showTip(x, y, title, rows) {
  const t = tip();
  t.replaceChildren();
  t.appendChild(el('div', 'vtip__title', title));
  for (const r of rows) {
    const row = el('div', 'vtip__row');
    if (r.color) { const k = el('span', 'vtip__key'); k.style.background = r.color; row.appendChild(k); }
    row.appendChild(el('b', null, r.value));
    row.appendChild(el('span', null, r.label));
    t.appendChild(row);
  }
  t.hidden = false;
  const w = t.offsetWidth, h = t.offsetHeight;
  const vw = document.documentElement.clientWidth;
  let left = x + 14, top = y - h - 10;
  if (left + w > vw - 8) left = x - w - 14;
  if (left < 8) left = 8;
  if (top < window.scrollY + 8) top = y + 16;
  t.style.left = `${left}px`; t.style.top = `${top}px`;
}
const hideTip = () => { if (tipEl) tipEl.hidden = true; };

// ── Légende (dès deux séries) ──
function legend(host, series, kind = 'line') {
  const lg = el('div', 'vlegend');
  for (const s of series) {
    const it = el('span', 'vlegend__item');
    const k = el('span', kind === 'line' ? 'vlegend__line' : 'vlegend__rect');
    k.style.background = s.color;
    it.append(k, document.createTextNode(s.name));
    lg.appendChild(it);
  }
  host.appendChild(lg);
}

// ── Graphique en lignes ──
// opts : x (epochs, ou libellés si cat), series [{name, color, values, area}], yFmt, tipFmt, cat, yMin, height, step (s)
function lineChart(host, opts) {
  host.replaceChildren();
  const series = opts.series.filter((s) => s.values.some((v) => v !== null && v !== undefined));
  if (!opts.x.length || !series.length) { host.appendChild(el('p', 'vempty', opts.empty || 'Pas encore de données.')); return; }
  if (series.length >= 2) legend(host, series, 'line');
  const W = Math.max(280, host.clientWidth || 600);
  const H = opts.height || (W < 520 ? 190 : 230);
  const m = { l: 44, r: opts.endLabel ? 46 : 14, t: 10, b: 26 };
  const iw = W - m.l - m.r, ih = H - m.t - m.b;
  const vals = series.flatMap((s) => s.values).filter((v) => v !== null && v !== undefined && Number.isFinite(v));
  let lo = opts.yMin !== undefined ? Math.min(opts.yMin, ...vals) : Math.min(...vals);
  let hi = Math.max(...vals, opts.yMaxMin ?? -Infinity);
  if (opts.yMax !== undefined) hi = Math.max(hi, opts.yMax);
  const yt = niceTicks(lo, hi, W < 520 ? 3 : 4);
  lo = yt[0]; hi = yt.at(-1);
  const yDec = tickDecimals(yt);
  const cat = !!opts.cat;
  const n = opts.x.length;
  const x0 = cat ? 0 : opts.x[0], x1 = cat ? n - 1 : opts.x.at(-1);
  const X = (i) => m.l + (cat ? (n === 1 ? iw / 2 : (i / (n - 1)) * iw) : x1 === x0 ? iw / 2 : ((opts.x[i] - x0) / (x1 - x0)) * iw);
  const Y = (v) => m.t + ih - ((v - lo) / (hi - lo || 1)) * ih;
  const svg = svgEl('svg', { width: W, height: H, viewBox: `0 0 ${W} ${H}`, class: 'vchart', role: 'img', 'aria-label': opts.label || '' });
  const grid = svgEl('g', { class: 'vgrid' });
  for (const v of yt) {
    grid.appendChild(svgEl('line', { x1: m.l, x2: W - m.r, y1: Y(v), y2: Y(v) }));
    const tx = svgEl('text', { x: m.l - 8, y: Y(v) + 4, 'text-anchor': 'end', class: 'vaxis' });
    tx.textContent = (opts.yFmt || fmtNum)(v, yDec);
    grid.appendChild(tx);
  }
  svg.appendChild(grid);
  // axe des x
  const ax = svgEl('g');
  if (cat) {
    // une graduation sur k, k choisi d'après la largeur des libellés ; la dernière seulement si elle tombe loin
    // de la précédente : jamais deux libellés l'un sur l'autre
    const lw = labelWidth(opts.x);
    // (le premier libellé s'aligne à gauche, le dernier à droite : 1,25 et 1,5 largeurs de marge)
    const k = n > 1 ? Math.max(1, Math.ceil(1.25 * lw / (iw / (n - 1)))) : 1;
    opts.x.forEach((lab, i) => {
      if (i % k !== 0 && !(i === n - 1 && ((n - 1) % k) * (iw / (n - 1)) >= 1.5 * lw)) return;
      if (i % k === 0 && i !== n - 1 && i > 0 && (n - 1 - i) * (iw / (n - 1)) < 1.5 * lw && (n - 1) % k === 0) return;
      const tx = svgEl('text', { x: X(i), y: H - 8, 'text-anchor': i === 0 && n > 1 ? 'start' : i === n - 1 && n > 1 ? 'end' : 'middle', class: 'vaxis' });
      tx.textContent = lab;
      ax.appendChild(tx);
    });
  } else {
    const { ticks, daily } = timeTicks(x0, x1, W < 520 ? 4 : 7);
    for (const t of ticks) {
      const tx = svgEl('text', { x: Math.min(W - m.r - 4, Math.max(m.l + 4, m.l + ((t - x0) / (x1 - x0 || 1)) * iw)), y: H - 8, 'text-anchor': 'middle', class: 'vaxis' });
      tx.textContent = daily ? fDayShort.format(new Date(t * 1000)) : `${lux(t).h} h`;
      ax.appendChild(tx);
    }
  }
  svg.appendChild(ax);
  // séries : un trou de plus d'1,5 pas coupe la ligne
  const step = opts.step || 0;
  for (const s of series) {
    let d = '', area = '', open = false, segStart = 0;
    const lone = [];                                          // points isolés (entre deux trous) : un point, pas un trait
    const closeArea = (iEnd) => {
      if (open && iEnd === segStart) lone.push(iEnd);
      if (s.area && open) area += `L${X(iEnd).toFixed(1)},${Y(Math.max(lo, 0)).toFixed(1)}L${X(segStart).toFixed(1)},${Y(Math.max(lo, 0)).toFixed(1)}Z`;
    };
    let last = -1;
    s.values.forEach((v, i) => {
      if (v === null || v === undefined || !Number.isFinite(v)) { if (open) { closeArea(last); open = false; } return; }
      const gap = !cat && step && last >= 0 && opts.x[i] - opts.x[last] > 1.5 * step;
      if (gap && open) { closeArea(last); open = false; }
      const p = `${X(i).toFixed(1)},${Y(v).toFixed(1)}`;
      if (!open) { d += `M${p}`; if (s.area) area += `M${p}`; segStart = i; open = true; }
      else { d += `L${p}`; if (s.area) area += `L${p}`; }
      last = i;
    });
    if (open) closeArea(last);
    if (s.area && area) svg.appendChild(svgEl('path', { d: area, fill: s.color, 'fill-opacity': 0.1, stroke: 'none' }));
    svg.appendChild(svgEl('path', { d, fill: 'none', stroke: s.color, 'stroke-width': 2, 'stroke-linejoin': 'round', 'stroke-linecap': 'round' }));
    for (const i of lone) if (i !== last) svg.appendChild(svgEl('circle', { cx: X(i), cy: Y(s.values[i]), r: 2.5, fill: s.color }));
    // point final + valeur (une seule série : l'étiquette directe suffit)
    if (last >= 0) {
      svg.appendChild(svgEl('circle', { cx: X(last), cy: Y(s.values[last]), r: 4, fill: s.color, stroke: cssVar('--s1') || '#0D1014', 'stroke-width': 2 }));
      if (opts.endLabel && series.length === 1) {
        const tx = svgEl('text', { x: X(last) + 8, y: Y(s.values[last]) + 4, class: 'vlabel' });
        tx.textContent = (opts.yFmt || fmtNum)(s.values[last]);
        svg.appendChild(tx);
      }
    }
  }
  // réticule
  const cross = svgEl('line', { y1: m.t, y2: m.t + ih, class: 'vcross', visibility: 'hidden' });
  svg.appendChild(cross);
  const dots = series.map((s) => { const c = svgEl('circle', { r: 4, fill: s.color, stroke: cssVar('--s1') || '#0D1014', 'stroke-width': 2, visibility: 'hidden' }); svg.appendChild(c); return c; });
  const hit = svgEl('rect', { x: m.l, y: m.t, width: iw, height: ih, fill: 'transparent', tabindex: 0, class: 'vhit' });
  svg.appendChild(hit);
  let cur = -1;
  const move = (clientX) => {
    const r = svg.getBoundingClientRect();
    const px = clientX - r.left;
    let best = 0, bd = Infinity;
    for (let i = 0; i < n; i++) { const dd = Math.abs(X(i) - px); if (dd < bd) { bd = dd; best = i; } }
    show(best, r);
  };
  const show = (i, r = svg.getBoundingClientRect()) => {
    cur = i;
    cross.setAttribute('x1', X(i)); cross.setAttribute('x2', X(i)); cross.setAttribute('visibility', 'visible');
    const rows = [];
    series.forEach((s, k) => {
      const v = s.values[i];
      if (v === null || v === undefined) { dots[k].setAttribute('visibility', 'hidden'); return; }
      dots[k].setAttribute('cx', X(i)); dots[k].setAttribute('cy', Y(v)); dots[k].setAttribute('visibility', 'visible');
      rows.push({ color: s.color, value: (opts.tipFmt || opts.yFmt || fmtNum)(v), label: s.name });
    });
    if (opts.extraTip) rows.push(...opts.extraTip(i));
    const title = cat ? (opts.tipTitleCat ? opts.tipTitleCat(i) : opts.x[i]) : (opts.tipTitle ? opts.tipTitle(opts.x[i]) : fFull.format(new Date(opts.x[i] * 1000)));
    const topV = Math.min(...series.map((s) => (s.values[i] === null || s.values[i] === undefined ? Infinity : Y(s.values[i]))));
    showTip(r.left + window.scrollX + X(i), r.top + window.scrollY + (Number.isFinite(topV) ? topV : m.t), title, rows.length ? rows : [{ value: '—', label: 'pas de relevé' }]);
  };
  const leave = () => { cross.setAttribute('visibility', 'hidden'); dots.forEach((d) => d.setAttribute('visibility', 'hidden')); hideTip(); cur = -1; };
  hit.addEventListener('pointermove', (e) => move(e.clientX));
  hit.addEventListener('pointerdown', (e) => move(e.clientX));
  hit.addEventListener('pointerleave', leave);
  hit.addEventListener('blur', leave);
  hit.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') { e.preventDefault(); show(Math.max(0, Math.min(n - 1, (cur < 0 ? n - 1 : cur) + (e.key === 'ArrowRight' ? 1 : -1)))); }
    if (e.key === 'Escape') leave();
  });
  host.appendChild(svg);
}

// ── Colonnes (empilées si plusieurs séries) ──
// opts : x (libellés), series [{name, color, values}], yFmt, tipTitle(i)
function columnChart(host, opts) {
  host.replaceChildren();
  const series = opts.series;
  const n = opts.x.length;
  if (!n || !series.some((s) => s.values.some((v) => v))) { host.appendChild(el('p', 'vempty', opts.empty || 'Pas encore de données.')); return; }
  if (series.length >= 2) legend(host, series, 'rect');
  const W = Math.max(280, host.clientWidth || 600);
  const H = opts.height || (W < 520 ? 180 : 210);
  const m = { l: 44, r: 10, t: 10, b: 26 };
  const iw = W - m.l - m.r, ih = H - m.t - m.b;
  const tot = opts.x.map((_, i) => series.reduce((a, s) => a + (s.values[i] || 0), 0));
  const yt = niceTicks(0, Math.max(...tot, 1e-9) || 1, W < 520 ? 3 : 4);
  const hi = yt.at(-1);
  const yDec = tickDecimals(yt);
  const Y = (v) => m.t + ih - (v / hi) * ih;
  const band = iw / n;
  const bw = Math.max(2, Math.min(24, band - 2));
  const svg = svgEl('svg', { width: W, height: H, viewBox: `0 0 ${W} ${H}`, class: 'vchart', role: 'img', 'aria-label': opts.label || '' });
  for (const v of yt) {
    svg.appendChild(svgEl('line', { x1: m.l, x2: W - m.r, y1: Y(v), y2: Y(v), class: 'vgridline' }));
    const tx = svgEl('text', { x: m.l - 8, y: Y(v) + 4, 'text-anchor': 'end', class: 'vaxis' });
    tx.textContent = (opts.yFmt || fmtNum)(v, yDec);
    svg.appendChild(tx);
  }
  const surface = cssVar('--s1') || '#0D1014';
  opts.x.forEach((lab, i) => {
    const cx = m.l + band * i + band / 2;
    let base = 0;
    const g = svgEl('g', { class: 'vbar', tabindex: 0 });
    const segs = series.map((s) => s.values[i] || 0);
    const topIdx = segs.reduce((acc, v, k) => (v > 0 ? k : acc), -1);
    segs.forEach((v, k) => {
      if (!(v > 0)) return;
      const y0 = Y(base), y1 = Y(base + v);
      const h = Math.max(1, y0 - y1 - (k < topIdx ? 2 : 0));       // 2 px de fond entre segments
      const y = y1 + (k < topIdx ? 2 : 0);
      const r = k === topIdx ? Math.min(4, bw / 2, h) : 0;
      const d = r
        ? `M${cx - bw / 2},${y + h}V${y + r}Q${cx - bw / 2},${y} ${cx - bw / 2 + r},${y}H${cx + bw / 2 - r}Q${cx + bw / 2},${y} ${cx + bw / 2},${y + r}V${y + h}Z`
        : `M${cx - bw / 2},${y + h}V${y}H${cx + bw / 2}V${y + h}Z`;
      g.appendChild(svgEl('path', { d, fill: series[k].color }));
      base += v;
    });
    g.appendChild(svgEl('rect', { x: m.l + band * i, y: m.t, width: band, height: ih, fill: 'transparent' }));
    const showIt = () => {
      const r = svg.getBoundingClientRect();
      g.classList.add('is-hot');
      showTip(r.left + window.scrollX + cx, r.top + window.scrollY + Y(tot[i]), opts.tipTitle ? opts.tipTitle(i) : lab,
        series.map((s) => ({ color: s.color, value: (opts.tipFmt || opts.yFmt || fmtNum)(s.values[i] ?? null), label: s.name })));
    };
    const off = () => { g.classList.remove('is-hot'); hideTip(); };
    g.addEventListener('pointerenter', showIt); g.addEventListener('pointerdown', showIt);
    g.addEventListener('pointerleave', off); g.addEventListener('focus', showIt); g.addEventListener('blur', off);
    svg.appendChild(g);
    const kk = Math.max(1, Math.ceil(labelWidth(opts.x) / band));
    if (i % kk === 0 || (i === n - 1 && ((n - 1) % kk) * band >= labelWidth(opts.x))) {
      const tx = svgEl('text', { x: cx, y: H - 8, 'text-anchor': 'middle', class: 'vaxis' });
      tx.textContent = lab;
      svg.appendChild(tx);
    }
  });
  host.appendChild(svg);
  void surface;
}

// Tableau équivalent (accessibilité, copier-coller)
function table(host, head, rows) {
  host.replaceChildren();
  const t = el('table', 'vtable');
  const tr = el('tr');
  for (const h of head) tr.appendChild(el('th', null, h));
  const th = el('thead'); th.appendChild(tr); t.appendChild(th);
  const tb = el('tbody');
  for (const r of rows) { const x = el('tr'); for (const c of r) x.appendChild(el('td', null, c)); tb.appendChild(x); }
  t.appendChild(tb);
  host.appendChild(t);
}
function tile(host, label, value, sub, tone) {
  const d = el('div', `kpi${tone ? ' kpi--' + tone : ''}`);
  d.append(el('div', 'kpi__label', label), el('div', 'kpi__value', value));
  if (sub) d.appendChild(el('div', 'kpi__sub', sub));
  host.appendChild(d);
}

// ── Couleurs (validées pour la surface sombre : scripts/validate_palette.js) ──
const C = {
  line: () => cssVar('--viz-1'), model: () => cssVar('--viz-1'), per: () => cssVar('--viz-2'), prof: () => cssVar('--viz-3'),
  empty: () => cssVar('--viz-empty'), full: () => cssVar('--viz-full'),
};

// ════════════════════════════════════════════════════════════════════
// Historique
// ════════════════════════════════════════════════════════════════════
const RANGES = { '1': { days: 1, step: 15 }, '7': { days: 7, step: 60 }, '28': { days: 28, step: 180 }, '90': { days: 90, step: 1440 } };
const H = { range: store.get('vn.histRange') || '7', sid: '', stations: [], info: null, last: null };

async function loadHistory() {
  const box = $('tab-hist');
  if (!box) return;
  box.classList.add('is-loading');
  try {
    if (!H.info) H.info = await api('api/history');
    if (!H.stations.length) {
      H.stations = await api('api/stations').catch(() => []);
      const dl = $('stList');
      dl.replaceChildren(...H.stations.filter((s) => s.active).sort((a, b) => a.name.localeCompare(b.name, 'fr')).map((s) => { const o = el('option'); o.value = `${s.name} · #${s.sid}`; return o; }));
    }
    const r = RANGES[H.range];
    const q = `days=${r.days}`;
    const sid = H.sid ? `&sid=${encodeURIComponent(H.sid)}` : '';
    const [ser, daily] = await Promise.all([api(`api/history/series?${q}&step=${r.step}${sid}`), api(`api/history/daily?days=${Math.max(r.days, 7)}`)]);
    H.last = { ser, daily };
    renderHistory();
  } catch (err) {
    $('histMain').replaceChildren(el('p', 'vempty', `Historique indisponible : ${err.message}`));
  } finally { box.classList.remove('is-loading'); }
}
function renderHistory() {
  if (!H.last) return;
  const { ser, daily } = H.last;
  const info = H.info || {};
  const st = ser.station;
  // résumé
  $('histMeta').textContent = info.days
    ? `${info.days} jour${info.days > 1 ? 's' : ''} gardé${info.days > 1 ? 's' : ''} (du ${dayLabel(info.from)} au ${dayLabel(info.to)}) · ${fmtNum((info.bytes || 0) / 1024)} Ko sur disque`
    : 'Le serveur commence à écrire l\'historique : une tranche toutes les 5 min.';
  const k = $('histKpis');
  k.replaceChildren();
  const lastIdx = ser.t.length - 1;
  if (st) {
    const v = ser.bikes.filter((x) => x !== null);
    tile(k, 'Vélos maintenant', lastIdx >= 0 ? fmtNum(ser.bikes[lastIdx], 0) : '—', `sur ${fmtNum(st.cap)} bornes`);
    tile(k, 'Moyenne de la période', v.length ? fmtNum(v.reduce((a, b) => a + b, 0) / v.length, 1) : '—', `creux ${fmtNum(Math.min(...v), 0)} · pic ${fmtNum(Math.max(...v), 0)}`);
    const emptyShare = v.length ? v.filter((x) => x < 0.5).length / v.length : null;
    tile(k, 'Temps à vide', emptyShare === null ? '—' : `${fmtNum(emptyShare * 100, 1)} %`, 'part des pas sans vélo', emptyShare > 0.1 ? 'bad' : null);
  } else {
    const av = ser.avail.filter((x) => x !== null);
    tile(k, 'Vélos disponibles', lastIdx >= 0 ? fmtNum(ser.avail[lastIdx]) : '—', av.length ? `moyenne ${fmtNum(av.reduce((a, b) => a + b, 0) / av.length)} sur la période` : '');
    const full = daily.days.filter((d) => d.slots >= 200);
    const depMean = full.length ? full.reduce((a, d) => a + d.depFull, 0) / full.length : null;
    tile(k, 'Retraits observés par jour', fmtNum(depMean), full.length ? `minimum · moyenne de ${full.length} jour${full.length > 1 ? 's' : ''} complet${full.length > 1 ? 's' : ''}` : 'aucun jour complet encore');
    const dd = daily.days.at(-1);
    tile(k, 'En rupture aujourd\'hui', dd ? `${fmtNum(dd.emptyH + dd.fullH, 0)} h` : '—', dd ? `stations·heures : ${fmtNum(dd.emptyH, 0)} à vide, ${fmtNum(dd.fullH, 0)} pleines` : '');
  }
  const cov = ser.cover.length ? ser.cover.reduce((a, b) => a + b, 0) / ser.cover.length : null;
  tile(k, 'Couverture', cov === null ? '—' : `${fmtNum(Math.min(1, cov) * 100, 0)} %`, 'tranches de 5 min relevées', cov !== null && cov < 0.9 ? 'warn' : null);
  // courbe principale
  $('histTitle').textContent = st ? `Vélos à ${st.name} (#${st.sid}) · ${st.cap} bornes` : 'Vélos disponibles sur le réseau';
  const stepS = ser.step * 60;
  const capSeries = st ? [{ name: 'Capacité', color: cssVar('--text3'), values: ser.cap }] : [];
  lineChart($('histChart'), {
    x: ser.t, step: stepS, endLabel: true, yMin: 0, yMax: st ? st.cap : undefined,
    series: [{ name: st ? 'Vélos' : 'Vélos disponibles', color: C.line(), values: st ? ser.bikes : ser.avail, area: true }],
    yFmt: (v, d) => fmtNum(v, d), tipFmt: (v) => fmtNum(v, st ? 1 : 0),
    tipTitle: (t) => (ser.step >= 1440 ? fDay.format(new Date(t * 1000)) : ser.step > 5 ? `${fFull.format(new Date(t * 1000))} (${ser.step >= 60 ? ser.step / 60 + ' h' : ser.step + ' min'})` : fFull.format(new Date(t * 1000))),
    extraTip: (i) => (st ? [{ value: fmtNum(ser.cap[i]), label: 'bornes' }] : [
      { value: fmtNum(ser.dep[i]), label: `retraits observés (${ser.step >= 60 ? 'par ' + (ser.step / 60) + ' h' : 'par ' + ser.step + ' min'})` },
      ...(ser.temp[i] !== null ? [{ value: `${fmtNum(ser.temp[i], 1)} °C`, label: ser.rain[i] > 0.05 ? `pluie ${fmtNum(ser.rain[i], 1)} mm` : 'sec' }] : []),
    ]),
    empty: 'Aucune tranche sur cette période.', label: 'Courbe des vélos disponibles',
  });
  void capSeries;
  // ruptures par jour (stations·heures, 7 h – 22 h)
  const dl = daily.days;
  columnChart($('histRupt'), {
    x: dl.map((d) => fDayShort.format(new Date(`${d.day}T12:00:00Z`))),
    series: [{ name: 'Vides', color: C.empty(), values: dl.map((d) => d.emptyH) }, { name: 'Pleines', color: C.full(), values: dl.map((d) => d.fullH) }],
    yFmt: (v, d) => `${fmtNum(v, d)} h`, tipFmt: (v) => `${fmtNum(v, 1)} h`,
    tipTitle: (i) => `${dayLabel(dl[i].day)}${dl[i].svcSlots < 180 ? ` · ${Math.round(dl[i].svcSlots / 1.8)} % relevé` : ''}`,
    empty: 'Pas encore de journée relevée entre 7 h et 22 h.', label: 'Stations-heures en rupture par jour',
  });
  columnChart($('histDep'), {
    x: dl.map((d) => fDayShort.format(new Date(`${d.day}T12:00:00Z`))),
    series: [{ name: 'Retraits observés', color: C.line(), values: dl.map((d) => d.dep) }],
    tipTitle: (i) => `${dayLabel(dl[i].day)} · ${Math.round(dl[i].slots / 2.88)} % de la journée relevée`,
    empty: 'Pas encore de retraits comptés.', label: 'Retraits observés par jour',
  });
  table($('histTable'), ['Jour', 'Relevé', 'Retraits observés', 'Vélos (moy. / creux / pic)', 'Vides 7–22 h', 'Pleines 7–22 h', 'Pluie'],
    dl.slice().reverse().map((d) => [dayLabel(d.day), `${Math.round(d.slots / 2.88)} %`, fmtNum(d.dep), `${fmtNum(d.availMean)} / ${fmtNum(d.availMin)} / ${fmtNum(d.availMax)}`,
      `${fmtNum(d.emptyH, 1)} h`, `${fmtNum(d.fullH, 1)} h`, `${fmtNum(d.rainH, 1)} h`]));
  // service (résumés de la carte, 60 jours) : disponibilité par jour
  const sv = (daily.service || []).filter((d) => d.recorded);
  $('histSvc').textContent = sv.length
    ? `Disponibilité (vue Service de la carte) : ${sv.slice(-7).map((d) => `${d.label} ${d.availText}`).join(' · ')}`
    : '';
  const csv = $('histCsv');
  const r = RANGES[H.range];
  if (csv && !DEMO) csv.href = `api/history/export.csv?days=${r.days}${H.sid ? '&stations=1' : ''}`;
}

// ════════════════════════════════════════════════════════════════════
// Prévisions
// ════════════════════════════════════════════════════════════════════
async function loadForecast() {
  const box = $('tab-prev');
  if (!box) return;
  box.classList.add('is-loading');
  try {
    const [f, b] = await Promise.all([api('api/forecast?days=14'), api('api/bench').catch(() => null)]);
    renderForecast(f, b);
  } catch (err) {
    $('prevKpis').replaceChildren(el('p', 'vempty', `Mesure indisponible : ${err.message}`));
  } finally { box.classList.remove('is-loading'); }
}
function renderForecast(f, b) {
  const k = $('prevKpis');
  k.replaceChildren();
  const m60 = f.total.model[2], p60 = f.total.per[2], q60 = f.total.prof[2];
  const gain = m60.mae && p60.mae ? 1 - m60.mae / p60.mae : null;
  tile(k, 'Erreur à +1 h', m60.n ? `${fmtNum(m60.mae, 2)} vélo` : '—', m60.n ? `moyenne sur ${fmtNum(m60.n)} prévisions` : 'il faut 1 h de recul');
  tile(k, 'Sans prévoir (persistance)', p60.n ? `${fmtNum(p60.mae, 2)} vélo` : '—', 'erreur si l\'on affiche le relevé');
  tile(k, 'Gain de la prévision', gain === null ? '—' : `${gain >= 0 ? '+' : '−'}${fmtNum(Math.abs(gain) * 100, 1)} %`, gain === null ? '' : gain >= 0 ? 'mieux que la persistance' : 'moins bien que la persistance', gain === null ? null : gain >= 0 ? 'good' : 'warn');
  const al = f.alerts || {};
  tile(k, 'Alertes « vide dans l\'heure »', al.emptyPrecision === null || al.emptyPrecision === undefined ? '—' : `${fmtNum(al.emptyPrecision, 0)} % confirmées`,
    al.emptyPred ? `${al.emptyHit}/${al.emptyPred} · ${al.emptyRecall === null ? '—' : fmtNum(al.emptyRecall, 0) + ' %'} des ruptures annoncées` : 'aucune alerte encore notée');
  const days = f.days.filter((d) => d.preds);
  $('prevMeta').textContent = days.length
    ? `${days.length} jour${days.length > 1 ? 's' : ''} mesuré${days.length > 1 ? 's' : ''} · ${fmtNum(days.reduce((a, d) => a + d.preds, 0))} quarts d'heure notés · dernier ${f.lastSlot ? fHour.format(new Date(f.lastSlot * 1000)) : '—'}`
    : 'Aucune prévision notée pour l\'instant (de 6 h à 22 h, tous les quarts d\'heure).';
  // erreur selon l'horizon
  const hl = f.horizons.map((h) => h.replace(' min', '').replace('+', '+ '));
  lineChart($('prevHorizon'), {
    x: ['15 min', '30 min', '1 h', '2 h', '4 h'], cat: true, yMin: 0, tipTitleCat: (i) => `Horizon ${f.horizons[i]}`,
    series: [
      { name: 'Prévision de la carte', color: C.model(), values: f.total.model.map((a) => a.mae) },
      { name: 'Persistance (le relevé)', color: C.per(), values: f.total.per.map((a) => a.mae) },
      { name: 'Profil seul', color: C.prof(), values: f.total.prof.map((a) => a.mae) },
    ],
    yFmt: (v, d) => fmtNum(v, Math.max(1, d)), tipFmt: (v) => `${fmtNum(v, 2)} vélo`,
    extraTip: (i) => [{ value: fmtNum(f.total.model[i].n), label: 'prévisions notées' }],
    empty: 'Les premières erreurs arrivent 15 min après la première prévision.', label: 'Erreur moyenne selon l\'horizon',
  });
  void hl;
  // jour par jour, +60 min
  const dd = f.days.filter((d) => d.n60);
  lineChart($('prevDays'), {
    x: dd.map((d) => fDayShort.format(new Date(`${d.day}T12:00:00Z`))), cat: true, yMin: 0,
    series: [
      { name: 'Prévision de la carte', color: C.model(), values: dd.map((d) => d.model[2]) },
      { name: 'Persistance (le relevé)', color: C.per(), values: dd.map((d) => d.per[2]) },
    ],
    yFmt: (v, d) => fmtNum(v, Math.max(1, d)), tipFmt: (v) => `${fmtNum(v, 2)} vélo`,
    extraTip: (i) => [{ value: fmtNum(dd[i].n60), label: dd[i].complete ? 'prévisions (journée complète)' : 'prévisions (journée en cours)' }],
    empty: 'Un point par jour dès qu\'une heure de prévisions est notée.', label: 'Erreur à une heure, jour par jour',
  });
  // selon l'heure
  columnChart($('prevHours'), {
    x: f.hours.map((h) => `${h.h} h`),
    series: [{ name: 'Erreur à +1 h', color: C.model(), values: f.hours.map((h) => h.model) }],
    yFmt: (v, d) => fmtNum(v, Math.max(1, d)), tipFmt: (v) => `${fmtNum(v, 2)} vélo`,
    tipTitle: (i) => `Prévisions faites à ${f.hours[i].h} h (${fmtNum(f.hours[i].n)}) · persistance ${fmtNum(f.hours[i].per, 2)}`,
    empty: 'Pas encore d\'heure complète.', label: 'Erreur à une heure selon l\'heure de la prévision',
  });
  table($('prevHard'), ['Station', 'Erreur +1 h', 'Persistance', 'n'], f.hardest.map((s) => [s.name, fmtNum(s.model, 2), fmtNum(s.per, 2), fmtNum(s.n)]));
  table($('prevTable'), ['Horizon', 'Prévision', 'Persistance', 'Profil seul', 'n'], f.horizons.map((h, i) => [h, fmtNum(f.total.model[i].mae, 3), fmtNum(f.total.per[i].mae, 3), fmtNum(f.total.prof[i].mae, 3), fmtNum(f.total.model[i].n)]));
  // banc de nuit
  renderBench(b);
}
function renderBench(b) {
  const box = $('benchBody');
  box.replaceChildren();
  const btn = $('benchRun');
  if (!b) { box.appendChild(el('p', 'txt txt--dim', 'Banc indisponible.')); btn.hidden = true; return; }
  btn.hidden = !b.available || DEMO;
  btn.disabled = !!b.running;
  if (!b.available) {
    box.appendChild(el('p', 'txt txt--dim', 'Le banc natif (sim/bench_prevision) n\'est pas installé sur ce serveur. L\'image Docker le compile et le lance chaque nuit ; ailleurs : make -C sim bench_prevision puis VN_BENCH_BIN=…/sim/bench_prevision.'));
    return;
  }
  const L = b.latest;
  if (!L) { box.appendChild(el('p', 'txt txt--dim', `Premier passage cette nuit à ${b.nightAt}.${b.inputs ? ' ' + b.inputs + '.' : ''}`)); return; }
  if (!L.ok) { box.appendChild(el('p', 'txt txt--dim', `${dayLabel(L.day)} : ${L.why || 'le banc a échoué (code ' + L.code + ')'}. Prochain passage à ${b.nightAt}.`)); return; }
  const head = el('p', 'txt');
  head.textContent = `${dayLabel(L.day)} · ${L.snaps} instantanés rejoués${L.span ? ` du ${L.span.from} au ${L.span.to} (${fmtNum(L.span.days, 1)} j, ${L.span.coverPct} % des tranches)` : ''} · ${L.evalDays} jour${L.evalDays > 1 ? 's' : ''} évaluable${L.evalDays > 1 ? 's' : ''} · ${fmtNum(L.durationS, 1)} s`;
  box.appendChild(head);
  if (L.leak) box.appendChild(el('p', 'txt txt--warn', 'Fuite : le profil de départ connaît déjà une partie des tranches rejouées — erreurs optimistes.'));
  const pd = (L.perDay || []).filter((d) => d.model !== null);
  const ch = el('div', 'vbox');
  box.appendChild(ch);
  lineChart(ch, {
    x: pd.map((d) => d.day.replace(/^\S+ /, '') + (d.holiday ? '*' : '')), cat: true, yMin: 0,
    tipTitleCat: (i) => `${pd[i].day}${pd[i].holiday ? ' (férié)' : ''}`,
    series: [{ name: 'Prévision rejouée', color: C.model(), values: pd.map((d) => d.model) }, { name: 'Persistance', color: C.per(), values: pd.map((d) => d.per) }],
    yFmt: (v, d) => fmtNum(v, Math.max(1, d)), tipFmt: (v) => `${fmtNum(v, 2)} vélo`, empty: 'Pas encore de jour évaluable (12 h relevées au moins).', label: 'Banc : erreur à une heure par jour',
  });
  const tb = el('div', 'vscroll');
  box.appendChild(tb);
  table(tb, ['Échantillon', '+15', '+30', '+60', '+120', '+240', 'n (+60)'],
    L.rows.map((r) => [r.name, ...r.h.map((c) => `${fmtNum(c[0], 2)} / ${fmtNum(c[1], 2)} / ${fmtNum(c[2], 2)}`), fmtNum(r.n60)]));
  box.appendChild(el('p', 'txt txt--dim', 'Chaque case : prévision / persistance / profil seul (vélos, erreur absolue moyenne du chiffre affiché).'));
  for (const v of L.verdicts || []) {
    const p = el('p', 'txt verdict');
    p.append(el('b', null, v.q + ' '), document.createTextNode(v.text.replace(/\*\*/g, '')));
    box.appendChild(p);
  }
  if (!DEMO) {
    const a = el('a', 'file', 'Rapport complet (Markdown)');
    a.href = `api/bench/report.md?day=${L.day}`; a.target = '_blank'; a.rel = 'noopener';
    box.appendChild(a);
  }
}

// ════════════════════════════════════════════════════════════════════
// Notifications
// ════════════════════════════════════════════════════════════════════
const KIND = { rupture: 'Rupture', retablie: 'Rétablie', prevue: 'Prévue', flux: 'Flux', sunton: 'Sunton', resume: 'Résumé', test: 'Test' };
async function loadNotify() {
  try { renderNotify(await api('api/notify')); }
  catch (err) { $('notifStatus').textContent = `Indisponible : ${err.message}`; }
}
function renderNotify(s) {
  const c = s.config;
  const f = $('notifForm');
  f.url.value = c.url || ''; f.webhook.value = c.webhook || ''; f.link.value = c.link || (DEMO ? '' : location.origin);
  f.token.value = c.tokenSet ? '••••' : '';
  for (const k of ['ruptures', 'resolved', 'predicted', 'feed', 'devices']) f[k].checked = !!c[k];
  f.minEpisode.value = c.minEpisode; f.maxPerHour.value = c.maxPerHour; f.hours.value = c.hours || ''; f.digest.value = c.digest || '';
  f.stations.value = (c.stations || []).join(', ');
  const st = $('notifStatus');
  st.replaceChildren();
  const badge = el('span', `badge ${s.enabled ? (s.lastError ? 'warn' : 'good') : ''}`, s.enabled ? (s.lastError ? 'ERREUR' : 'ACTIVES') : 'INACTIVES');
  st.append(badge, document.createTextNode(s.enabled
    ? ` ${s.sentLastHour} envoyée(s) cette heure · ${s.openRuptures} rupture(s) signalée(s) en cours${s.lastError ? ' · dernier échec : ' + s.lastError : ''}`
    : ' Renseigne une adresse ntfy (ou un webhook) pour commencer.'));
  const ul = $('notifRecent');
  ul.replaceChildren();
  if (!s.recent.length) ul.appendChild(el('li', 'empty', 'Aucun message envoyé pour l\'instant.'));
  for (const r of s.recent) {
    const li = el('li');
    const top = el('div', 'msg__top');
    top.append(el('span', `badge${r.ok ? '' : ' bad'}`, (KIND[r.kind] || r.kind).toUpperCase()), el('b', null, r.title), el('span', 'when', ago(r.t)));
    li.append(top, el('p', 'msg__body', r.message));
    if (!r.ok && r.why) li.appendChild(el('p', 'msg__err', `Échec : ${r.why}`));
    ul.appendChild(li);
  }
}
function wireNotify() {
  const f = $('notifForm');
  if (!f) return;
  const say = (t, bad) => { const o = $('notifSay'); o.textContent = t; o.classList.toggle('is-bad', !!bad); };
  f.addEventListener('submit', async (e) => {
    e.preventDefault();
    const body = {
      url: f.url.value, webhook: f.webhook.value, link: f.link.value, token: f.token.value,
      ruptures: f.ruptures.checked, resolved: f.resolved.checked, predicted: f.predicted.checked, feed: f.feed.checked, devices: f.devices.checked,
      minEpisode: +f.minEpisode.value, maxPerHour: +f.maxPerHour.value, hours: f.hours.value, digest: f.digest.value, stations: f.stations.value,
    };
    try { renderNotify(await api('api/notify', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) })); say('Réglages enregistrés.'); }
    catch (err) { say(err.message, true); }
  });
  $('notifTest').addEventListener('click', async () => {
    say('Envoi…');
    try { const r = await api('api/notify/test', { method: 'POST' }); say(r.ok ? 'Message de test envoyé : il doit apparaître sur le téléphone.' : `Échec : ${r.why}`, !r.ok); loadNotify(); }
    catch (err) { say(err.message, true); }
  });
  $('notifDigest').addEventListener('click', async () => {
    try {
      const r = await api('api/notify/digest', { method: 'POST' });
      const pv = $('notifPreview');
      pv.hidden = false;
      pv.replaceChildren(el('b', null, r.message.title), el('p', 'msg__body', r.message.message));
    } catch (err) { say(err.message, true); }
  });
}

// ════════════════════════════════════════════════════════════════════
// Onglets, filtres, rafraîchissement
// ════════════════════════════════════════════════════════════════════
const loaders = { hist: loadHistory, prev: loadForecast, notif: loadNotify };
let current = store.get('vn.tab') || 'hist';
function select(tab) {
  if (!loaders[tab]) tab = 'hist';
  current = tab;
  store.set('vn.tab', tab);
  for (const b of document.querySelectorAll('.tabs [role=tab]')) {
    const on = b.dataset.tab === tab;
    b.setAttribute('aria-selected', on ? 'true' : 'false');
    b.tabIndex = on ? 0 : -1;
  }
  for (const p of document.querySelectorAll('.tabpanel')) p.hidden = p.id !== `tab-${tab}`;
  loaders[tab]();
}
export function initBoard() {
  if (!$('board')) return;
  for (const b of document.querySelectorAll('.tabs [role=tab]')) {
    b.addEventListener('click', () => select(b.dataset.tab));
    b.addEventListener('keydown', (e) => {
      if (e.key !== 'ArrowRight' && e.key !== 'ArrowLeft') return;
      const tabs = [...document.querySelectorAll('.tabs [role=tab]')];
      const i = tabs.indexOf(b) + (e.key === 'ArrowRight' ? 1 : -1);
      const nb = tabs[(i + tabs.length) % tabs.length];
      nb.focus(); select(nb.dataset.tab);
    });
  }
  for (const b of document.querySelectorAll('#histRange button')) {
    b.setAttribute('aria-pressed', b.dataset.r === H.range ? 'true' : 'false');
    b.addEventListener('click', () => {
      H.range = b.dataset.r; store.set('vn.histRange', H.range);
      for (const x of document.querySelectorAll('#histRange button')) x.setAttribute('aria-pressed', x === b ? 'true' : 'false');
      loadHistory();
    });
  }
  const inp = $('histStation');
  inp.addEventListener('change', () => {
    const v = inp.value.trim();
    const m = v.match(/#(\S+)$/);
    const s = m ? H.stations.find((x) => x.sid === m[1]) : H.stations.find((x) => x.name.toLowerCase() === v.toLowerCase());
    H.sid = s ? s.sid : '';
    if (!s) inp.value = '';
    loadHistory();
  });
  $('histClear').addEventListener('click', () => { inp.value = ''; H.sid = ''; loadHistory(); });
  $('benchRun')?.addEventListener('click', async () => {
    try { await api('api/bench/run', { method: 'POST' }); $('benchRun').disabled = true; setTimeout(loadForecast, 8000); } catch (err) { $('benchBody').prepend(el('p', 'txt txt--warn', err.message)); }
  });
  wireNotify();
  // l'aperçu autonome change de jeu de données (serveur réel / exemple simulé) : tout relire
  window.addEventListener('vn-board-reload', () => {
    H.info = null; H.stations = []; H.sid = ''; H.last = null;
    const i2 = $('histStation'); if (i2) i2.value = '';
    loaders[current]();
  });
  if (DEMO) { const c = $('histCsv'); if (c) c.hidden = true; }
  select(current);
  // nouvelles tranches toutes les 5 min ; redessin à la largeur
  setInterval(() => { if (!document.hidden && current !== 'notif') { H.info = null; loaders[current](); } }, 5 * 60 * 1000);
  let rz = 0, lastW = window.innerWidth;
  window.addEventListener('resize', () => {
    if (Math.abs(window.innerWidth - lastW) < 8) return;
    lastW = window.innerWidth;
    clearTimeout(rz);
    rz = setTimeout(() => { if (current === 'hist') renderHistory(); else if (current === 'prev') loadForecast(); }, 150);
  });
}
if (typeof document !== 'undefined') {
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', initBoard); else initBoard();
}
