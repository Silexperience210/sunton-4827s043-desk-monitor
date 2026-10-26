// ── Notifications sur le téléphone (ntfy) ────────────────────────────
// Le serveur tourne en permanence : il peut prévenir quand une station reste vide ou pleine, quand le
// flux Vel'OH! se tait, quand un Sunton se resynchronise, et envoyer chaque matin le résumé de la veille.
//
// Transport : ntfy (application libre iOS / Android, serveur public ntfy.sh ou auto-hébergé, une app
// Umbrel existe). On s'abonne dans l'app au sujet choisi, c'est tout. Publication en JSON (accents sûrs) :
// POST <racine du serveur ntfy> {topic, title, message, priority, tags, click}. En plus, ou à la place,
// un webhook générique reçoit le même objet en JSON (Home Assistant, n8n, Gotify via un relais…).
//
// Réglages : variables VN_NTFY_* (valeurs de départ), puis la page (enregistrés dans <données>/notify.json).
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { atomicWrite, luxParts } from './store.mjs';

const env = (k, d = '') => (process.env[k] === undefined ? d : process.env[k]);
const ST_EMPTY = 0, ST_FULL = 4;
const AL_FEED_FROZEN = 10;

export const DEFAULTS = {
  url: env('VN_NTFY_URL'),                 // https://ntfy.sh/<sujet-difficile-à-deviner>
  token: env('VN_NTFY_TOKEN'),             // jeton d'accès ntfy (serveur protégé), facultatif
  webhook: env('VN_WEBHOOK_URL'),          // POST JSON générique, facultatif
  link: env('VN_PUBLIC_URL'),              // adresse de la page, ouverte en touchant la notification
  ruptures: env('VN_NOTIFY_RUPTURES', '1') !== '0',
  minEpisode: parseInt(env('VN_NOTIFY_MIN', '15'), 10) || 15,      // minutes de rupture avant d'avertir
  resolved: env('VN_NOTIFY_RESOLVED', '1') !== '0',                 // « rétablie » quand une rupture signalée finit
  predicted: env('VN_NOTIFY_PREDICTED', '0') === '1',               // rupture prévue dans ≤ 30 min (bavard)
  feed: env('VN_NOTIFY_FEED', '1') !== '0',
  devices: env('VN_NOTIFY_DEVICES', '1') !== '0',
  digest: env('VN_NOTIFY_DIGEST', '07:30'),                        // résumé du matin, '' = aucun
  hours: env('VN_NOTIFY_HOURS', '7-22'),                            // ruptures signalées dans ces heures seulement
  maxPerHour: parseInt(env('VN_NOTIFY_MAX_H', '6'), 10) || 6,
  stations: env('VN_NOTIFY_STATIONS').split(',').map((s) => s.trim()).filter(Boolean),   // vide = toutes
};

const fmtMin = (m) => (m < 60 ? `${m} min` : `${Math.floor(m / 60)} h ${String(m % 60).padStart(2, '0')}`);
const inHours = (spec, h) => {
  const m = String(spec || '').match(/^(\d{1,2})\s*-\s*(\d{1,2})$/);
  if (!m) return true;
  const a = +m[1], b = +m[2];
  return a <= b ? h >= a && h < b : h >= a || h < b;
};

export class Notifier {
  constructor({ dataDir, log = () => {}, fetchImpl = fetch, now = () => Math.floor(Date.now() / 1000) }) {
    this.file = join(dataDir, 'notify.json');
    this.log = log;
    this.fetch = fetchImpl;
    this.now = now;
    this.cfg = { ...DEFAULTS };
    this.recent = [];
    this.sentTimes = [];
    this.ep = new Map();        // sid → {type, since, notified}
    this.pred = new Map();      // sid → {type, at}
    this.first = true;
    this.feedDown = null;       // {since, notified}
    this.frozen = null;
    this.lastDigestDay = '';
    this.lastError = '';
    try {
      const j = JSON.parse(readFileSync(this.file, 'utf8'));
      if (j.config) Object.assign(this.cfg, j.config);
      if (Array.isArray(j.recent)) this.recent = j.recent.slice(-40);
      if (j.lastDigestDay) this.lastDigestDay = j.lastDigestDay;
    } catch { /* pas encore de réglages */ }
  }
  get enabled() { return !!(this.cfg.url || this.cfg.webhook); }
  persist() {
    try { atomicWrite(this.file, JSON.stringify({ config: this.cfg, recent: this.recent.slice(-40), lastDigestDay: this.lastDigestDay }, null, 1)); }
    catch (err) { this.log(`[notif] réglages non enregistrés : ${err.message}`); }
  }
  // Réglages vus par la page : le jeton ntfy n'est jamais renvoyé en clair
  publicConfig() {
    const { token, ...rest } = this.cfg;
    return { ...rest, tokenSet: !!token };
  }
  update(patch) {
    const c = { ...this.cfg };
    const str = (k, max = 500) => { if (patch[k] !== undefined) c[k] = String(patch[k]).trim().slice(0, max); };
    const bool = (k) => { if (patch[k] !== undefined) c[k] = !!patch[k]; };
    str('url'); str('webhook'); str('link'); str('digest', 5); str('hours', 5);
    if (patch.token !== undefined && patch.token !== '••••') c.token = String(patch.token).trim().slice(0, 200);
    for (const k of ['ruptures', 'resolved', 'predicted', 'feed', 'devices']) bool(k);
    if (patch.minEpisode !== undefined) c.minEpisode = Math.min(240, Math.max(5, parseInt(patch.minEpisode, 10) || 15));
    if (patch.maxPerHour !== undefined) c.maxPerHour = Math.min(60, Math.max(1, parseInt(patch.maxPerHour, 10) || 6));
    if (patch.stations !== undefined) c.stations = (Array.isArray(patch.stations) ? patch.stations : String(patch.stations).split(',')).map((s) => String(s).trim()).filter(Boolean).slice(0, 200);
    for (const k of ['url', 'webhook', 'link']) if (c[k] && !/^https?:\/\/[^\s]+$/i.test(c[k])) throw new Error(`adresse invalide (${k}) : http:// ou https:// attendu`);
    if (c.digest && !/^([01]?\d|2[0-3]):[0-5]\d$/.test(c.digest)) throw new Error('heure du résumé invalide (HH:MM, ou vide)');
    if (c.hours && !/^\d{1,2}-\d{1,2}$/.test(c.hours)) throw new Error('heures invalides (par exemple 7-22)');
    this.cfg = c;
    this.persist();
    return this.publicConfig();
  }

  // ── Envoi ──
  // ntfy : https://ntfy.sh/mon-sujet → POST https://ntfy.sh/ {topic: "mon-sujet", …}
  ntfyTarget(url) {
    const u = new URL(url);
    const parts = u.pathname.split('/').filter(Boolean);
    const topic = parts.pop();
    if (!topic) throw new Error('adresse ntfy sans sujet (https://ntfy.sh/<sujet>)');
    return { root: `${u.origin}/${parts.length ? parts.join('/') + '/' : ''}`, topic };
  }
  async send(msg, { force = false, kind = 'info' } = {}) {
    if (!this.enabled) return { ok: false, why: 'aucune destination réglée' };
    const t = this.now();
    this.sentTimes = this.sentTimes.filter((x) => t - x < 3600);
    if (!force && this.sentTimes.length >= this.cfg.maxPerHour) return { ok: false, why: 'plafond horaire atteint', limited: true };
    const body = { title: msg.title, message: msg.message, priority: msg.priority || 3, tags: msg.tags || [] };
    if (msg.click || this.cfg.link) body.click = msg.click || this.cfg.link;
    const results = [];
    const post = async (url, json, headers = {}) => {
      const ac = new AbortController();
      const to = setTimeout(() => ac.abort(), 10000);
      try {
        const r = await this.fetch(url, { method: 'POST', signal: ac.signal, headers: { 'Content-Type': 'application/json', ...headers }, body: JSON.stringify(json) });
        const txt = await r.text().catch(() => '');
        return r.ok ? { ok: true } : { ok: false, why: `HTTP ${r.status}${txt ? ' ' + txt.slice(0, 120) : ''}` };
      } catch (err) { return { ok: false, why: ac.signal.aborted ? 'délai dépassé' : String(err.cause?.code || err.message || err) }; }
      finally { clearTimeout(to); }
    };
    if (this.cfg.url) {
      try {
        const { root, topic } = this.ntfyTarget(this.cfg.url);
        results.push(await post(root, { topic, ...body }, this.cfg.token ? { Authorization: `Bearer ${this.cfg.token}` } : {}));
      } catch (err) { results.push({ ok: false, why: err.message }); }
    }
    if (this.cfg.webhook) results.push(await post(this.cfg.webhook, { source: 'velohnav-ops', kind, t, ...body }));
    const ok = results.some((r) => r.ok);
    const why = results.filter((r) => !r.ok).map((r) => r.why).join(' ; ');
    this.lastError = ok ? (why ? `partiel : ${why}` : '') : why;
    this.sentTimes.push(t);
    this.recent.push({ t, kind, title: body.title, message: body.message, priority: body.priority, ok, why: ok ? undefined : why });
    if (this.recent.length > 40) this.recent.splice(0, this.recent.length - 40);
    this.persist();
    this.log(`[notif] ${ok ? 'envoyée' : 'ÉCHEC'} · ${body.title}${ok ? '' : ` — ${why}`}`);
    return { ok, why };
  }
  test() {
    return this.send({ title: 'VelohNav Ops : test', message: 'Les notifications arrivent bien sur ce téléphone.', priority: 3, tags: ['white_check_mark'] }, { force: true, kind: 'test' });
  }

  // ── Après chaque cycle de relevé ──
  // stations : engine.stations() ; summary : engine.summary() ; alerts : engine.alerts()
  async evaluate({ stations, summary, alerts, refreshS = 60 }) {
    const t = this.now();
    const lp = luxParts(t);
    const c = this.cfg;
    const watch = (s) => !c.stations.length || c.stations.includes(s.sid);
    const out = [];

    // 1. ruptures qui durent
    const open = [], closed = [];
    const seen = new Set();
    for (const s of stations) {
      if (!s.active || !s.live) continue;
      const type = s.status === ST_EMPTY ? 'vide' : s.status === ST_FULL ? 'pleine' : null;
      if (!type) continue;
      seen.add(s.sid);
      let e = this.ep.get(s.sid);
      if (!e || e.type !== type) {
        const since = (type === 'vide' ? s.emptySince : s.fullSince) || t;
        // au premier passage, les ruptures déjà en cours ne sont pas annoncées (le résumé du matin les donne)
        e = { type, since, notified: this.first, s };
        this.ep.set(s.sid, e);
      }
      e.s = s;
      const mins = Math.floor((t - e.since) / 60);
      if (!e.notified && mins >= c.minEpisode && watch(s)) open.push({ e, s, mins });
    }
    for (const [sid, e] of this.ep) {
      if (seen.has(sid)) continue;
      this.ep.delete(sid);
      if (e.notified === 'sent' && c.resolved) closed.push({ e, mins: Math.floor((t - e.since) / 60) });
    }
    const quiet = !inHours(c.hours, lp.h);
    if (c.ruptures && open.length) {
      if (quiet) for (const o of open) o.e.notified = true;          // hors des heures : le résumé du matin s'en charge
      else {
        open.sort((a, b) => b.mins - a.mins);
        const line = (o) => `${o.s.name} — ${o.e.type} depuis ${fmtMin(o.mins)}${o.e.type === 'vide' ? `, ${o.s.docks} bornes libres` : `, ${o.s.bikes} vélos`}` +
          `${o.s.need ? ` · ${o.s.need > 0 ? 'apporter' : 'retirer'} ${Math.abs(o.s.need)}` : ''}`;
        const msg = open.length === 1
          ? { title: `Station ${open[0].e.type} : ${open[0].s.name}`, message: line(open[0]) + `\nPrévision à +1 h : ${Math.round(open[0].s.fc60)} vélo(s)`, priority: 4, tags: [open[0].e.type === 'vide' ? 'bike' : 'parking'] }
          : { title: `${open.length} stations en rupture`, message: open.slice(0, 8).map((o) => '• ' + line(o)).join('\n') + (open.length > 8 ? `\n… et ${open.length - 8} autre(s)` : ''), priority: 4, tags: ['warning'] };
        const r = await this.send(msg, { kind: 'rupture' });
        if (r.ok || !r.limited) for (const o of open) o.e.notified = 'sent';
        out.push(msg);
      }
    }
    if (c.ruptures && closed.length && !quiet) {
      const msg = { title: closed.length === 1 ? `Rétablie : ${closed[0].e.s.name}` : `${closed.length} stations rétablies`,
        message: closed.map((o) => `${o.e.s.name} — ${o.e.type} pendant ${fmtMin(o.mins)}`).join('\n'), priority: 2, tags: ['white_check_mark'] };
      await this.send(msg, { kind: 'retablie' });
      out.push(msg);
    }

    // 2. ruptures prévues (échéance ≤ 30 min), une fois par épisode
    if (c.predicted && !quiet) {
      const soon = [];
      for (const s of stations) {
        if (!s.active || !s.live || !watch(s) || s.status === ST_EMPTY || s.status === ST_FULL) { this.pred.delete(s.sid); continue; }
        const type = s.etaEmpty > 0 && s.etaEmpty <= 30 ? 'vide' : s.etaFull > 0 && s.etaFull <= 30 ? 'pleine' : null;
        if (!type) { this.pred.delete(s.sid); continue; }
        const p = this.pred.get(s.sid);
        if (p && p.type === type) continue;
        this.pred.set(s.sid, { type, at: t });
        if (!this.first) soon.push({ s, type, eta: type === 'vide' ? s.etaEmpty : s.etaFull });
      }
      if (soon.length) {
        const msg = { title: soon.length === 1 ? `Bientôt ${soon[0].type} : ${soon[0].s.name}` : `${soon.length} ruptures prévues`,
          message: soon.slice(0, 8).map((o) => `${o.s.name} — ${o.type} dans ~${o.eta} min (${o.s.bikes} vélos, ${o.s.docks} bornes)`).join('\n'), priority: 3, tags: ['hourglass'] };
        await this.send(msg, { kind: 'prevue' });
        out.push(msg);
      }
    }

    // 3. flux muet ou figé
    if (c.feed && summary) {
      const limit = Math.max(300, 3 * refreshS + 60);
      const age = summary.lastUpdate ? t - summary.lastUpdate : null;
      if (age !== null && age > limit) {
        if (!this.feedDown) this.feedDown = { since: summary.lastUpdate, notified: false };
        if (!this.feedDown.notified) {
          this.feedDown.notified = true;
          const msg = { title: 'Flux Vel\'OH! muet', message: `Aucun relevé réussi depuis ${fmtMin(Math.floor(age / 60))} (${summary.fetchErrors || 0} échec(s)). Les Sunton affichent la même chose.`, priority: 4, tags: ['rotating_light'] };
          await this.send(msg, { kind: 'flux', force: true });
          out.push(msg);
        }
      } else if (this.feedDown && age !== null) {
        const d = this.feedDown;
        this.feedDown = null;
        if (d.notified) {
          const msg = { title: 'Flux Vel\'OH! rétabli', message: `Relevés à nouveau reçus après ${fmtMin(Math.floor((summary.lastUpdate - d.since) / 60))} de silence.`, priority: 3, tags: ['white_check_mark'] };
          await this.send(msg, { kind: 'flux', force: true });
          out.push(msg);
        }
      }
      const frozen = summary.feedFrozen ?? (alerts || []).some((a) => a.type === AL_FEED_FROZEN);
      if (frozen && !this.frozen) {
        this.frozen = { since: t };
        if (!this.first) {
          const msg = { title: 'Flux Vel\'OH! figé', message: 'Le flux répond, mais plus aucune station ne bouge : les chiffres affichés ne changent plus.', priority: 4, tags: ['ice_cube'] };
          await this.send(msg, { kind: 'flux', force: true });
          out.push(msg);
        }
      } else if (!frozen && this.frozen) {
        const mins = Math.floor((t - this.frozen.since) / 60);
        this.frozen = null;
        const msg = { title: 'Flux Vel\'OH! reparti', message: `Les stations bougent à nouveau (figé ${fmtMin(mins)}).`, priority: 2, tags: ['white_check_mark'] };
        await this.send(msg, { kind: 'flux', force: true });
        out.push(msg);
      }
    }
    this.first = false;
    return out;
  }

  // ── Un Sunton vient de se synchroniser ──
  async device(d) {
    if (!this.cfg.devices || !d || !d.took) return null;
    const msg = { title: `Sunton resynchronisé : ${d.name || d.id}`, message: `A repris ${d.took}${d.fw ? ` (firmware ${d.fw})` : ''}.`, priority: 2, tags: ['arrows_counterclockwise'] };
    await this.send(msg, { kind: 'sunton' });
    return msg;
  }

  // ── Résumé du matin : la veille (vue Service), la prévision notée, l'état du moment ──
  digestDue() {
    if (!this.cfg.digest || !this.enabled) return false;
    const lp = luxParts(this.now());
    const day = `${lp.y}-${lp.mo}-${lp.d}`;
    const [hh, mm] = this.cfg.digest.split(':').map(Number);
    return day !== this.lastDigestDay && (lp.h > hh || (lp.h === hh && lp.mi >= mm)) && lp.h < hh + 3;
  }
  buildDigest({ digest, note, stations, devices = [], dateLabel }) {
    const L = [];
    if (digest && digest.recorded) {
      L.push(`${digest.label} : ${digest.availText} du temps disponible · ${digest.evEmpty} passage(s) à vide, ${digest.evFull} à plein`);
      if (digest.worst?.length) L.push('Pires : ' + digest.worst.map((w) => `${w.name} ${w.badText}`).join(', '));
      for (const r of (digest.recur || []).slice(0, 3)) L.push(`Récurrent — ${r.name} : ${r.text}`);
      if (digest.recurrent > 3) L.push(`… ${digest.recurrent - 3} autre(s) problème(s) récurrent(s)`);
    } else L.push(`${digest?.label || 'Hier'} : pas assez de relevés pour un résumé.`);
    if (note && note.model?.[2]?.n) {
      const m = note.model[2], p = note.per[2];
      const fmt = (x) => x.toFixed(2).replace('.', ',');
      L.push(`Prévision à +1 h : erreur moyenne ${fmt(m.mae)} vélo (persistance ${fmt(p.mae)}) sur ${m.n} prévisions`);
    }
    const empty = stations.filter((s) => s.active && s.live && s.status === ST_EMPTY);
    const full = stations.filter((s) => s.active && s.live && s.status === ST_FULL);
    L.push(`En ce moment : ${empty.length} vide(s), ${full.length} pleine(s)` + (empty.length ? ` — ${empty.slice(0, 5).map((s) => s.name).join(', ')}${empty.length > 5 ? '…' : ''}` : ''));
    const yest = devices.filter((d) => d.lastReport && this.now() - d.lastReport < 86400);
    if (yest.length) L.push(`Sunton synchronisés en 24 h : ${yest.map((d) => d.name || d.id).join(', ')}`);
    return { title: `VelohNav Ops · ${dateLabel}`, message: L.join('\n'), priority: 3, tags: ['sunrise'] };
  }
  async sendDigest(parts) {
    const lp = luxParts(this.now());
    this.lastDigestDay = `${lp.y}-${lp.mo}-${lp.d}`;
    const msg = this.buildDigest(parts);
    await this.send(msg, { kind: 'resume', force: true });
    return msg;
  }
  status() {
    const t = this.now();
    return { enabled: this.enabled, config: this.publicConfig(), recent: this.recent.slice(-15).reverse(), lastError: this.lastError,
      sentLastHour: this.sentTimes.filter((x) => t - x < 3600).length, openRuptures: [...this.ep.values()].filter((e) => e.notified === 'sent').length };
  }
}
