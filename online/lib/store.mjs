// ── Persistance sur disque et archive pour le banc ───────────────────
// Les fichiers sont ceux de la carte (/vn_prof.bin, /vn_hist.bin, /vn_days.bin — octets exacts de
// Model::serialize) plus /vn_live.bin, propre au moteur en ligne. Écriture atomique (fichier
// temporaire puis renommage) : une coupure pendant l'écriture laisse l'ancienne version intacte.
import { mkdirSync, writeFileSync, renameSync, readFileSync, existsSync, readdirSync, rmSync, statSync } from 'node:fs';
import { join } from 'node:path';

export const STATE_FILES = ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin', 'vn_live.bin'];
export const CARD_FILES = ['vn_prof.bin', 'vn_hist.bin', 'vn_days.bin'];

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
      const b = this.read(n);
      if (b && b.length) { engine.putFile('/' + n, b); got.push(n); }
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
