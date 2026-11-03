// ── Synchronisation : instantanés pour les Sunton et les jumeaux ─────
// Un instantané = les fichiers de la carte sérialisés à un instant (Model::serialize), avec leur
// CRC32 (zlib) et ce qu'il faut pour décider sans les télécharger : relevés appris (profils),
// date de sauvegarde (historique 24 h), jours résumés. La carte télécharge le manifeste, compare à
// ses propres en-têtes, puis ne télécharge que ce qui est meilleur que ce qu'elle a (src/core/sync).
import { readFileSync, existsSync } from 'node:fs';
import { atomicWrite } from './store.mjs';

const hex8 = (n) => (n >>> 0).toString(16).toUpperCase().padStart(8, '0');
// Registre des écrans : borné (une rafale de X-VN-Device inventés le faisait grossir sans fin, 1,45 Mo pour
// 3000 valeurs, réécrit à chaque requête). Le plus ancien laisse sa place ; un écran réel revient à sa synchro.
export const MAX_DEVICES = 50;
const FIELD_MAX = 200, DEVICES_SAVE_MS = 5000;

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

// ── Plausibilité d'un fichier envoyé par une carte ──
// L'adoption retient le profil qui a le plus de relevés : un compteur forgé (« 4 000 000 000 relevés »,
// charge à zéro) serait adopté, propagé à toutes les cartes, et bloquerait ensuite tout envoi légitime.
// Ce que src/core/model.cpp (Model::applyStatus) garantit pour un profil réellement appris : chaque tranche
// de 5 min apprise ajoute 1 à learnSamples ET à cellN[cellule] (plafonné à 255), et 1 au n (plafonné) de chaque
// station relevée dans cette cellule. D'où : learnSamples ≤ tranches écoulées entre learnSince et savedAt,
// Σ cellN = learnSamples (tant qu'aucune cellule n'est saturée), n ≤ cellN, et des cellules apprises
// renseignées pour presque toutes les stations (98 % sur les 13 profils de production du 26/10 au 01/11/2026).
// Ces règles lisent la disposition des versions 1 à 5 de /vn_prof.bin (Model::profFileOk ; la version 5, modèle 2
// de feat/modele-v2 en 1.4.0, ajoute FcLearn en fin de fichier : en-tête, cellules et cellN restent en place) : une
// autre version est refusée tant que ce contrôle ne la connaît pas.
const PROF_MAGIC = 0x31504E56, DAYQ = 672;
export const LEARN_EPOCH = Date.UTC(2026, 9, 1) / 1000;    // VelohNav Ops 1.0.0 date du 13/10/2026 : aucune carte n'apprenait avant
const iso = (t) => new Date(t * 1000).toISOString().slice(0, 16).replace('T', ' ');
export function profImplausible(b, now) {
  const h = profHeader(b);
  if (!h || h.magic !== PROF_MAGIC) return 'en-tête illisible';
  if (h.ver < 1 || h.ver > 5) return `version ${h.ver} inconnue du contrôle de plausibilité`;
  const cell = h.ver >= 3 ? 4 : 3, nMask = h.ver >= 3 ? 0x1f : 0xff;       // ProfCell : n sur 5 bits depuis la v3
  const cellsAt = 40 + h.nSt * 8, cellNAt = cellsAt + h.nSt * DAYQ * cell;
  if (b.length < cellNAt + DAYQ) return 'fichier trop court';
  if (h.savedAt > now + 600) return `enregistré dans le futur (${iso(h.savedAt)} UTC)`;
  if (!h.learnSamples) return null;
  if (h.learnSince < LEARN_EPOCH || h.learnSince > h.savedAt) return `début d'apprentissage impossible (${iso(h.learnSince)} UTC)`;
  // une tranche au plus par 5 min, plus 1 h de recul d'horloge toléré (resynchronisation NTP)
  const slots = Math.floor((h.savedAt - h.learnSince) / 300) + 2 + 12;
  if (h.learnSamples > slots) return `${h.learnSamples} relevés pour ${slots} tranches de 5 min écoulées depuis ${iso(h.learnSince)} UTC`;
  let sum = 0, saturated = false, pairs = 0, learned = 0, over = 0;
  for (let q = 0; q < DAYQ; q++) {
    const c = b[cellNAt + q];
    sum += c;
    if (c === 255) saturated = true;
    if (!c) continue;
    for (let j = 0; j < h.nSt; j++) {
      const n = b[cellsAt + (j * DAYQ + q) * cell + 2] & nMask;
      pairs++;
      if (n) learned++;
      if (n > c) over++;
    }
  }
  if (saturated ? sum > h.learnSamples : sum !== h.learnSamples) return `compteur (${h.learnSamples}) incohérent avec les cellules du profil (${sum})`;
  if (over) return `${over} cellule(s) de station apprise(s) plus souvent que le parc`;
  if (pairs && learned < pairs / 2) return `profil vide : ${(100 * learned / pairs).toFixed(0)} % des cellules apprises renseignées`;
  return null;
}
// Résumés par jour : la carte reprend ceux du serveur s'ils sont « plus récents » — une date future gagnerait toujours
export function daysImplausible(b, now) {
  const h = daysHeader(b);
  if (!h) return 'en-tête illisible';
  if (h.savedAt > now + 600) return `enregistré dans le futur (${iso(h.savedAt)} UTC)`;
  return null;
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
    let devices = {};
    try { if (existsSync(this.devicesPath)) devices = JSON.parse(readFileSync(this.devicesPath, 'utf8')); } catch (err) { this.log(`[sync] registre illisible : ${err.message}`); }
    // un registre d'avant la borne est ramené aux 50 plus récents ; touch : ordre des passages (en mémoire), le
    // plus ancien laisse sa place (last, à la seconde, départage mal une rafale)
    const list = Object.values(devices).filter((d) => d && d.id).sort((a, b) => (b.last || 0) - (a.last || 0)).slice(0, MAX_DEVICES);
    this.devices = Object.fromEntries(list.map((d) => [d.id, d]));
    this.touch = new Map(list.map((d, i) => [d.id, list.length - i]));
    this.tick = list.length;
    this.devTimer = null;
    this.devDirty = false;
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
    let d = this.devices[id];
    if (!d) {
      const ids = Object.keys(this.devices);
      if (ids.length >= MAX_DEVICES) {
        const old = ids.reduce((a, b) => ((this.touch.get(a) ?? 0) <= (this.touch.get(b) ?? 0) ? a : b));
        delete this.devices[old];
        this.touch.delete(old);
      }
      d = { id, first: Math.floor(Date.now() / 1000) };
    }
    d.last = Math.floor(Date.now() / 1000);
    d.ip = req.socket.remoteAddress;
    d.fw = String(req.headers['x-vn-fw'] || d.fw || '').slice(0, 24);
    if (req.headers['x-vn-local']) d.local = String(req.headers['x-vn-local']).slice(0, 160);
    for (const [k, v] of Object.entries(extra || {})) d[k] = typeof v === 'string' ? v.slice(0, FIELD_MAX) : v;
    this.devices[id] = d;
    this.touch.set(id, ++this.tick);
    this.saveDevices();
    return d;
  }
  // Écriture différée : au plus une toutes les 5 s (et à l'arrêt : flushDevices), plus une par requête
  saveDevices() {
    this.devDirty = true;
    if (this.devTimer) return;
    this.devTimer = setTimeout(() => this.flushDevices(), DEVICES_SAVE_MS);
    this.devTimer.unref?.();
  }
  flushDevices() {
    if (this.devTimer) { clearTimeout(this.devTimer); this.devTimer = null; }
    if (!this.devDirty) return;
    this.devDirty = false;
    try { atomicWrite(this.devicesPath, JSON.stringify(this.devices, null, 2)); } catch (err) { this.log(`[sync] registre : ${err.message}`); }
  }
}
