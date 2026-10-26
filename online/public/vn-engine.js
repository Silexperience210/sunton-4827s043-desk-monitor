// ── Moteur VelohNav Ops (WebAssembly) : enveloppe JavaScript ─────────
// Même module pour le serveur (Node ≥ 18) et le navigateur : aucune dépendance.
// Le module est le cœur du firmware compilé tel quel (online/engine/build.sh) ; cette enveloppe ne
// fait que passer les octets et les chaînes, elle ne calcule rien.

export const FEED = { STATUS: 0, INFO: 1, TYPES: 2, WEATHER: 3 };
export const FEED_NAMES = ['station_status', 'station_information', 'vehicle_types', 'météo'];
// Codes d'échec propres à VelohNav Ops (src/core/ops.h)
export const ING = { PARSE: -100, EMPTY: -101, TRUNC: -102, BEGIN: -103 };
// État Wi-Fi de la carte (src/net/net.h)
export const WIFI = { NONE: 0, CONNECTING: 1, CONNECTED: 2, FAILED: 3 };

const enc = new TextEncoder();
const dec = new TextDecoder();

// Les trois appels système que la bibliothèque C peut faire (abort, printf) : rien d'autre n'est importé
function wasiShim(getMemory, onLog) {
  let pending = '';
  return {
    fd_write(fd, iovs, iovsLen, nwritten) {
      const mem = getMemory();
      const view = new DataView(mem.buffer);
      let total = 0;
      for (let i = 0; i < iovsLen; i++) {
        const p = view.getUint32(iovs + i * 8, true), n = view.getUint32(iovs + i * 8 + 4, true);
        pending += dec.decode(new Uint8Array(mem.buffer, p, n));
        total += n;
      }
      let k;
      while ((k = pending.indexOf('\n')) >= 0) { onLog(pending.slice(0, k), fd); pending = pending.slice(k + 1); }
      view.setUint32(nwritten, total, true);
      return 0;
    },
    fd_close() { return 0; },
    fd_seek() { return 70; },        // ESPIPE : aucun fichier réel
  };
}

export class Engine {
  constructor(instance) {
    this.x = instance.exports;
    this.ui = typeof this.x.vn_ui_init === 'function';
  }
  get mem() { return this.x.memory.buffer; }

  // ── chaînes et octets ──
  _bytes(data) {
    const b = typeof data === 'string' ? enc.encode(data) : data;
    const p = this.x.vn_alloc(b.length + 1);
    const u = new Uint8Array(this.mem, p, b.length + 1);
    u.set(b); u[b.length] = 0;
    return [p, b.length];
  }
  _with(data, fn) {
    const [p, n] = this._bytes(data);
    try { return fn(p, n); } finally { this.x.vn_free(p); }
  }
  _outBytes() { return new Uint8Array(this.mem, this.x.vn_out_ptr(), this.x.vn_out_len()).slice(); }
  _outText() { return dec.decode(new Uint8Array(this.mem, this.x.vn_out_ptr(), this.x.vn_out_len())); }

  // ── horloge : epoch UTC en secondes, horloge monotone en ms ──
  clock(epochS, ms) { this.x.vn_clock(Math.floor(epochS), ms); }

  init() { return this.x.vn_init() === 1; }
  version() { const p = this.x.vn_version(); const u = new Uint8Array(this.mem, p, 32); return dec.decode(u.slice(0, u.indexOf(0))); }
  logs() { this.x.vn_log_take(); return this._outText(); }

  putFile(path, data) { return this._with(path, (pp) => this._with(data, (dp, n) => this.x.vn_fs_put(pp, dp, n))) === 1; }
  getFile(path) { return this._with(path, (pp) => this.x.vn_fs_get(pp)) < 0 ? null : this._outBytes(); }
  delFile(path) { return this._with(path, (pp) => this.x.vn_fs_del(pp)) === 1; }

  // Un flux reçu (got = corps non vide en 200) ou en échec (code). Renvoie le nombre d'enregistrements, ‑1 = échec.
  ingest(feed, got, code, body, ms, cycleMs = ms) {
    return this._with(body || '', (p, n) => this.x.vn_ingest(feed, got ? 1 : 0, code | 0, p, got ? n : (body ? n : 0), ms >>> 0, cycleMs >>> 0));
  }
  cycleEnd(ok) { this.x.vn_cycle_end(ok ? 1 : 0); }
  setFetching(on) { this.x.vn_set_fetching(on ? 1 : 0); }
  infoAge() { return this.x.vn_info_age(); }
  typesAge() { return this.x.vn_types_age(); }
  recompute() { this.x.vn_recompute(); }
  settings(s) {
    const v = (k) => (s[k] === undefined || s[k] === null ? -1 : s[k] | 0);
    this.x.vn_settings_set(v('refreshSec'), v('lowBikes'), v('lowDocks'), v('maintBikes'), v('maintDocks'), v('frozenHours'), v('staleMin'));
  }

  save() { return this.x.vn_save() === 1; }
  // 1 = fichiers de la carte relus, 2 = état vivant relu
  load(withLive = true) { return this.x.vn_load(withLive ? 1 : 0); }
  stationsCsv() { this.x.vn_stations_csv(); return this._outText(); }
  crc32(bytes) { return this._with(bytes, (p, n) => this.x.vn_crc32(p, n)) >>> 0; }
  summary() { this.x.vn_summary_json(); return JSON.parse(this._outText()); }
  ring() { this.x.vn_ring_json(); return JSON.parse(this._outText()); }
  // ── historique long, prévisions, notifications ──
  ringHead() { return this.x.vn_ring_head(); }              // début de la tranche 5 min la plus récente (0 = aucune)
  slot(back) { this.x.vn_slot_json(back | 0); return JSON.parse(this._outText()); }
  stations() { this.x.vn_stations_json(); return JSON.parse(this._outText()); }
  forecast() { this.x.vn_forecast_json(); return JSON.parse(this._outText()); }
  alerts() { this.x.vn_alerts_json(); return JSON.parse(this._outText()); }
  days() { this.x.vn_days_json(); return JSON.parse(this._outText()); }
  daysStation(i) { this.x.vn_days_station_json(i | 0); return JSON.parse(this._outText()); }
  digest(day) { this.x.vn_digest_json(day | 0); return JSON.parse(this._outText()); }
  localDay(t) { return this.x.vn_local_day(t); }
  curves() { this.x.vn_curves_json(); return JSON.parse(this._outText()); }   // courbes prévues 0–240 min par station

  // ── jumeau d'écran ──
  uiInit() { return this.x.vn_ui_init() === 1; }
  uiTick() { return this.x.vn_ui_tick(); }
  uiTouch(x, y, pressed) { this.x.vn_ui_touch(x | 0, y | 0, pressed ? 1 : 0); }
  fb() { return new Uint16Array(this.mem, this.x.vn_ui_fb(), 480 * 272); }
  dirty() { return Array.from(new Int32Array(this.mem, this.x.vn_ui_dirty(), 4)); }
  uiNet(wifi, rssi, name, stage) {
    this._with(name || '', (np) => this._with(stage || '', (sp) => this.x.vn_ui_net(wifi, rssi | 0, np, sp)));
  }
  uiRequests() { return this.x.vn_ui_requests(); }
  // Réglages → Synchro en ligne du jumeau : adresse du serveur et dernière reprise d'état
  uiSync(url, state, at, text) {
    this._with(url || '', (up) => this._with(text || '', (tp) => this.x.vn_ui_sync(up, state | 0, at || 0, tp)));
  }
  saveSettings() { return this.x.vn_ui_settings_save() === 1; }
}

// bytes : ArrayBuffer / Uint8Array du module ; onLog(texte) : journal de la bibliothèque C
export async function loadEngine(bytes, { onLog = (l) => console.log('[vn]', l) } = {}) {
  let memory = null;
  const { instance } = await WebAssembly.instantiate(bytes, {
    wasi_snapshot_preview1: wasiShim(() => memory, onLog),
  });
  memory = instance.exports.memory;
  instance.exports._initialize();
  return new Engine(instance);
}
