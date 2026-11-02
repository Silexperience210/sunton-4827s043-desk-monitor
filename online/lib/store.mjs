// ── Persistance sur disque et archive pour le banc ───────────────────
// Les fichiers sont ceux de la carte (/vn_prof.bin, /vn_hist.bin, /vn_days.bin — octets exacts de
// Model::serialize) plus /vn_live.bin, propre au moteur en ligne. Écriture atomique (fichier
// temporaire puis renommage) : une coupure pendant l'écriture laisse l'ancienne version intacte.
import { mkdirSync, writeFileSync, renameSync, readFileSync, existsSync, readdirSync, rmSync, statSync } from 'node:fs';
import { join } from 'node:path';

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

export function atomicWrite(path, data) {
  const tmp = `${path}.tmp-${process.pid}`;
  writeFileSync(tmp, data);
  renameSync(tmp, path);
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

  // Dépose dans le moteur ce qui existe sur disque ; renvoie les noms relus
  restoreInto(engine, names = STATE_FILES) {
    const got = [];
    for (const n of names) {
      let b = this.read(n);
      if (!b || !b.length) continue;
      if (n === 'vn_live.bin') {
        const c = checkLive(b);
        if (c.why) { this.log(`[fs] vn_live.bin refusé (${c.why}) : état vivant repris de zéro`); continue; }
        b = c.buf;
      }
      engine.putFile('/' + n, b);
      got.push(n);
    }
    return got;
  }
  // Écrit ce que le moteur vient de sérialiser (vn_save)
  persistFrom(engine) {
    let bytes = 0;
    for (const n of STATE_FILES) {
      const b = engine.getFile('/' + n);
      if (b) { this.write(n, b); bytes += b.length; }
    }
    return bytes;
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
      writeFileSync(join(tmp, name), data);
      fich[name] = { taille: data.length, crc32: engine.crc32(data).toString(16).toUpperCase().padStart(8, '0') };
    }
    writeFileSync(join(tmp, 'manifeste.json'), JSON.stringify({
      source: 'serveur en ligne', version_carte: meta.fw, t_carte: meta.t, redemarrage_vu: false, fichiers: fich,
    }, null, 2));
    renameSync(tmp, final);
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
