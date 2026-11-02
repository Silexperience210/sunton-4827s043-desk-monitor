// ── Qualité du flux Vel'OH! : ce que le serveur applique, et ce qu'il refuse ─
// Rejoue les constats de la revue du 01/11/2026 sur le vrai serveur, avec un faux flux GBFS local (127.0.0.1).
//  m6. flux périmé (last_updated vieux de 6 h, le fournisseur gèle en répondant 200) : non appliqué — aucune
//      tranche ouverte, rien d'appris, /healthz en 503 ; appliqué de nouveau dès que le flux est à jour.
//      last_reported par station n'est PAS filtré : dans le flux réel, 135 stations sur 148 ont un last_reported
//      de plus de 6 h tout en louant (docs/donnees/station_status.json) ; ce n'est pas un signal de vie.
// node online/test/flux.test.mjs
import { mkdtempSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { req, startServer, fakeGbfs, sleep } from './aides.mjs';

let fails = 0, checks = 0;
const ok = (c, msg) => { checks++; if (!c) { fails++; console.log(`  ÉCHEC : ${msg}`); } else if (process.env.VN_TEST_DETAIL) console.log(`  ok : ${msg}`); };
const tmp = mkdtempSync(join(tmpdir(), 'vn-flux-'));
let dirN = 0;
const fresh = () => join(tmp, `d${++dirN}`);
// Attend que le serveur ait servi `n` station_status de plus, puis le temps de les appliquer
async function cycles(gbfs, n0, n = 1) {
  for (let k = 0; k < 100 && gbfs.status < n0 + n; k++) await sleep(100);
  await sleep(400);
}

try {
  console.log('m6. flux périmé (last_updated vieux de 6 h) : non appliqué, aucune tranche ouverte');
  {
    const gbfs = await fakeGbfs();
    const D = fresh();
    try {
      const now = Math.floor(Date.now() / 1000);
      gbfs.lastUpdated = () => new Date((Math.floor(Date.now() / 1000) - 6 * 3600) * 1000).toISOString();
      let n0 = gbfs.status;
      let s = await startServer({ data: D, env: { VN_GBFS_BASE: gbfs.base, VN_REFRESH_S: '30' } });
      await cycles(gbfs, n0);
      let sum = (await req(s.port, { path: '/api/summary' })).json;
      let ring = (await req(s.port, { path: '/api/ring' })).json || [];
      const health = (await req(s.port, { path: '/healthz' })).status;
      await s.stop();
      ok(gbfs.status > n0, `le faux flux a été lu (${gbfs.status - n0} station_status servis)`);
      ok(!sum.lastUpdate && ring.length === 0, `flux vieux de 6 h : rien d'appliqué (lastUpdate ${sum.lastUpdate || 0}, ${ring.length} tranche(s) dans l'anneau)`);
      ok(health === 503, `/healthz : ${health} (503 attendu : le relevé n'avance pas)`);
      ok(/station_status : last_updated .* vieux de 6 h/.test(s.log()), `refus journalisé (${(s.log().match(/\[net\] station_status[^\n]*/) || ['aucune ligne'])[0]})`);
      // le flux redevient à jour : appliqué au premier cycle
      gbfs.lastUpdated = undefined;
      n0 = gbfs.status;
      s = await startServer({ data: D, env: { VN_GBFS_BASE: gbfs.base, VN_REFRESH_S: '30' } });
      await cycles(gbfs, n0);
      sum = (await req(s.port, { path: '/api/summary' })).json;
      ring = (await req(s.port, { path: '/api/ring' })).json || [];
      await s.stop();
      ok(sum.lastUpdate >= now && ring.length >= 1, `flux à jour : appliqué (lastUpdate ${sum.lastUpdate - now} s après le début, ${ring.length} tranche(s))`);
      // retard ordinaire du fournisseur (1 min) : appliqué
      gbfs.lastUpdated = () => new Date(Date.now() - 60000).toISOString();
      n0 = gbfs.status;
      s = await startServer({ data: fresh(), env: { VN_GBFS_BASE: gbfs.base, VN_REFRESH_S: '30' } });
      await cycles(gbfs, n0);
      sum = (await req(s.port, { path: '/api/summary' })).json;
      await s.stop();
      ok(sum.lastUpdate >= now, 'flux en retard de 1 min : appliqué');
    } finally { await gbfs.close(); }
  }
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`\n${checks} vérifications, ${fails} échec(s)`);
process.exit(fails ? 1 : 0);
