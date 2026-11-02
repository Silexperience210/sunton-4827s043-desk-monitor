// ── Persistance sur disque et archive pour le banc ───────────────────
// Les fichiers sont ceux de la carte (/vn_prof.bin, /vn_hist.bin, /vn_days.bin — octets exacts de
// Model::serialize) plus /vn_live.bin, propre au moteur en ligne. Écriture atomique et durable (fichier
// temporaire écrit puis fsync, renommage, fsync du dossier) : une coupure de courant laisse l'ancienne
// version ou la nouvelle, entières — jamais un fichier renommé mais vide.
import { mkdirSync, writeFileSync, renameSync, readFileSync, existsSync, readdirSync, rmSync, statSync, linkSync, copyFileSync,
  openSync, closeSync, fsyncSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { randomBytes } from 'node:crypto';

export const STATE_FILES = ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin', 'vn_live.bin'];
export const CARD_FILES = ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin'];

// ── /vn_live.bin : index contrôlés avant la relecture ──
// liveLoad (online/engine/engine.cpp) vérifie l'en-tête et la taille, pas les index rHead, rCount, nLog,
// logHead ni la station de chaque entrée du journal : un fichier de bonne taille mais abîmé faisait lire et
// écrire hors des tableaux du modèle à chaque relevé (« memory access out of bounds » dans applyStatus), et
// la corruption était resauvegardée à l'arrêt. Disposition LiveHdr version 1 (96 octets) : rHead, rCount,
// nLog, logHead en int32 aux octets 68, 72, 76, 80 ; puis stations, recT[60], rec[60][n], journal[1024].
// Renvoie { buf, why } : why = raison du refus (le fichier ne doit pas être relu), buf = le fichier, journal
// ramené dans les bornes (station hors [0, n) → -1). Une autre version est laissée à liveLoad, qui la refuse.
const LIVE_MAGIC = 0x314C4E56, LIVE_HDR = 96, RECENT_SLOTS = 60, ALERT_LOG = 1024, LOG_ENTRY = 16;
export function checkLive(b) {
  if (!b || b.length < LIVE_HDR) return { buf: b, why: 'en-tête tronqué' };
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  if (v.getUint32(0, true) !== LIVE_MAGIC || v.getUint16(4, true) !== 1) return { buf: b, why: null };
  const nSt = v.getUint16(6, true), szStation = v.getUint16(8, true), szLog = v.getUint16(16, true);
  const rHead = v.getInt32(68, true), rCount = v.getInt32(72, true), nLog = v.getInt32(76, true), logHead = v.getInt32(80, true);
  if (rHead < -1 || rHead >= RECENT_SLOTS) return { buf: b, why: `rHead ${rHead} hors [-1, ${RECENT_SLOTS})` };
  if (rCount < 0 || rCount > RECENT_SLOTS || (rHead < 0) !== (rCount === 0)) return { buf: b, why: `rCount ${rCount} incohérent (rHead ${rHead})` };
  if (logHead < -1 || logHead >= ALERT_LOG) return { buf: b, why: `logHead ${logHead} hors [-1, ${ALERT_LOG})` };
  if (nLog < 0 || nLog > ALERT_LOG || (logHead < 0) !== (nLog === 0)) return { buf: b, why: `nLog ${nLog} incohérent (logHead ${logHead})` };
  const logAt = LIVE_HDR + szStation * nSt + 8 * RECENT_SLOTS + RECENT_SLOTS * nSt;
  if (szLog !== LOG_ENTRY || b.length < logAt + ALERT_LOG * LOG_ENTRY) return { buf: b, why: null };   // taille : liveLoad
  let out = b;
  for (let k = 0; k < ALERT_LOG; k++) {
    const at = logAt + k * LOG_ENTRY + 12, st = v.getInt16(at, true);
    if (st === -1 || (st >= 0 && st < nSt)) continue;
    if (out === b) out = Buffer.from(b);
    out.writeInt16LE(-1, at);
  }
  return { buf: out, why: null };
}

// Un fichier d'état relu SEUL dans un moteur neuf, à l'heure `now` (la règle des 20 h de l'historique 24 h en
// dépend) : { ok, buf, why, stale }. stale = historique 24 h trop ancien, pas abîmé (règle de la carte).
const HIST_MAGIC = 0x31484E56, HIST_FRESH_S = 20 * 3600;
export async function readable(name, b, makeEngine, now) {
  if (!b.length) return { ok: false, why: 'fichier vide' };
  if (name === 'vn_live.bin') {
    const c = checkLive(b);
    if (c.why) return { ok: false, why: c.why };
    b = c.buf;
  }
  if (name === 'vn_hist.bin' && b.length >= 40 && b.readUInt32LE(0) === HIST_MAGIC) {
    const saved = Number(b.readBigInt64LE(32));
    if (saved > 0 && now - saved >= HIST_FRESH_S) return { ok: false, stale: true, why: `sauvegardé il y a ${Math.floor((now - saved) / 3600)} h (au-delà de 20 h, comme la carte)` };
  }
  const live = name === 'vn_live.bin';
  const e = await makeEngine();
  e.clock(now, 1000);
  e.init();
  e.putFile('/' + name, b);
  const r = e.load(live);
  if (live ? r & 2 : r & 1) return { ok: true, buf: b };
  return { ok: false, why: `illisible par ${live ? 'liveLoad' : 'Model::load'}, ${b.length} octets` };
}

// Sans fsync, le renommage peut atteindre le disque avant les données : après une coupure (Umbrel, Raspberry
// Pi), un fichier renommé mais vide, refusé au démarrage. Nom temporaire unique : sous Docker le PID vaut 1.
export function atomicWrite(path, data) {
  const tmp = `${path}.tmp-${process.pid}-${randomBytes(4).toString('hex')}`;
  try {
    durableWrite(tmp, data);
    renameSync(tmp, path);
  } catch (err) { rmSync(tmp, { force: true }); throw err; }
  syncDir(dirname(path));
}
function durableWrite(path, data) {
  const fd = openSync(path, 'w');
  try { writeFileSync(fd, data); fsyncSync(fd); } finally { closeSync(fd); }
}
// Le renommage lui-même n'est durable qu'une fois le dossier écrit (sans effet là où un dossier ne s'ouvre pas)
function syncDir(dir) {
  let fd = null;
  try { fd = openSync(dir, 'r'); fsyncSync(fd); } catch { /* Windows */ } finally { if (fd !== null) closeSync(fd); }
}
// Temporaires d'une écriture interrompue (arrêt brutal) : .tmp-* dans les dossiers de données, .instantane-*
// dans l'archive. Plus de 10 min seulement : un autre processus sur le même volume peut être en train d'écrire.
const ORPHAN = /\.tmp-\d+(-[0-9a-f]{8})?$/;
export function removeOrphans(root, { ageS = 600, depth = 3 } = {}) {
  const limit = Date.now() - ageS * 1000;
  let n = 0;
  const walk = (d, k) => {
    let names = [];
    try { names = readdirSync(d, { withFileTypes: true }); } catch { return; }
    for (const x of names) {
      const p = join(d, x.name);
      const orphan = x.isDirectory() ? x.name.startsWith('.instantane-') : ORPHAN.test(x.name);
      if (orphan) {
        try { if (statSync(p).mtimeMs < limit) { rmSync(p, { recursive: true, force: true }); n++; } } catch { /* déjà parti */ }
      } else if (x.isDirectory() && k < depth) walk(p, k + 1);
    }
  };
  walk(root, 0);
  return n;
}

// Heure locale de Luxembourg (la carte et le banc datent tout en heure de Paris/Luxembourg) : public/vn-series.js
import { luxParts } from '../public/vn-series.js';
export { luxParts };

export class Store {
  constructor(dir, log) {
    this.dir = dir;
    this.log = log;
    mkdirSync(dir, { recursive: true });
    this.archiveDir = join(dir, 'archive');
  }
  path(name) { return join(this.dir, name); }
  has(name) { return existsSync(this.path(name)); }
  read(name) { try { return readFileSync(this.path(name)); } catch { return null; } }
  write(name, data) { atomicWrite(this.path(name), data); }

  // ── Reprise de l'état au démarrage ──
  // Chaque fichier est d'abord relu SEUL dans un moteur neuf (Model::load, liveLoad : les contrôles de la carte),
  // pour savoir lequel est lisible : la première sauvegarde réécrit tout, un fichier refusé serait perdu en
  // silence. Non vide et refusé, il est mis de côté (<nom>.rejete-AAAAMMJJ-HHMMSS) et remplacé par la génération
  // précédente (<nom>.prev, gardée à chaque sauvegarde) si elle est lisible. Le journal nomme chaque fichier.
  // Un historique 24 h de plus de 20 h n'est pas une erreur (règle de la carte) : non repris, laissé en place.
  // Dépose dans le moteur ce qui est lisible ; renvoie { nom: { state: oui | prev | non | absent, why } }.
  async restore(engine, { makeEngine, now }) {
    const got = {};
    for (const n of STATE_FILES) {
      const r = await this._pick(n, makeEngine, now);
      got[n] = r;
      if (r.buf) engine.putFile('/' + n, r.buf);
    }
    return got;
  }
  async _pick(n, makeEngine, now) {
    const main = this.read(n);
    if (!main) return { state: 'absent' };
    const c = await readable(n, main, makeEngine, now);
    if (c.ok) return { state: 'oui', buf: c.buf };
    if (c.stale) { this.log(`[fs] ${n} non repris : ${c.why}`); return { state: 'non', why: c.why }; }
    if (main.length) this.log(`[fs] ${n} refusé (${c.why}) : gardé sous ${this.reject(n)}`);
    else { rmSync(this.path(n), { force: true }); this.log(`[fs] ${n} vide : ignoré`); }
    const prev = this.read(`${n}.prev`);
    if (prev && prev.length) {
      const p = await readable(n, prev, makeEngine, now);
      if (p.ok) { this.log(`[fs] ${n} repris de ${n}.prev (génération précédente)`); return { state: 'prev', buf: p.buf, why: c.why }; }
      if (!p.stale) this.log(`[fs] ${n}.prev refusé aussi (${p.why}) : gardé sous ${this.reject(`${n}.prev`)}`);
    }
    return { state: 'non', why: c.why };
  }
  // Met un fichier refusé de côté (jamais écrasé) ; renvoie son nouveau nom
  reject(name) {
    const { stamp } = luxParts(Math.floor(Date.now() / 1000));
    let to = `${name}.rejete-${stamp}`, k = 2;
    while (existsSync(this.path(to))) to = `${name}.rejete-${stamp}-${k++}`;
    renameSync(this.path(name), this.path(to));
    return to;
  }
  // Écrit ce que le moteur vient de sérialiser (vn_save) ; la version remplacée devient <nom>.prev
  persistFrom(engine) {
    let bytes = 0;
    for (const n of STATE_FILES) {
      const b = engine.getFile('/' + n);
      if (b) { this.keepPrevious(n); this.write(n, b); bytes += b.length; }
    }
    return bytes;
  }
  // Lien dur (aucune copie, le fichier courant reste en place), copie si le système de fichiers les refuse
  keepPrevious(n) {
    const p = this.path(n), prev = `${p}.prev`;
    if (!existsSync(p)) return;
    try { rmSync(prev, { force: true }); linkSync(p, prev); } catch {
      try { copyFileSync(p, prev); } catch (err) { this.log(`[fs] ${n}.prev non gardé : ${err.message}`); }
    }
  }

  // ── Archive (même disposition que tools/carte_dump.py) ──
  archive(engine, files, meta) {
    mkdirSync(this.archiveDir, { recursive: true });
    const { stamp } = luxParts(meta.t);
    let final = join(this.archiveDir, stamp), n = 2;
    while (existsSync(final)) final = join(this.archiveDir, `${stamp}-${n++}`);
    const tmp = join(this.archiveDir, `.instantane-${process.pid}-${Date.now()}`);
    mkdirSync(tmp);
    const fich = {};
    for (const [name, data] of Object.entries(files)) {
      durableWrite(join(tmp, name), data);
      fich[name] = { taille: data.length, crc32: engine.crc32(data).toString(16).toUpperCase().padStart(8, '0') };
    }
    durableWrite(join(tmp, 'manifeste.json'), JSON.stringify({
      source: 'serveur en ligne', version_carte: meta.fw, t_carte: meta.t, redemarrage_vu: false, fichiers: fich,
    }, null, 2));
    syncDir(tmp);
    renameSync(tmp, final);
    syncDir(this.archiveDir);
    return final;
  }
  archives() {
    try { return readdirSync(this.archiveDir).filter((d) => /^\d{8}-\d{6}/.test(d)).sort(); } catch { return []; }
  }
  pruneArchives(keepDays) {
    if (!keepDays) return 0;
    const limit = Date.now() - keepDays * 86400e3;
    let n = 0;
    for (const d of this.archives()) {
      const p = join(this.archiveDir, d);
      try { if (statSync(p).mtimeMs < limit) { rmSync(p, { recursive: true, force: true }); n++; } } catch { /* déjà parti */ }
    }
    return n;
  }
}
