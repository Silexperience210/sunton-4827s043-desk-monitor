// ── Banc de nuit : sim/bench_prevision --reel sur l'archive du serveur ─
// Le banc natif rejoue l'archive (instantanés de 6 h et 18 h) avec le cœur du firmware et publie l'erreur
// de la prévision, de la persistance et du profil seul, jour par jour, avec ses verdicts appariés
// (docs/mesures-prevision.md). Complète la mesure en continu (lib/forecast.mjs) : celle-ci note ce que la
// carte a VRAIMENT affiché ; le banc mesure la chaîne sur toute l'archive, de façon reproductible.
//
// Activé si VN_BENCH_BIN désigne l'exécutable (l'image Docker le compile et le règle). Rapport complet
// en Markdown et extrait JSON dans <données>/banc/AAAA-MM-JJ.{md,json}.
import { spawn } from 'node:child_process';
import { mkdirSync, readdirSync, readFileSync, writeFileSync, existsSync, rmSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { atomicWrite } from './store.mjs';
import { snapshotDirs } from './history.mjs';

const num = (s) => { const v = parseFloat(String(s).replace(',', '.')); return Number.isFinite(v) ? v : null; };

// Extrait ce que la page affiche du rapport Markdown du banc
export function parseBenchReport(md) {
  const out = { rows: [], perDay: [], verdicts: [], evalDays: null, span: null, leak: /\*\*FUITE\*\*/.test(md), empty: /AUCUNE donnée exploitable/.test(md) };
  const sec1 = md.split('## 1.')[1]?.split('## 2.')[0] || '';
  for (const line of sec1.split('\n')) {
    const m = line.match(/^\|\s*([^|]+?)\s*\|((?:[^|]+\|){5})\s*(\d+)\s*\|\s*$/);
    if (!m || /échantillon|---/.test(m[1])) continue;
    const cells = m[2].split('|').filter((c) => c.trim()).map((c) => c.split('/').map((x) => num(x.trim())));
    out.rows.push({ name: m[1], h: cells, n60: +m[3] });
  }
  const ev = md.match(/Jours évaluables \(≥ 12 h relevées\) : (\d+)/);
  if (ev) out.evalDays = +ev[1];
  const pd = md.match(/Par jour \(\+60 min, modèle \/ persistance\) :(.*)/);
  if (pd) {
    for (const part of pd[1].split(';')) {
      const m = part.trim().match(/^(\S+ \d{2}\/\d{2})( \(férié\))? (\S+)\/(\S+)$/);
      if (m) out.perDay.push({ day: m[1], holiday: !!m[2], model: num(m[3]), per: num(m[4]) });
    }
  }
  const sec2 = md.split('## 2.')[1]?.split('## 3.')[0] || '';
  for (const line of sec2.split('\n')) {
    const m = line.match(/^\s*(\d)\. \*\*(.+?)\*\*(.*)$/);
    if (m) out.verdicts.push({ q: m[2], text: m[3].replace(/\s+$/, '').replace(/^\s*/, '') });
    else if (out.verdicts.length && /^\s{2,}\S/.test(line)) out.verdicts.at(-1).text += (out.verdicts.at(-1).text ? ' ' : '') + line.trim();
  }
  const sp = md.match(/; (\d+) tranches de 5 min, du (.+?) au (.+?) \(([\d.]+) jours, (\d+) % des tranches présentes\)/);
  if (sp) out.span = { slots: +sp[1], from: sp[2], to: sp[3], days: +sp[4], coverPct: +sp[5] };
  return out;
}

export class NightBench {
  constructor({ dataDir, archiveDir, bin = process.env.VN_BENCH_BIN || '', cwd = process.env.VN_BENCH_CWD || '', keep = 60, log = () => {} }) {
    this.dir = join(dataDir, 'banc');
    this.archiveDir = archiveDir;
    this.bin = bin;
    this.cwd = cwd || (bin ? dirname(bin) : '');
    this.keep = keep;
    this.log = log;
    this.running = false;
    mkdirSync(this.dir, { recursive: true });
  }
  get available() { return !!this.bin && existsSync(this.bin); }
  reports() {
    try { return readdirSync(this.dir).filter((f) => /^\d{4}-\d{2}-\d{2}\.json$/.test(f)).sort().map((f) => f.slice(0, 10)); } catch { return []; }
  }
  read(day) {
    try {
      const j = JSON.parse(readFileSync(join(this.dir, `${day}.json`), 'utf8'));
      return j;
    } catch { return null; }
  }
  markdown(day) { try { return readFileSync(join(this.dir, `${day}.md`), 'utf8'); } catch { return null; } }
  latest() { const r = this.reports(); return r.length ? this.read(r.at(-1)) : null; }

  // Choix des entrées : tout l'historique de l'archive, et le profil du PLUS ANCIEN instantané comme
  // départ (le banc écarte les tranches que ce profil a déjà apprises : mesure sans fuite).
  inputs() {
    const snaps = snapshotDirs(this.archiveDir).filter((d) => existsSync(join(d, 'vn_prof.bin')));
    if (snaps.length < 2) return { why: `archive trop courte (${snaps.length} instantané${snaps.length > 1 ? 's' : ''} ; il en faut au moins 2, soit une journée)` };
    return { args: ['--reel', this.archiveDir, join(snaps[0], 'vn_prof.bin')], snaps: snaps.length };
  }
  async run(day, { timeoutS = 1800 } = {}) {
    if (!this.available) return { ok: false, why: 'banc absent (VN_BENCH_BIN)' };
    if (this.running) return { ok: false, why: 'banc déjà en cours' };
    const inp = this.inputs();
    if (!inp.args) {
      const j = { day, ok: false, why: inp.why, at: Math.floor(Date.now() / 1000) };
      atomicWrite(join(this.dir, `${day}.json`), JSON.stringify(j));
      this.log(`[banc] ${inp.why}`);
      return j;
    }
    this.running = true;
    const t0 = Date.now();
    this.log(`[banc] bench_prevision ${inp.args.join(' ')} (${inp.snaps} instantanés)`);
    try {
      const r = await new Promise((resolve) => {
        const p = spawn(this.bin, inp.args, { cwd: this.cwd || undefined, stdio: ['ignore', 'pipe', 'pipe'] });
        let out = '', err = '';
        p.stdout.on('data', (d) => { out += d; if (out.length > 4e6) p.kill('SIGKILL'); });
        p.stderr.on('data', (d) => { err += d; });
        const to = setTimeout(() => p.kill('SIGKILL'), timeoutS * 1000);
        p.on('error', (e) => { clearTimeout(to); resolve({ code: -1, out, err: String(e) }); });
        p.on('close', (code) => { clearTimeout(to); resolve({ code, out, err }); });
      });
      const parsed = parseBenchReport(r.out);
      const j = { day, ok: r.code === 0 && !parsed.empty, code: r.code, durationS: +((Date.now() - t0) / 1000).toFixed(1), at: Math.floor(Date.now() / 1000),
        snaps: inp.snaps, stderr: r.err.slice(0, 2000), ...parsed };
      writeFileSync(join(this.dir, `${day}.md`), r.out || r.err);
      atomicWrite(join(this.dir, `${day}.json`), JSON.stringify(j));
      this.prune();
      const all = parsed.rows.find((x) => x.name === 'tous les jours');
      this.log(`[banc] fini en ${j.durationS} s (code ${r.code})${all ? ` : MAE +60 modèle ${all.h[2][0] ?? '—'} / persistance ${all.h[2][1] ?? '—'} sur ${parsed.evalDays} jour(s)` : ''}`);
      return j;
    } finally { this.running = false; }
  }
  prune() {
    const r = this.reports();
    for (const d of r.slice(0, Math.max(0, r.length - this.keep))) {
      for (const ext of ['json', 'md']) { try { rmSync(join(this.dir, `${d}.${ext}`)); } catch { /* absent */ } }
    }
  }
}
