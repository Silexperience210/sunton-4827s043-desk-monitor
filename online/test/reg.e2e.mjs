// ── Régulation de bout en bout : serveur en ligne ⇄ Sunton du camion simulé ─
// Sans Internet : un faux GBFS / Open-Meteo local (instantané docs/donnees), le vrai serveur
// (online/server.mjs) et le vrai rôle « régulateur » du firmware (src/core/regul.cpp, compilé dans
// sim/reg_sim avec curl pour transport ; sa « flash » est un dossier, gardée d'un appel à l'autre).
// Vérifie :
//   1. sans jeton, rien n'est lu ni envoyé ; avec, le Sunton reçoit camions et régulateurs ;
//   2. tournée commencée et arrêt déclaré HORS CONNEXION : rangés dans la file, la vue locale suit
//      (chargement), puis tout part dans l'ordre au retour du réseau ;
//   3. la même file renvoyée (redémarrage au mauvais moment) : le serveur reconnaît les doublons ;
//   4. un refus du serveur (chargement devenu négatif après une déclaration du téléphone) sort de la
//      file avec la raison du serveur, affichée telle quelle ;
//   5. « arrêt suivant » : fait comme proposé, et le plan relu ne le propose plus ;
//   6. redémarrage sans réseau : la tournée et le plan gardés en flash restent affichés, datés ;
//   7. un second appareil rejoint la tournée ; la fin de tournée garde déclaré et calculé côte à côte ;
//   8. début refusé (camion retiré entre-temps) : les déclarations de cette tournée sont dites non enregistrées.
// Prérequis : cd sim && make reg_sim.   Lancer : node online/test/reg.e2e.mjs
import { createServer } from 'node:http';
import { spawn, execFileSync } from 'node:child_process';
import { readFileSync, mkdtempSync, existsSync, copyFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..', '..');
const D = join(ROOT, 'docs', 'donnees');
const SIM = join(ROOT, 'sim', 'reg_sim');
let fails = 0, checks = 0;
const ok = (c, msg) => { checks++; console.log(`  ${c ? 'ok   ' : 'ÉCHEC'} ${msg}`); if (!c) fails++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
if (!existsSync(SIM)) { console.log('sim/reg_sim absent : cd sim && make reg_sim'); process.exit(2); }

// ── Faux GBFS + météo ──
const info = readFileSync(join(D, 'station_information.json'), 'utf8');
const types = readFileSync(join(D, 'vehicle_types.json'), 'utf8');
const wx = readFileSync(join(D, 'weather.json'), 'utf8');
const st0 = JSON.parse(readFileSync(join(D, 'station_status.json'), 'utf8'));
function status(t) {
  st0.last_updated = new Date(t * 1000).toISOString();
  return JSON.stringify(st0);
}
const mock = createServer((req, res) => {
  const body = req.url.includes('station_status') ? status(Math.floor(Date.now() / 1000))
    : req.url.includes('station_information') ? info : req.url.includes('vehicle_types') ? types : req.url.startsWith('/wx') ? wx : null;
  res.writeHead(body ? 200 : 404, { 'Content-Type': 'application/json' });
  res.end(body || '{}');
});
await new Promise((r) => mock.listen(0, '127.0.0.1', r));
const mockUrl = `http://127.0.0.1:${mock.address().port}`;

// ── Serveur en ligne (vrai processus) ──
const TOKEN = 'jeton-regulation';
const data = mkdtempSync(join(tmpdir(), 'vn-reg-e2e-'));
const port = 19290 + Math.floor(Math.random() * 600);
const base = `http://127.0.0.1:${port}`;
const DEAD = 'http://127.0.0.1:9';                    // partage de connexion coupé
const srv = spawn(process.execPath, [join(ROOT, 'online', 'server.mjs')], {
  env: { ...process.env, VN_DATA: join(data, 'serveur'), VN_PORT: String(port), VN_HOST: '127.0.0.1', VN_TOKEN: TOKEN,
    VN_GBFS_BASE: `${mockUrl}/gbfs`, VN_WX_URL: `${mockUrl}/wx`, VN_ARCHIVE_HOURS: '', VN_NIGHT_AT: '23:59' },
  stdio: ['ignore', 'pipe', 'pipe'] });
let slog = '';
srv.stdout.on('data', (d) => (slog += d));
srv.stderr.on('data', (d) => (slog += d));
const api = async (path, body, dev = 'telephone-lea') => {
  const r = await fetch(base + path, { method: body ? 'POST' : 'GET', headers: { Authorization: `Bearer ${TOKEN}`, 'X-VN-Device': dev, ...(body ? { 'Content-Type': 'application/json' } : {}) }, body: body ? JSON.stringify(body) : undefined });
  return { status: r.status, j: await r.json().catch(() => ({})) };
};
// Le Sunton simulé : une commande, sortie « CLÉ valeur » par ligne
const flash1 = join(data, 'sunton-camion'), flash2 = join(data, 'sunton-second');
function sunton(url, flash, args, env = {}) {
  const out = execFileSync(SIM, [url, flash, ...args.map(String)], { env: { ...process.env, VN_TOKEN: TOKEN, ...env }, encoding: 'utf8', timeout: 60000 });
  const lines = out.trim().split('\n');
  const one = (k) => lines.find((l) => l.startsWith(k + ' '))?.slice(k.length + 1) ?? null;
  const all = (k) => lines.filter((l) => l.startsWith(k + ' ')).map((l) => l.slice(k.length + 1));
  return { out, one, all, ok: one('OK') === '1', poll: Number(one('POLL')), queue: Number(one('QUEUE')?.split(' ')[0]),
    load: one('LOAD')?.split(' ').map(Number), tour: one('TOUR'), stops: all('STOP'), rej: all('REJ') };
}

try {
  let ready = false;
  for (let i = 0; i < 100 && !ready; i++) { await sleep(150); try { ready = (await api('/api/stations')).status === 200; } catch { /* pas prêt */ } }
  ok(ready, 'serveur prêt, flux relevé');
  const cf = await api('/reg/config', { trucks: [{ id: 'kangoo', name: 'Kangoo', capacity: 10 }, { id: 'master', name: 'Master', capacity: 25 }],
    regulators: [{ id: 'paul', name: 'Paul' }, { id: 'lea', name: 'Léa' }], atelier: { name: 'Atelier Bertrange', point: '49.6106, 6.0507' }, atelierStock: 6 });
  ok(cf.status === 200 && cf.j.trucks.length === 2, 'réglages de la régulation déclarés');
  const stations = (await api('/api/stations')).j.filter((s) => s.active && s.live);
  const S1 = stations.find((s) => s.bikes <= 2) || stations[0];

  console.log('1. jeton');
  let r = sunton(base, flash1, ['poll'], { VN_TOKEN: '' });
  ok(r.poll === -1 && /jeton/.test(r.one('LASTERR') || '') && r.one('LISTS') === '0 0', `sans jeton : rien reçu (${r.one('LASTERR')})`);
  r = sunton(base, flash1, ['poll']);
  ok(r.poll === 0 && r.one('LISTS') === '2 2' && r.tour === '-', 'avec le jeton : camions et régulateurs reçus, aucune tournée');

  console.log('2. hors connexion');
  r = sunton(DEAD, flash1, ['start', 1, 1, 8, 0]);
  const tourId = r.tour?.split(' ')[0];
  ok(r.ok && r.poll === -1 && r.queue === 1 && /^t-sunton-camion-sim-/.test(tourId) && r.tour.split(' ')[1] === '1', `tournée commencée sans réseau, gardée en file (${r.tour})`);
  ok(/1 geste en attente/.test(r.one('STATUS')), `écran : ${r.one('STATUS')}`);
  r = sunton(DEAD, flash1, ['station', S1.sid, 3, 0, 0]);
  ok(r.ok && r.queue === 2 && r.load[0] === 5, `arrêt déclaré sans réseau : à bord ${r.load?.join(' + HS ')}`);
  r = sunton(DEAD, flash1, ['station', S1.sid, 9, 0, 0]);
  ok(!r.ok && /que 5/.test(r.one('WHY')), `contrôle local avant tout envoi : « ${r.one('WHY')} »`);
  copyFileSync(join(flash1, 'vn_regq.bin'), join(data, 'file-copie.bin'));
  r = sunton(base, flash1, ['poll']);
  ok(r.poll === 2 && r.queue === 0 && r.tour.split(' ')[0] === tourId && r.tour.split(' ')[1] === '0', 'retour du réseau : les deux gestes partent dans l\'ordre, tournée confirmée');
  let st = (await api('/reg/state?days=1')).j;
  let evs = st.events.filter((e) => e.tour === tourId);
  ok(st.open.some((t) => t.id === tourId && t.device === 'sunton-camion-sim' && t.truckName === 'Master'), 'serveur : tournée ouverte au nom du Sunton');
  ok(evs.length === 1 && evs[0].sid === S1.sid && evs[0].dropped.ok === 3 && evs[0].name === S1.name && evs[0].state, `serveur : déclaration enregistrée, nom pris dans le flux (« ${evs[0]?.name} »), état de la station à la réception`);

  console.log('3. doublons');
  copyFileSync(join(data, 'file-copie.bin'), join(flash1, 'vn_regq.bin'));
  r = sunton(base, flash1, ['poll']);
  st = (await api('/reg/state?days=1')).j;
  ok(r.poll === 2 && r.queue === 0 && st.events.filter((e) => e.tour === tourId).length === 1 && st.open.filter((t) => t.id === tourId).length === 1,
    'même file renvoyée : doublons reconnus, rien compté deux fois');

  console.log('4. refus du serveur');
  const ph = await api('/reg/event', { id: `tel-lea-${Date.now().toString(36)}`, tour: tourId, kind: 'station', sid: stations[1].sid, at: Math.floor(Date.now() / 1000), dropped: { ok: 4 } });
  ok(ph.status === 200 && ph.j.load.ok === 1, 'le téléphone dépose 4 vélos dans la même tournée (reste 1 à bord)');
  r = sunton(DEAD, flash1, ['station', stations[2].sid, 3, 0, 0]);
  ok(r.ok && r.load[0] === 2, 'le Sunton, hors ligne et sans le savoir, déclare 3 de plus (vue locale : 2)');
  r = sunton(base, flash1, ['poll']);
  ok(r.poll === 1 && r.queue === 0 && r.rej.length === 1 && /négatif/.test(r.rej[0]) && r.load[0] === 1, `refus gardé avec la raison du serveur : « ${r.rej[0]} » ; à bord ${r.load?.[0]} (serveur)`);

  console.log('5. arrêt suivant');
  r = sunton(base, flash1, ['atelier', 0, 0, 6]);
  ok(r.ok && r.poll === 1 && r.load[0] === 7, `atelier : 6 neufs repris (à bord ${r.load?.[0]})`);
  r = sunton(base, flash1, ['poll']);
  const firstStop = r.stops[0];
  ok(r.stops.length > 0 && /ESTIM|estimé|mesuré/.test(r.out), `plan reçu : ${r.stops.length} arrêt(s), premier « ${firstStop?.split(' | ')[0]} »`);
  r = sunton(base, flash1, ['next']);
  const doneSid = r.one('NEXT')?.split(' ')[0];
  ok(r.ok && r.poll === 1, `fait comme proposé : ${r.one('NEXT')}`);
  const plan = (await api('/reg/plan?force=1')).j.plans.find((p) => p.tour === tourId);
  ok(plan && !plan.stops.some((s) => s.sid === doneSid) && !r.stops.some((l) => l.split(' ')[2] === doneSid), 'le plan relu ne repropose pas la station tout juste servie');

  console.log('6. redémarrage sans réseau');
  r = sunton(DEAD, flash1, ['show'], { REG_OFFLINE: '1' });
  ok(r.tour.split(' ')[0] === tourId && r.one('SNAP').split(' ')[2] === '1' && r.stops.length > 0 && /gard/.test(r.one('STATUS')), `tournée et plan relus de la flash, datés (${r.one('STATUS')})`);

  console.log('7. second appareil, fin de tournée');
  r = sunton(base, flash2, ['poll'], { REG_DEV: 'sunton-second' });
  ok(r.tour === '-' && r.all('OPEN').some((l) => l.startsWith(tourId)), 'second Sunton : voit la tournée ouverte');
  r = sunton(base, flash2, ['join', tourId], { REG_DEV: 'sunton-second' });
  ok(r.tour.split(' ')[0] === tourId, 'second Sunton : a rejoint la tournée');
  const loadNow = r.load;
  r = sunton(base, flash1, ['end', loadNow[0], loadNow[1]]);
  ok(r.ok && r.poll === 1 && r.tour === '-', 'fin de tournée envoyée');
  const tours = JSON.parse(readFileSync(join(data, 'serveur', 'regulation', 'tours.json'), 'utf8'));
  const t = tours.find((x) => x.id === tourId);
  ok(t && t.endedAt > 0 && t.loadEndDeclared.ok === loadNow[0] && JSON.stringify(t.loadEndDeclared) === JSON.stringify(t.loadEndComputed),
    `serveur : fin, déclaré ${JSON.stringify(t?.loadEndDeclared)} = calculé ${JSON.stringify(t?.loadEndComputed)}`);
  r = sunton(base, flash2, ['poll'], { REG_DEV: 'sunton-second' });
  ok(r.tour === '-', 'second Sunton : la tournée rejointe est finie, il le voit');

  console.log('8. début refusé');
  r = sunton(DEAD, flash1, ['start', 0, 0, 2, 0]);
  sunton(DEAD, flash1, ['station', S1.sid, 1, 0, 0]);
  sunton(DEAD, flash1, ['station', stations[3].sid, 1, 0, 0]);
  await api('/reg/config', { trucks: [{ id: 'master', name: 'Master', capacity: 25 }] });
  r = sunton(base, flash1, ['poll']);
  ok(r.queue === 0 && r.rej.some((x) => /camion inconnu/.test(x) && /2 déclarations non enregistrées/.test(x)), `dit une fois : « ${r.rej[0]} »`);
} catch (err) {
  ok(false, `exception : ${err.stack || err}`);
} finally {
  srv.kill('SIGTERM');
  mock.close();
  await new Promise((res) => srv.on('exit', res));
}
if (fails) console.log(slog.split('\n').slice(-25).join('\n'));
rmSync(data, { recursive: true, force: true });
console.log(`\n${checks - fails}/${checks} vérifications réussies${fails ? ` — ${fails} ÉCHEC(S)` : ''}`);
process.exit(fails ? 1 : 0);
