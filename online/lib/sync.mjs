// ── Synchronisation : instantanés pour les Sunton et les jumeaux ─────
// Un instantané = les fichiers de la carte sérialisés à un instant (Model::serialize), avec leur
// CRC32 (zlib) et ce qu'il faut pour décider sans les télécharger : relevés appris (profils),
// date de sauvegarde (historique 24 h), jours résumés. La carte télécharge le manifeste, compare à
// ses propres en-têtes, puis ne télécharge que ce qui est meilleur que ce qu'elle a (src/core/sync).
import { readFileSync, existsSync } from 'node:fs';
import { atomicWrite } from './store.mjs';

const hex8 = (n) => (n >>> 0).toString(16).toUpperCase().padStart(8, '0');

// En-têtes des fichiers de la carte (src/core/model.cpp FileHdr, src/core/days.h DaysFileHdr)
export function profHeader(b) {
  if (!b || b.length < 40) return null;
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  return { magic: v.getUint32(0, true), ver: v.getUint16(4, true), nSt: v.getUint16(6, true),
    learnSince: Number(v.getBigInt64(16, true)), learnSamples: v.getUint32(24, true), savedAt: Number(v.getBigInt64(32, true)) };
}
export function daysHeader(b) {
  if (!b || b.length < 40) return null;
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  return { magic: v.getUint32(0, true), ver: v.getUint16(4, true), nSt: v.getUint16(6, true), nDays: v.getUint16(8, true),
    savedAt: Number(v.getBigInt64(32, true)) };
}

export class SyncHub {
  constructor({ engine, store, events, cfg, log, beforeSnapshot }) {
    this.e = engine;
    this.store = store;
    this.events = events;
    this.cfg = cfg;
    this.log = log;
    this.beforeSnapshot = beforeSnapshot || (() => {});
    this.snaps = [];                     // les trois derniers instantanés
    this.devicesPath = store.path('devices.json');
    this.devices = existsSync(this.devicesPath) ? JSON.parse(readFileSync(this.devicesPath, 'utf8')) : {};
  }

  // Instantané récent (≤ maxAgeS) ou nouveau ; null si le modèle est encore vide
  snapshot(maxAgeS = 60) {
    const now = Math.floor(Date.now() / 1000);
    const last = this.snaps[this.snaps.length - 1];
    if (last && now - last.t <= maxAgeS) return last;
    this.beforeSnapshot();
    if (!this.e.save()) return null;
    const files = {};
    for (const n of ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin', 'vn_live.bin']) {
      const b = this.e.getFile('/' + n);
      if (b) files[n] = Buffer.from(b);
    }
    if (!files['vn_prof.bin'] || !files['vn_hist.bin']) return null;
    const s = { id: `${now.toString(36)}${(this.events.seq % 1296).toString(36).padStart(2, '0')}`, t: now, seq: this.events.seq, files, crc: {} };
    for (const [n, b] of Object.entries(files)) s.crc[n] = this.e.crc32(b);
    this.snaps.push(s);
    if (this.snaps.length > 3) this.snaps.shift();
    return s;
  }
  byId(id) { return this.snaps.find((s) => s.id === id) || null; }
  // Après un rechargement de l'état : plus aucun instantané en cache ne décrit le serveur
  invalidate() { this.snaps.length = 0; }

  // Manifeste lu par la carte (ArduinoJson) : court, sans imbrication inutile
  manifest(s, base) {
    const p = profHeader(s.files['vn_prof.bin']);
    const h = profHeader(s.files['vn_hist.bin']);         // même en-tête FileHdr
    const d = daysHeader(s.files['vn_days.bin']);
    const sum = this.e.summary();
    return {
      proto: 1, fw: this.e.version(), name: this.cfg.name, id: s.id, t: s.t, base: `${base}/sync/${s.id}/`,
      prof: { size: s.files['vn_prof.bin'].length, crc: hex8(s.crc['vn_prof.bin']), ver: p.ver, samples: p.learnSamples, since: p.learnSince },
      hist: { size: s.files['vn_hist.bin'].length, crc: hex8(s.crc['vn_hist.bin']), ver: h.ver, savedAt: h.savedAt, minutes: sum.histMinutes },
      days: s.files['vn_days.bin']
        ? { size: s.files['vn_days.bin'].length, crc: hex8(s.crc['vn_days.bin']), ver: d.ver, n: d.nDays, savedAt: d.savedAt }
        : null,
    };
  }

  // ── Registre des écrans synchronisés ──
  seen(req, extra) {
    const id = String(req.headers['x-vn-device'] || '').slice(0, 40) || `ip-${req.socket.remoteAddress}`;
    const d = this.devices[id] || { id, first: Math.floor(Date.now() / 1000) };
    d.last = Math.floor(Date.now() / 1000);
    d.ip = req.socket.remoteAddress;
    d.fw = String(req.headers['x-vn-fw'] || d.fw || '').slice(0, 24);
    if (req.headers['x-vn-local']) d.local = String(req.headers['x-vn-local']).slice(0, 160);
    Object.assign(d, extra || {});
    this.devices[id] = d;
    this.saveDevices();
    return d;
  }
  saveDevices() {
    try { atomicWrite(this.devicesPath, JSON.stringify(this.devices, null, 2)); } catch (err) { this.log(`[sync] registre : ${err.message}`); }
  }
}
