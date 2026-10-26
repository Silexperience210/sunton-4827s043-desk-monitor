// ── Relevé des flux 24 h/24 : la tâche réseau de la carte, sans Wi-Fi ─
// Reprend src/net/net_esp32.cpp pas à pas : mêmes flux, mêmes périodes, mêmes trois tentatives
// espacées (800 ms × n), mêmes délais (connexion 8 s, lecture 15 s), mêmes codes d'échec, même
// ordre (vehicle_types, station_information, station_status, puis la météo à son rythme).
// Chaque appel au moteur est aussi publié comme évènement : un jumeau qui les rejoue dans l'ordre,
// aux mêmes instants, calcule exactement la même chose que le serveur.
import { FEED, ING } from '../public/vn-engine.js';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const nowS = () => Math.floor(Date.now() / 1000);

// Journal d'évènements pour les jumeaux (longue attente : /api/twin/events?since=)
export class EventLog {
  constructor(keep) { this.keep = keep; this.list = []; this.seq = 0; this.waiters = new Set(); }
  push(ev) {
    ev.seq = ++this.seq;
    this.list.push(ev);
    if (this.list.length > this.keep) this.list.splice(0, this.list.length - this.keep);
    for (const w of this.waiters) w();
    this.waiters.clear();
    return ev.seq;
  }
  // Évènements postérieurs à `since` ; null si `since` est sorti de la mémoire (le jumeau repart d'un instantané)
  after(since) {
    if (since > this.seq) return null;
    if (since === this.seq) return [];
    const first = this.list.length ? this.list[0].seq : this.seq + 1;
    if (since < first - 1) return null;
    return this.list.filter((e) => e.seq > since);
  }
  wait(ms) {
    return new Promise((res) => {
      const done = () => { clearTimeout(t); this.waiters.delete(done); res(); };
      const t = setTimeout(done, ms);
      this.waiters.add(done);
    });
  }
}

export class Collector {
  constructor({ engine, cfg, events, mono, log, onCycle }) {
    this.e = engine;
    this.cfg = cfg;
    this.events = events;
    this.mono = mono;                       // horloge monotone (ms) partagée avec le moteur
    this.log = log;
    this.onCycle = onCycle || (() => {});
    this.ua = cfg.userAgent.replace('%v', engine.version());
    this.lastFetch = 0;                     // ms monotones du dernier cycle (0 = jamais)
    this.wxAt = 0;                          // epoch du dernier essai météo (s_wxAt)
    this.refreshReq = false;
    this.running = false;
    this.busy = false;
    this.cycles = 0;
  }

  clock() { this.e.clock(nowS(), this.mono()); }

  // Un évènement = un appel au moteur, rejouable tel quel par un jumeau
  call(ev) {
    if (ev.t === undefined) ev.t = nowS();
    this.e.clock(ev.t, this.mono());
    let r;
    switch (ev.k) {
      case 'ingest': r = this.e.ingest(ev.feed, ev.got, ev.code, ev.body, ev.dur, ev.cyc); break;
      case 'cycle': this.e.cycleEnd(ev.ok); break;
      case 'fetching': this.e.setFetching(ev.on); break;
      case 'reset': break;                   // l'état a été rechargé : les jumeaux le reprennent
    }
    this.events.push(ev);
    return r;
  }

  // ── HTTP GET (httpGetOnce + httpGet de la carte) ──
  async getOnce(url) {
    const t0 = Date.now();
    const ac = new AbortController();
    const to = setTimeout(() => ac.abort(), 15000 + 8000);
    try {
      const r = await fetch(url, { signal: ac.signal, headers: { 'User-Agent': this.ua, Accept: 'application/json' } });
      if (r.status !== 200) { await r.arrayBuffer().catch(() => {}); return { code: r.status, body: null, ms: Date.now() - t0 }; }
      const buf = Buffer.from(await r.arrayBuffer());
      const len = parseInt(r.headers.get('content-length') || '-1', 10);
      if (len > 0 && buf.length < len) return { code: ING.TRUNC, body: buf, ms: Date.now() - t0 };
      return { code: 200, body: buf, ms: Date.now() - t0 };
    } catch (err) {
      // HTTPC_ERROR_READ_TIMEOUT (‑11) ou HTTPC_ERROR_CONNECTION_REFUSED (‑1), comme HTTPClient
      return { code: ac.signal.aborted ? -11 : -1, body: null, ms: Date.now() - t0, err: String(err.cause?.code || err.message || err) };
    } finally { clearTimeout(to); }
  }
  async httpGet(url) {
    let last = null, ms = 0;
    for (let a = 0; a < 3; a++) {
      last = await this.getOnce(url);
      ms += last.ms;
      if (last.code === 200 && last.body && last.body.length > 0) return { got: true, code: 200, body: last.body, ms };
      if (last.code === 200) last.code = ING.EMPTY;             // 200 sans corps
      this.log(`[net] ${url} → ${last.code}${last.err ? ' ' + last.err : ''} (tentative ${a + 1})`);
      await sleep(800 * (a + 1));
      ms += 800 * (a + 1);
    }
    return { got: false, code: last.code, body: last.body, ms };
  }

  // ── Cycle de récupération (fetchCycle) ──
  async fetchCycle() {
    const t0 = Date.now();
    // La carte date vehicle_types et station_information du DÉBUT du cycle (int64_t now = plat::now()
    // en tête de fetchCycle) ; station_status, lui, est appliqué à l'heure de sa réception.
    const cycleT = nowS();
    this.call({ k: 'fetching', on: true });
    let ok = true;
    this.clock();
    const typesAge = this.e.typesAge();
    if (typesAge < 0 || typesAge > this.cfg.typesRefreshS) {
      const r = await this.httpGet(`${this.cfg.gbfsBase}/vehicle_types.json`);
      this.call({ k: 'ingest', t: cycleT, feed: FEED.TYPES, got: r.got, code: r.code, body: r.got ? r.body.toString('utf8') : '', dur: r.ms, cyc: 0 });
    }
    this.clock();
    const infoAge = this.e.infoAge();
    if (infoAge < 0 || infoAge > this.cfg.infoRefreshS) {
      const r = await this.httpGet(`${this.cfg.gbfsBase}/station_information.json`);
      const n = this.call({ k: 'ingest', t: cycleT, feed: FEED.INFO, got: r.got, code: r.code, body: r.got ? r.body.toString('utf8') : '', dur: r.ms, cyc: 0 });
      if (!(n > 0)) ok = false;
    }
    this.clock();
    if (this.e.infoAge() >= 0) {
      const r = await this.httpGet(`${this.cfg.gbfsBase}/station_status.json`);
      const n = this.call({ k: 'ingest', feed: FEED.STATUS, got: r.got, code: r.code, body: r.got ? r.body.toString('utf8') : '', dur: r.ms, cyc: Date.now() - t0 });
      if (!(n > 0)) ok = false;
    }
    if (!ok) this.call({ k: 'cycle', ok: false });
    this.call({ k: 'fetching', on: false });
    this.cycles++;
    this.onCycle(ok);
    return ok;
  }

  async fetchWeather() {
    const r = await this.httpGet(this.cfg.wxUrl);
    const n = this.call({ k: 'ingest', feed: FEED.WEATHER, got: r.got, code: r.code, body: r.got ? r.body.toString('utf8') : '', dur: r.ms, cyc: 0 });
    return n >= 0;
  }

  // ── Boucle (task()) : un passage toutes les 200 ms ──
  async loop() {
    while (this.running) {
      try {
        const due = this.refreshReq || !this.lastFetch || this.mono() - this.lastFetch >= this.cfg.refreshS * 1000;
        if (due) {
          this.refreshReq = false;
          this.lastFetch = this.mono();
          this.busy = true;
          await this.fetchCycle();
          this.busy = false;
        }
        if (!this.wxAt || nowS() - this.wxAt >= this.cfg.wxRefreshS) {
          this.wxAt = nowS();
          this.busy = true;
          if (!(await this.fetchWeather())) this.wxAt -= this.cfg.wxRefreshS - 120;   // nouvel essai dans 2 min
          this.busy = false;
        }
      } catch (err) {
        this.busy = false;
        this.log(`[net] erreur inattendue : ${err.stack || err}`);
      }
      await sleep(200);
    }
  }
  start() { if (!this.running) { this.running = true; this.loop(); } }
  stop() { this.running = false; }
}
