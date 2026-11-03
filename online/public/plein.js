// ── Plein écran natif ────────────────────────────────────────────────────────────────────────────
// Le jumeau est une image de 480 × 272 : l'agrandir ne peut que grossir des blocs. Cette vue-ci
// redessine les mêmes chiffres à la résolution de l'écran (HTML + SVG), donc nets à toute taille.
// Le jumeau pixel reste la vue normale, pour vérifier la dalle.
// Données : /api/summary, /api/stations, /api/history/series — lectures publiques.

const $ = (id) => document.getElementById(id);
const store = { get: (k) => { try { return localStorage.getItem(k); } catch { return null; } } };
const FR = new Intl.NumberFormat('fr-FR');
const nf = (v) => (typeof v === 'number' && isFinite(v) ? FR.format(Math.round(v)) : '—');
const plur = (n, un, plusieurs) => `${nf(n)} ${n > 1 ? plusieurs : un}`;
const SVGNS = 'http://www.w3.org/2000/svg';
const el = (tag, attrs = {}, txt) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  if (txt !== undefined) e.textContent = txt;
  return e;
};
const svg = (tag, attrs = {}) => {
  const e = document.createElementNS(SVGNS, tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  return e;
};
const hhmm = (s) => new Date(s * 1000).toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit' });

async function api(path) {
  const token = store.get('vn.token') || '';
  const r = await fetch(path, { headers: token ? { Authorization: `Bearer ${token}` } : {} });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}

let root = null, timer = null, tick = null, last = { at: 0, feed: 0, fw: '' };

function build() {
  root = el('div', { id: 'vnFull', class: 'vnfull', role: 'dialog', 'aria-modal': 'true', 'aria-label': 'Tableau de bord en plein écran', hidden: '' });
  root.innerHTML = `
    <header class="vnfull__head">
      <div class="vnfull__brand">
        <span class="vnfull__mark">VELOHNAV OPS</span>
        <span class="vnfull__sub" id="feSub">réseau Vel'OH! · Luxembourg</span>
      </div>
      <div class="vnfull__state">
        <span class="vnfull__live" id="feLive">LIVE</span>
        <span class="vnfull__age" id="feAge">—</span>
      </div>
      <button class="vnfull__close" type="button" aria-label="Quitter le plein écran" title="Quitter (Échap)">&times;</button>
    </header>
    <div class="vnfull__kpis" id="feKpis"></div>
    <section class="vnfull__chart" aria-label="Vélos disponibles sur 24 heures">
      <div class="vnfull__charthead">
        <span class="eyebrow">VÉLOS DISPONIBLES · 24 H</span>
        <span class="vnfull__legend" id="feLegend"></span>
      </div>
      <div class="vnfull__plot" id="fePlot"></div>
    </section>
    <footer class="vnfull__foot" id="feFoot"></footer>
    <div class="vnfull__quitbar"><button class="vnfull__quit" type="button" id="feQuit">Quitter le plein écran</button></div>`;
  document.body.appendChild(root);
  root.querySelector('.vnfull__close').addEventListener('click', () => VN_FULL.leave());
  // Sortie visible en bas au centre : le coin supérieur droit est souvent masqué par les boutons de
  // l'application qui affiche la page (constat de Claude, valable aussi pour cette vue).
  root.querySelector('#feQuit').addEventListener('click', () => VN_FULL.leave());
  root.addEventListener('click', (e) => { if (e.target === root) VN_FULL.leave(); });
}

const kpi = (label, value, sub, tone) =>
  `<div class="vnfull__kpi${tone ? ' is-' + tone : ''}"><span class="vnfull__klabel">${label}</span>` +
  `<span class="vnfull__kval">${value}</span><span class="vnfull__ksub">${sub || ''}</span></div>`;

const swatch = (cls, n, mot) => `<span class="vnfull__item"><span class="vnfull__sw ${cls}"></span>${nf(n)} ${mot}</span>`;

// Courbe : cadre de 24 h toujours dessiné (axes + grille), courbe par-dessus quand il y a des points.
function plot(series, opt = {}) {
  const box = $('fePlot');
  box.textContent = '';
  const t = series.t || [], avail = series.avail || [];
  const n = Math.min(t.length, avail.length);
  const W = Math.max(320, box.clientWidth), H = Math.max(140, box.clientHeight);
  const m = { t: 10, r: 10, b: 26, l: 58 };
  const now = Math.round(Date.now() / 1000);
  const x0 = now - 24 * 3600, x1 = now;
  const vals = avail.slice(0, n).filter((v) => typeof v === 'number');
  const yMax = Math.max(opt.yMax || 0, ...vals, 10) * 1.08;
  const X = (ts) => m.l + ((ts - x0) / (x1 - x0)) * (W - m.l - m.r);
  const Y = (v) => H - m.b - (v / yMax) * (H - m.t - m.b);
  const s = svg('svg', { viewBox: `0 0 ${W} ${H}`, width: W, height: H, class: 'vnfull__svg', role: 'img', 'aria-label': 'Vélos disponibles sur 24 heures' });
  for (let k = 0; k <= 4; k++) {
    const v = (yMax * k) / 4, y = Y(v);
    s.appendChild(svg('line', { x1: m.l, x2: W - m.r, y1: y, y2: y, class: 'vnfull__grid' }));
    const tx = svg('text', { x: m.l - 10, y: y + 5, 'text-anchor': 'end', class: 'vnfull__axis' });
    tx.textContent = nf(v); s.appendChild(tx);
  }
  for (let h = 0; h <= 24; h += 4) {
    const ts = x0 + h * 3600, x = X(ts);
    s.appendChild(svg('line', { x1: x, x2: x, y1: m.t, y2: H - m.b, class: 'vnfull__grid vnfull__grid--v' }));
    const tx = svg('text', { x, y: H - 8, 'text-anchor': 'middle', class: 'vnfull__axis' });
    tx.textContent = hhmm(ts); s.appendChild(tx);
  }
  if (n > 1) {
    let d = '';
    for (let i = 0; i < n; i++) d += `${i ? 'L' : 'M'}${X(t[i]).toFixed(1)},${Y(avail[i]).toFixed(1)}`;
    s.appendChild(svg('path', { d: `${d}L${X(t[n - 1]).toFixed(1)},${H - m.b}L${X(t[0]).toFixed(1)},${H - m.b}Z`, class: 'vnfull__area' }));
    s.appendChild(svg('path', { d, class: 'vnfull__line' }));
    s.appendChild(svg('circle', { cx: X(t[n - 1]), cy: Y(avail[n - 1]), r: 5, class: 'vnfull__dot' }));
  } else if (n === 1) {
    s.appendChild(svg('circle', { cx: X(t[0]), cy: Y(avail[0]), r: 5, class: 'vnfull__dot' }));
  }
  box.appendChild(s);
  $('feLegend').textContent = n > 1 ? `${nf(avail[0])} → ${nf(avail[n - 1])} · ${n} points` : '';
  if (n < 2) {
    const msg = el('div', { class: 'vnfull__empty' });
    msg.innerHTML = `<strong>Aucun historique disponible</strong><span>collecte démarrée à ${opt.startedAt ? hhmm(opt.startedAt) : '—'} · la courbe se remplit à chaque relevé (60 s)</span>`;
    box.appendChild(msg);
  }
}

async function refresh() {
  try {
    const [sum, st, ser] = await Promise.all([
      api('/api/summary'),
      api('/api/stations'),
      api('/api/history/series?step=5&days=1').catch(() => ({ t: [], avail: [] })),
    ]);
    const T = sum.totals || {};
    last = { at: Date.now(), feed: sum.lastUpdate || 0, fw: sum.fw };
    $('feSub').textContent = `réseau Vel'OH! · Luxembourg · serveur v${sum.fw}`;
    const delta = typeof T.fc60 === 'number' && typeof T.bikes === 'number' ? T.fc60 - T.bikes : null;
    $('feKpis').innerHTML = [
      kpi('VÉLOS DISPO', nf(T.bikes), `${nf(T.elec)} électriques`),
      kpi('BORNES LIBRES', nf(T.docks), `occupation réseau ${T.fillPct ? T.fillPct.toFixed(0) : '—'} %`),
      kpi('VÉLOS HS', nf(T.bikesDis), `${nf(T.docksDis)} bornes HS`, T.bikesDis > 0 ? 'warn' : ''),
      kpi('EN TRAJET', T.inUseValid ? nf(T.inUse) : '—', T.inUseValid ? 'estimation serveur' : 'estimation en cours'),
    ].join('');
    const cnt = {};
    for (const x of st) cnt[x.statusText] = (cnt[x.statusText] || 0) + 1;
    const open = st.filter((x) => x.active).length || 1;
    const ok = cnt['ÉQUILIBRÉE'] || 0;
    // chaque état porte son singulier et son pluriel : « 1 fermée » et non « 1 fermée(s) »
    const segs = [['VIDE', 'is-bad', 'vide', 'vides'], ['FAIBLE', 'is-warn', 'faible', 'faibles'],
      ['ÉQUILIBRÉE', 'is-ok', 'équilibrée', 'équilibrées'], ['CHARGÉE', 'is-warn', 'chargée', 'chargées'],
      ['PLEINE', 'is-bad', 'pleine', 'pleines'], ['FERMÉE', 'is-off', 'fermée', 'fermées']]
      .filter(([k]) => cnt[k]);
    const bar = segs.map(([k, c]) => `<span class="vnfull__seg ${c}" style="flex:${cnt[k]}" title="${k} : ${cnt[k]}"></span>`).join('');
    const legende = segs.map(([k, c, un, pl]) => swatch(c, cnt[k], cnt[k] > 1 ? pl : un)).join('');
    $('feFoot').innerHTML = `
      <div class="vnfull__block vnfull__block--wide"><span class="eyebrow">ÉQUILIBRE DES STATIONS</span>
        <div class="vnfull__balance"><span class="vnfull__pct">${Math.round((ok / open) * 100)} %</span>
          <span class="vnfull__bar">${bar}</span></div>
        <span class="vnfull__items">${legende}</span></div>
      <div class="vnfull__block"><span class="eyebrow">PRÉVISION +1 H</span>
        <span class="vnfull__kv">${nf(T.fc60)}${delta === null ? '' : `<span class="vnfull__delta">${delta === 0 ? 'stable' : (delta > 0 ? '+' : '') + nf(delta)}</span>`}</span>
        <span class="vnfull__ksub">vélos attendus</span></div>
      <div class="vnfull__block"><span class="eyebrow">ALERTES ACTIVES</span>
        <span class="vnfull__kv${sum.alerts ? ' is-warn' : ''}">${nf(sum.alerts)}</span>
        <span class="vnfull__ksub">${plur(T.empty, 'station vide', 'stations vides')} · ${plur(T.full, 'station pleine', 'stations pleines')}</span></div>
      <div class="vnfull__block"><span class="eyebrow">RÉSEAU</span>
        <span class="vnfull__kv">${nf(sum.stations)}</span>
        <span class="vnfull__ksub">stations · ${plur(T.closed, 'fermée', 'fermées')}</span></div>`;
    plot(ser, { yMax: T.fleet || 0, startedAt: sum.learnSince });
    const fresh = sum.lastUpdate && Date.now() / 1000 - sum.lastUpdate < 300;
    $('feLive').textContent = fresh ? 'LIVE' : 'RETARD';
    $('feLive').classList.toggle('is-stale', !fresh);
    tickAge();
  } catch (e) {
    $('feLive').textContent = 'HORS LIGNE';
    $('feLive').classList.add('is-stale');
  }
}

function tickAge() {
  const age = last.feed ? Math.round(Date.now() / 1000 - last.feed) : null;
  $('feAge').textContent = age === null ? '—' : `dernier relevé ${hhmm(last.feed)} · il y a ${age} s`;
}

const VN_FULL = {
  get open() { return root && !root.hidden; },
  async enter() {
    if (!root) build();
    root.hidden = false;
    document.documentElement.classList.add('vn-full-open');
    try { if (document.fullscreenEnabled && root.requestFullscreen) await root.requestFullscreen({ navigationUI: 'hide' }); } catch { /* cadre sans permission : la vue couvre la fenêtre */ }
    const b = $('btnFull'); if (b) b.textContent = 'Quitter le plein écran';
    await refresh();
    clearInterval(timer); clearInterval(tick);
    timer = setInterval(refresh, 30000);
    tick = setInterval(tickAge, 1000);
    addEventListener('resize', onResize);
  },
  leave() {
    if (!root) return;
    root.hidden = true;
    document.documentElement.classList.remove('vn-full-open');
    clearInterval(timer); clearInterval(tick); timer = tick = null;
    removeEventListener('resize', onResize);
    if (document.fullscreenElement === root) document.exitFullscreen?.().catch(() => {});
    const b = $('btnFull'); if (b) b.textContent = 'Plein écran';
  },
  toggle() { return this.open ? this.leave() : this.enter(); },
};

let rsz = null;
function onResize() {
  clearTimeout(rsz);
  rsz = setTimeout(() => {
    if (VN_FULL.open) api('/api/history/series?step=5&days=1').then((s) => plot(s, { yMax: 0 })).catch(() => {});
  }, 250);
}

document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && VN_FULL.open) VN_FULL.leave(); });
window.VN_FULL = VN_FULL;
