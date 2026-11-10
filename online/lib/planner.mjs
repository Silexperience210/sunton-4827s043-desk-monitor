// ── Planificateur de tournées de régulation ──────────────────────────
// Pour chaque camion en tournée (capacité et chargement DÉCLARÉS), une suite d'arrêts : prendre / déposer,
// combien, dans quel ordre. Le critère, dit tel quel à l'écran : minutes de rupture (vide ou pleine) ÉVITÉES
// d'après la prévision de la carte — une ESTIMATION, dont l'erreur est mesurée en continu (onglet Prévisions).
//
// Pour chaque station, la courbe prévue 0 → 240 min (Model::project, pas de 5 min) ; une dépose de q vélos à
// l'arrivée du camion décale la courbe de +q à partir de cet instant (bornée par 0 et la capacité), une prise
// de −q, un ramassage de vélos hors service libère autant de bornes. Minutes de rupture comptées sur les 2 h
// qui suivent l'arrivée. Recherche en faisceau sur les prochains arrêts (au plus maxStops, au plus horizonMin
// de tournée), les arrêts lointains pesant moins (la prévision s'y dégrade). Deux camions : le second est
// planifié avec les actions du premier déjà appliquées. Le prochain arrêt n'est changé que si le gain le justifie.
//
// Durées : vol d'oiseau × détour ÷ vitesse + temps d'arrêt — ESTIMÉES ; dès 20 trajets déclarés, la vitesse
// mesurée sur les tournées réelles remplace la valeur par défaut (et l'écran le dit).
import { haversineKm } from './regulation.mjs';

const STEP = 5;                 // minutes par pas de courbe
const NSTEP = 49;               // 0 … 240 min
const WIN = 24;                 // 2 h de conséquences comptées après l'arrivée
const BEAM = 40;
const fmtHM = (t) => new Intl.DateTimeFormat('fr-FR', { timeZone: 'Europe/Luxembourg', hour: '2-digit', minute: '2-digit' }).format(new Date(t * 1000));

// Marge d'incertitude : une station compte « en rupture » au pas k si sa courbe prévue passe sous 0,5 vélo
// PLUS l'erreur moyenne de la prévision à cet horizon (ou au-dessus de la capacité moins cette marge).
// L'erreur vient du journal des prévisions en continu (onglet Prévisions) dès 500 prévisions notées à +1 h ;
// avant, celle du banc simulé — et le plan dit laquelle il utilise. Sans marge, déposer un seul vélo dans une
// station vide « réglerait » tout sur le papier : le premier client le prend.
const HKM = [15, 30, 60, 120, 240];
const ERR_BANC = [0.41, 0.70, 1.10, 1.61, 2.19];        // MAE du banc simulé (docs/mesures-prevision.md)
const fr2 = (v) => Number(v).toFixed(2).replace('.', ',');
export function errorProfile(report) {
  const tot = report?.total?.model, per = report?.total?.per;
  // mesurée dès 500 prévisions à +1 h ; un horizon encore peu mesuré (< 200) garde la valeur du banc, et le dit
  const measured = !!(tot && tot[2]?.n >= 500 && Number.isFinite(tot[2].mae));
  const fromBanc = [];
  const e = ERR_BANC.map((b, j) => {
    if (measured && tot[j]?.n >= 200 && Number.isFinite(tot[j].mae)) return tot[j].mae;
    if (measured) fromBanc.push(['+15 min', '+30 min', '+1 h', '+2 h', '+4 h'][j]);
    return b;
  });
  const out = new Array(NSTEP).fill(0);
  for (let k = 1; k < NSTEP; k++) {
    const m = k * STEP;
    let j = HKM.findIndex((h) => h >= m);
    if (j < 0) j = HKM.length - 1;
    const h0 = j ? HKM[j - 1] : 0, e0 = j ? e[j - 1] : 0;
    out[k] = +(e0 + (e[j] - e0) * Math.min(1, (m - h0) / (HKM[j] - h0))).toFixed(3);
  }
  // le modèle bat-il la simple persistance (« rien ne bouge ») sur les mesures réelles à +1 h ?
  const beats = measured && Number.isFinite(per?.[2]?.mae) ? tot[2].mae < per[2].mae : null;
  const source = measured
    ? `erreur mesurée (${tot[2].n} prévisions à +1 h${fromBanc.length ? ` ; ${fromBanc.join(', ')} : banc simulé, pas encore assez mesuré` : ''})`
    : 'erreur du banc simulé (pas encore assez de mesures réelles)';
  const caution = beats === false
    ? `Attention : sur les mesures réelles, la prévision ne fait pas mieux que « rien ne bouge » à +1 h (erreur moyenne ${fr2(tot[2].mae)} vélo contre ${fr2(per[2].mae)}) — les risques annoncés sont à prendre avec prudence.`
    : '';
  return { err: out, measured, source, beats, caution };
}
let ERR = new Array(NSTEP).fill(0);
// minutes de rupture d'une courbe sur [k0, k1], capacité cap ; mod = {k, q, h} appliqué à partir de k
function rupture(curve, cap, k0, k1, mod) {
  let m = 0;
  for (let k = k0; k <= k1; k++) {
    let c = curve[k], C = cap;
    if (mod && k >= mod.k) { C = cap + mod.h; c = Math.min(C, Math.max(0, c + mod.q)); }
    const e = ERR[k];
    if (c < 0.5 + e) m += STEP; else if (c > C - 0.5 - e) m += STEP;
  }
  return m;
}
// gain d'une action, mémorisé pendant un calcul de plan (les mêmes arrivées reviennent dans tout le faisceau)
function gain(S, ka, q, h) {
  const key = (ka * 512 + (q + 128)) * 64 + h;
  let m = S.memo.get(key);
  if (m === undefined) {
    const k1 = Math.min(NSTEP - 1, ka + WIN);
    m = rupture(S.curve, S.cap, ka, k1, null) - rupture(S.curve, S.cap, ka, k1, { k: ka, q, h });
    S.memo.set(key, m);
  }
  return m;
}
const weight = (tMin) => (tMin <= 30 ? 1 : Math.max(0.5, 1 - 0.5 * (tMin - 30) / 90));

// Vitesse mesurée sur les tournées déclarées : trajets entre deux arrêts validés consécutifs
export function measuredSpeed(samples, cfg) {
  const v = [];
  for (const s of samples) {
    const handling = cfg.stopMin + cfg.perBikeMin * s.bikes;
    const drive = s.min - handling;
    if (s.km < 0.2 || drive < 1 || s.min > 90) continue;
    v.push((s.km * cfg.detour) / (drive / 60));
  }
  if (v.length < 20) return { kmh: cfg.speedKmh, measured: false, n: v.length };
  v.sort((a, b) => a - b);
  return { kmh: +v[Math.floor(v.length / 2)].toFixed(1), measured: true, n: v.length };
}

export function planTours({ now, curves, stations, tours, cfg, speed, previous = new Map(), error = errorProfile(null), exclude = new Set() }) {
  const P = cfg.plan;
  ERR = error.err;
  const kmh = speed?.kmh || P.speedKmh;
  const atelier = Number.isFinite(cfg.atelier?.lat) && Number.isFinite(cfg.atelier?.lon) ? { lat: cfg.atelier.lat, lon: cfg.atelier.lon } : null;
  let stock = Math.max(0, cfg.atelierStock?.ok || 0);
  // stations planifiables : vivantes, ouvertes, avec une courbe
  const S = [];
  stations.forEach((s, i) => {
    const c = curves.st[i];
    if (!c || !s.active || !s.live || s.status === 5) return;     // 5 = ST_CLOSED
    S.push({ i, sid: s.sid, name: s.name, lat: s.lat, lon: s.lon, cap: c[0], bikes: c[1], docks: c[2], dis: c[3], status: c[4], curve: c.slice(5), memo: new Map() });
  });
  const travel = (a, b) => (a ? (haversineKm(a, b) * P.detour / kmh) * 60 : 0);
  const out = [];
  // stations déjà servies (déclaré) depuis moins de 30 min : le flux ne le montre peut-être pas encore
  const taken = new Set(exclude);

  for (const tour of tours) {
    const start = tour.position || atelier;
    const cap = tour.capacity;
    const prevFirst = previous.get(tour.id)?.stops?.[0] || null;

    const expand = (st) => {
      const kids = [];
      const space = cap - st.ok - st.hs;
      const cands = [];
      for (const s of S) {
        if (taken.has(s.sid) || st.touched.has(s.sid)) continue;
        const tt = travel(st.pos, s);
        const ta = st.t + tt;
        if (ta > P.horizonMin) continue;
        const ka = Math.min(NSTEP - 1, Math.round(ta / STEP));
        const cNow = s.curve[ka];
        const opts = [];
        // dépose : la plus petite quantité qui obtient le gain maximal, et celle qui vise le remplissage cible
        if (st.ok > 0) {
          const qMax = Math.min(st.ok, Math.floor(s.cap - cNow));
          let best = 0, qb = 0;
          for (let q = 1; q <= qMax; q++) { const g = gain(s, ka, q, 0); if (g > best) { best = g; qb = q; } }
          if (best > 0) {
            opts.push({ dOk: qb, tOk: 0, g: best });
            const qt = Math.min(qMax, Math.max(qb, Math.round(P.targetFill * s.cap - cNow)));
            if (qt > qb) opts.push({ dOk: qt, tOk: 0, g: gain(s, ka, qt, 0) });
          }
        }
        // prise : soulager une station pleine, ou prendre dans une réserve sans créer de rupture sur 2 h
        if (space > 0) {
          const qMax = Math.min(space, Math.floor(cNow));
          let best = 0, qb = 0;
          for (let q = 1; q <= qMax; q++) { const g = gain(s, ka, -q, 0); if (g > best) { best = g; qb = q; } }
          if (best > 0) opts.push({ dOk: 0, tOk: qb, g: best, why: 'full' });
          const keep = Math.max(2, Math.ceil(0.3 * s.cap));
          let minC = Infinity;
          for (let k = ka; k <= Math.min(NSTEP - 1, ka + WIN); k++) minC = Math.min(minC, s.curve[k]);
          const qs = Math.min(space, Math.floor(minC - keep));
          if (qs >= 2 && gain(s, ka, -qs, 0) >= 0 && qs !== qb) opts.push({ dOk: 0, tOk: qs, g: gain(s, ka, -qs, 0), why: 'reserve' });
        }
        for (const o of opts) {
          // vélos hors service ramassés au passage s'il reste de la place (aucun détour pour eux)
          const room = cap - (st.ok - o.dOk + o.tOk) - st.hs;
          const tHs = Math.max(0, Math.min(s.dis, room));
          const g = tHs ? gain(s, ka, o.tOk ? -o.tOk : o.dOk, tHs) : o.g;
          const bikes = o.dOk + o.tOk + tHs;
          const dur = tt + P.stopMin + P.perBikeMin * bikes;
          cands.push({ s, ka, ta, dur, g, o, tHs, val: (g * weight(ta) + 0.01) / (dur + 1) });
        }
      }
      // atelier : décharger les vélos hors service, reprendre des vélos neufs s'il y en a de prêts (déclaré)
      if (atelier && !st.touched.has('@atelier') && (st.hs > 0 || (stock > 0 && st.ok < cap / 2))) {
        const tt = travel(st.pos, atelier);
        if (st.t + tt <= P.horizonMin) {
          const newOk = Math.min(stock, cap - st.ok);
          const bikes = st.hs + newOk;
          cands.push({ atelier: true, ta: st.t + tt, dur: tt + P.stopMin + P.perBikeMin * bikes, g: 0, dropHs: st.hs, newOk, val: 0.001 });
        }
      }
      cands.sort((a, b) => b.val - a.val);
      for (const c of cands.slice(0, 28)) {
        const touched = new Set(st.touched);
        if (c.atelier) {
          touched.add('@atelier');
          kids.push({ pos: atelier, t: st.t + c.dur, ok: st.ok + c.newOk, hs: 0, score: st.score - 0.05 * c.dur, touched,
            stops: [...st.stops, { kind: 'atelier', ta: c.ta, tEnd: st.t + c.dur, dropHs: c.dropHs, newOk: c.newOk, g: 0, okAfter: st.ok + c.newOk, hsAfter: 0 }] });
          continue;
        }
        touched.add(c.s.sid);
        const ok = st.ok - c.o.dOk + c.o.tOk, hs = st.hs + c.tHs;
        kids.push({ pos: c.s, t: st.t + c.dur, ok, hs, score: st.score + c.g * weight(c.ta) - 0.05 * c.dur, touched,
          stops: [...st.stops, { kind: 'station', s: c.s, ka: c.ka, ta: c.ta, tEnd: st.t + c.dur, dOk: c.o.dOk, tOk: c.o.tOk, tHs: c.tHs, g: c.g, why: c.o.why, okAfter: ok, hsAfter: hs }] });
      }
      return kids;
    };

    const search = (root, forceFirst) => {
      let beam = [root], best = root;
      for (let d = 0; d < P.maxStops; d++) {
        let next = [];
        for (const st of beam) next.push(...expand(st));
        if (d === 0 && forceFirst) next = next.filter((k) => (forceFirst === '@atelier' ? k.stops[0].kind === 'atelier' : k.stops[0].s?.sid === forceFirst));
        if (!next.length) break;
        next.sort((a, b) => b.score - a.score);
        beam = next.slice(0, BEAM);
        if (beam[0].score > best.score) best = beam[0];
      }
      // couper la fin du plan qui n'apporte rien (prise sans dépose derrière, passage à l'atelier final)
      const stops = [...best.stops];
      while (stops.length && !(stops.at(-1).g > 0)) stops.pop();
      return { ...best, stops, t: stops.length ? stops.at(-1).tEnd : 0 };
    };
    const root = { pos: start, t: 0, ok: tour.load.ok, hs: tour.load.hs, score: 0, touched: new Set(), stops: [] };
    let best = search(root, null);
    let kept = false;
    if (prevFirst && best.stops[0] && (prevFirst.kind === 'atelier' ? best.stops[0].kind !== 'atelier' : best.stops[0].s?.sid !== prevFirst.sid)) {
      const alt = search(root, prevFirst.kind === 'atelier' ? '@atelier' : prevFirst.sid);
      if (alt.stops.length && alt.score >= 0.85 * best.score) { best = alt; kept = true; }
    }
    const stops = best.stops.map((p, k) => {
      if (p.kind === 'atelier') {
        return { seq: k + 1, kind: 'atelier', name: cfg.atelier.name || 'Atelier', lat: atelier.lat, lon: atelier.lon,
          drop: { ok: 0, hs: p.dropHs }, take: { ok: p.newOk, hs: 0 }, etaMin: Math.round(p.ta), arriveAt: now + Math.round(p.ta * 60),
          benefitMin: 0, why: [p.dropHs ? `décharger ${p.dropHs} vélo(s) hors service` : null, p.newOk ? `reprendre ${p.newOk} vélo(s) prêts (stock déclaré)` : null].filter(Boolean).join(' · '),
          loadAfter: { ok: p.okAfter, hs: p.hsAfter } };
      }
      const s = p.s;
      const why = [];
      // premier pas où la courbe prévue entre dans la zone de rupture, marge d'erreur comprise
      const emptyAt = s.curve.findIndex((c, k) => c < 0.5 + ERR[k]), fullAt = s.curve.findIndex((c, k) => c > s.cap - 0.5 - ERR[k]);
      if (p.dOk) {
        if (s.bikes === 0) why.push('vide maintenant (mesuré)');
        else if (emptyAt >= 0) why.push(`risque de vide dès ${fmtHM(now + emptyAt * STEP * 60)} (estimé, marge d'erreur comprise)`);
      }
      if (p.tOk) {
        if (p.g > 0) why.push(s.docks === 0 ? 'pleine maintenant (mesuré)' : fullAt >= 0 ? `risque de station pleine dès ${fmtHM(now + fullAt * STEP * 60)} (estimé, marge d'erreur comprise)` : 'trop chargée (estimé)');
        else why.push(`réserve : ${s.bikes} vélos maintenant (mesuré), il en reste assez sur 2 h (estimé)`);
      }
      if (p.tHs) why.push(`${p.tHs} vélo(s) hors service ramassé(s) au passage (mesuré : ${s.dis} HS)`);
      return { seq: k + 1, kind: 'station', sid: s.sid, name: s.name, lat: s.lat, lon: s.lon,
        drop: { ok: p.dOk, hs: 0 }, take: { ok: p.tOk, hs: p.tHs }, etaMin: Math.round(p.ta), arriveAt: now + Math.round(p.ta * 60),
        benefitMin: Math.round(p.g), why: why.join(' · '), now: { bikes: s.bikes, docks: s.docks, dis: s.dis, cap: s.cap },
        atArrival: Math.round(s.curve[p.ka]), loadAfter: { ok: p.okAfter, hs: p.hsAfter } };
    });
    // appliquer ce plan aux courbes avant de planifier le camion suivant
    for (const p of best.stops) {
      if (p.kind !== 'station') continue;
      taken.add(p.s.sid);
      const q = p.dOk - p.tOk;
      for (let k = p.ka; k < NSTEP; k++) p.s.curve[k] = Math.min(p.s.cap + p.tHs, Math.max(0, p.s.curve[k] + q));
      p.s.cap += p.tHs;
    }
    if (best.stops.some((p) => p.kind === 'atelier')) stock = Math.max(0, stock - best.stops.filter((p) => p.kind === 'atelier').reduce((a, p) => a + p.newOk, 0));

    out.push({ tour: tour.id, truck: tour.truckName, capacity: cap, load: tour.load, startKnown: !!start, kept,
      stops, benefitMin: Math.round(stops.reduce((a, s) => a + s.benefitMin, 0)), durationMin: Math.round(best.t),
      speed: { kmh, measured: !!speed?.measured, n: speed?.n || 0 } });
  }
  return { at: now, plans: out, atelierSet: !!atelier, error: error.source,
    method: `Plan proposé : minutes de rupture évitées ESTIMÉES par la prévision de la carte (2 h après chaque arrivée), avec une marge égale à l'${error.source} ; durées estimées${speed?.measured ? ` (vitesse mesurée sur ${speed.n} trajets déclarés : ${speed.kmh} km/h)` : ` (vitesse supposée ${kmh} km/h)`}.${error.caution ? ' ' + error.caution : ''}` };
}
