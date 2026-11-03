// ── Séries de l'historique long : un seul code pour le serveur et la page ─
// Agrège des tranches de 5 min (lignes de <données>/historique, voir lib/history.mjs) par pas de N minutes,
// et résume chaque jour. Le serveur s'en sert pour /api/history/* ; l'aperçu autonome, pour calculer les
// mêmes courbes dans le navigateur à partir des mêmes tranches. Aucune dépendance (Node ≥ 18, navigateurs).

export const SLOT_S = 300;

// Heure locale de Luxembourg (la carte et le banc datent tout en heure de Paris/Luxembourg)
const LUX = new Intl.DateTimeFormat('fr-FR', {
  timeZone: 'Europe/Luxembourg', year: 'numeric', month: '2-digit', day: '2-digit',
  hour: '2-digit', minute: '2-digit', second: '2-digit', hourCycle: 'h23',
});
export function luxParts(epochS) {
  const p = Object.fromEntries(LUX.formatToParts(new Date(epochS * 1000)).map((x) => [x.type, x.value]));
  return { y: p.year, mo: p.month, d: p.day, h: +p.hour, mi: +p.minute, s: p.second, stamp: `${p.year}${p.month}${p.day}-${p.hour}${p.minute}${p.second}` };
}
export const dayOf = (t) => { const p = luxParts(t); return `${p.y}-${p.mo}-${p.d}`; };

// Début du pas contenant t : multiples du pas en heure LOCALE (un pas de 1 jour commence à minuit à
// Luxembourg, un pas de 3 h à 0 h, 3 h, 6 h…), pas en heure UTC
function bucketOf(t, stepS) {
  if (stepS <= 3600) return Math.floor(t / stepS) * stepS;
  const p = luxParts(t);
  const secs = p.h * 3600 + p.mi * 60 + +p.s;
  const into = secs % stepS;
  return t - into;
}

// recs : [{t, n, w, b, st}] triées ; step en minutes ; sid = une station (sinon le réseau)
export function aggregate(recs, { from = null, to = null, step = 60, sid = null } = {}) {
  const stepS = Math.max(5, step) * 60;
  const buckets = new Map();
  let meta = null;
  const colCache = new Map();
  for (const r of recs) {
    const k = bucketOf(r.t, stepS);
    let b = buckets.get(k);
    if (!b) { b = { n: 0, avail: [0, 0], docks: [0, 0], dis: [0, 0], dep: 0, ret: 0, flow: 0, empty: [0, 0], full: [0, 0], temp: [0, 0], rain: [0, 0], bikes: [0, 0], cap: 0 }; buckets.set(k, b); }
    b.n++;
    if (r.n) {
      b.avail[0] += r.n[0]; b.avail[1]++;
      b.docks[0] += r.n[1]; b.docks[1]++;
      b.dis[0] += r.n[2]; b.dis[1]++;
      b.dep += r.n[4]; b.ret += r.n[5]; b.flow++;
      b.empty[0] += r.n[6]; b.empty[1]++;
      b.full[0] += r.n[7]; b.full[1]++;
    }
    if (r.w) {
      if (r.w[0] !== null) { b.temp[0] += r.w[0]; b.temp[1]++; }
      if (r.w[1] !== null) { b.rain[0] += r.w[1]; b.rain[1]++; }
    }
    if (sid) {
      let i = colCache.get(r.st);
      if (i === undefined) { i = r.st.findIndex((s) => s[0] === sid); colCache.set(r.st, i); }
      if (i >= 0) {
        meta = { sid, name: r.st[i][1], cap: r.st[i][2] };
        b.cap = Math.max(b.cap, r.st[i][2]);
        if (r.b[i] !== null && r.b[i] !== undefined) { b.bikes[0] += r.b[i]; b.bikes[1]++; }
      }
    }
  }
  const keys = [...buckets.keys()].sort((a, b) => a - b);
  const avg = (p, d = 1) => (p[1] ? +(p[0] / p[1]).toFixed(d) : null);
  const per = stepS / SLOT_S;
  const out = { from, to, step: stepS / 60, t: [], cover: [], avail: [], docks: [], dis: [], dep: [], ret: [], empty: [], full: [], temp: [], rain: [] };
  if (sid) { out.station = meta; out.bikes = []; out.cap = []; }
  for (const k of keys) {
    const b = buckets.get(k);
    out.t.push(k); out.cover.push(+(b.n / per).toFixed(2));
    out.avail.push(avg(b.avail, 0)); out.docks.push(avg(b.docks, 0)); out.dis.push(avg(b.dis, 0));
    // retraits / retours ramenés au pas complet (une heure à moitié relevée ne compte pas pour moitié)
    out.dep.push(b.flow ? Math.round(b.dep * per / b.flow) : null);
    out.ret.push(b.flow ? Math.round(b.ret * per / b.flow) : null);
    out.empty.push(avg(b.empty, 1)); out.full.push(avg(b.full, 1));
    out.temp.push(avg(b.temp, 1)); out.rain.push(avg(b.rain, 2));
    if (sid) { out.bikes.push(avg(b.bikes, 1)); out.cap.push(b.cap || null); }
  }
  return out;
}

// Un résumé par jour : tranches relevées, retraits, moyenne / creux / pic des vélos disponibles,
// stations·heures vides et pleines de 7 h à 22 h (les heures de la vue Service), heures de pluie
export function dailySummary(recsByDay) {
  const out = [];
  for (const [day, recs] of recsByDay) {
    let n = 0, dep = 0, ret = 0, flow = 0, avS = 0, avN = 0, avMin = null, avMax = null, emptyS = 0, fullS = 0, svc = 0, rainS = 0;
    for (const r of recs) {
      n++;
      if (!r.n) continue;
      dep += r.n[4]; ret += r.n[5]; flow++;
      avS += r.n[0]; avN++;
      avMin = avMin === null ? r.n[0] : Math.min(avMin, r.n[0]);
      avMax = avMax === null ? r.n[0] : Math.max(avMax, r.n[0]);
      const h = luxParts(r.t).h;
      if (h >= 7 && h < 22) { svc++; emptyS += r.n[6]; fullS += r.n[7]; }
      if (r.w && r.w[1] > 0.1) rainS++;
    }
    out.push({ day, slots: n, dep, ret, depFull: flow ? Math.round(dep * 288 / flow) : null,
      availMean: avN ? Math.round(avS / avN) : null, availMin: avMin, availMax: avMax,
      emptyH: +(emptyS / 12).toFixed(1), fullH: +(fullS / 12).toFixed(1), svcSlots: svc, rainH: +(rainS / 12).toFixed(1) });
  }
  return out;
}

// ── Tranches compactes (aperçu autonome) : vélos par station en colonnes, delta d'une tranche à
// l'autre (presque toujours 0 ou ±1 : la compression gzip fait le reste). v + 1, 0 = inconnu.
export function packRecords(recs) {
  const st = recs.length ? recs.at(-1).st : [];
  const S = st.length, T = recs.length;
  const cols = new Map(st.map((s, i) => [s[0], i]));
  const b = new Uint8Array(S * T);
  for (let k = 0; k < T; k++) {
    const r = recs[k];
    r.st.forEach((s, i) => { const c = cols.get(s[0]); if (c !== undefined && r.b[i] !== null) b[c * T + k] = Math.min(254, r.b[i]) + 1; });
  }
  for (let c = 0; c < S; c++) for (let k = T - 1; k > 0; k--) b[c * T + k] = (b[c * T + k] - b[c * T + k - 1]) & 255;
  return { st, t: recs.map((r) => r.t), n: recs.map((r) => r.n), w: recs.map((r) => r.w), b };
}
export function unpackRecords(p) {
  const S = p.st.length, T = p.t.length;
  const b = p.b;
  for (let c = 0; c < S; c++) for (let k = 1; k < T; k++) b[c * T + k] = (b[c * T + k] + b[c * T + k - 1]) & 255;
  const out = [];
  for (let k = 0; k < T; k++) {
    const row = new Array(S);
    for (let c = 0; c < S; c++) { const v = b[c * T + k]; row[c] = v ? v - 1 : null; }
    out.push({ t: p.t[k], n: p.n[k], w: p.w[k], b: row, st: p.st });
  }
  return out;
}
