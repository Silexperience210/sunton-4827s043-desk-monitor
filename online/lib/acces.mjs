// ── Contrôle d'accès du serveur ──────────────────────────────────────
// La page et les lectures sont publiques (tableau de bord ouvert). Sont « protégés » : tout ce qui écrit,
// ce qui expose des réglages ou les écrans du réseau local, la synchro des cartes, et la régulation (/reg/… :
// déclarations nominatives des régulateurs, tournées, réglages).
//  - VN_TOKEN défini : jeton exigé (Authorization: Bearer, ou X-VN-Token ; jamais dans l'URL), sinon 401.
//  - VN_TOKEN vide : les ÉCRITURES sont fermées (403), sauf VN_OPEN=1 explicite ou une requête locale
//    (adresse de bouclage, en-tête Host local, aucun en-tête de proxy : un rebinding DNS ou un proxy du même
//    hôte ne passent pas). Les lectures protégées (synchro des cartes, réglages, écrans) restent ouvertes.
//  - Sans jeton, une requête de navigateur venue d'une autre origine (Origin ≠ Host, Sec-Fetch-Site autre que
//    same-origin / none) est refusée sur toute route protégée : un POST « simple » en text/plain, ou une image
//    pointée sur /sync/…/report, part de n'importe quelle page web sans préflight (CSRF).
//  - Corps typés : application/json sur POST /api/notify et POST /reg/…, application/octet-stream sur les envois des cartes
//    (415) — un type « simple » serait la porte d'entrée du CSRF.
import { timingSafeEqual } from 'node:crypto';

// Comparaison en temps constant : une comparaison par === laisse fuir la longueur du préfixe correct.
function sameSecret(a, b) {
  const A = Buffer.from(String(a || '')), B = Buffer.from(String(b || ''));
  if (A.length !== B.length) return false;
  return timingSafeEqual(A, B);
}

// Le jeton ne circule QUE dans un en-tête : accepté en ?token=, il fuit dans les journaux du proxy,
// l'historique du navigateur et les en-têtes Referer.
export function tokenOk(req, token) {
  if (!token) return false;
  const h = req.headers.authorization || '';
  const bearer = h.startsWith('Bearer ') ? h.slice(7) : '';
  if (bearer && sameSecret(bearer, token)) return true;
  const alt = req.headers['x-vn-token'];
  return !!(alt && sameSecret(alt, token));
}

export function protectedRoute(p, method) {
  if (p.startsWith('/sync/')) return true;              // écrit l'état appris
  if (p.startsWith('/reg/')) return true;               // régulation : déclarations nominatives, réglages, tournées
  if (p.startsWith('/api/notify')) return true;         // réglages de notification (ntfy)
  if (p === '/api/bench/run') return true;              // lance un binaire
  if (p === '/api/devices') return true;                // écrans du réseau local
  if (method !== 'GET' && method !== 'HEAD') return true;  // toute écriture
  return false;
}

const LOOPBACK_IP = /^(127\.\d+\.\d+\.\d+|::1|::ffff:127\.\d+\.\d+\.\d+)$/;
const LOCAL_HOST = /^(localhost|127\.\d+\.\d+\.\d+|\[::1\])(:\d+)?$/i;
const PROXY_HEADERS = ['x-forwarded-for', 'x-forwarded-host', 'x-real-ip', 'forwarded', 'cf-connecting-ip'];

export function isLocal(req) {
  return LOOPBACK_IP.test(req.socket?.remoteAddress || '') && LOCAL_HOST.test(req.headers.host || '')
    && !PROXY_HEADERS.some((h) => req.headers[h] !== undefined);
}

// Requête de navigateur venue d'une autre origine que la page (les cartes et curl n'envoient ni Origin ni Sec-Fetch-Site)
export function crossOrigin(req) {
  const site = req.headers['sec-fetch-site'];
  if (site !== undefined && site !== 'same-origin' && site !== 'none') return true;
  const origin = req.headers.origin;
  if (origin === undefined) return false;
  try { return new URL(origin).host.toLowerCase() !== String(req.headers.host || '').toLowerCase(); } catch { return true; }  // « null » compris
}

const mime = (req) => String(req.headers['content-type'] || '').split(';')[0].trim().toLowerCase();

// null si la requête passe ; sinon { code, error } à renvoyer tel quel
export function checkAccess(req, p, { token = '', open = false } = {}) {
  if (!(p.startsWith('/api/') || p.startsWith('/sync/') || p.startsWith('/reg/'))) return null;
  const method = req.method;
  if (protectedRoute(p, method)) {
    if (token) {
      if (!tokenOk(req, token)) return { code: 401, error: 'jeton requis' };
    } else {
      if (crossOrigin(req)) return { code: 403, error: 'requête venue d\'une autre origine : refusée' };
      const write = method !== 'GET' && method !== 'HEAD';
      if (write && !open && !isLocal(req)) return { code: 403, error: 'écritures fermées : définir VN_TOKEN (ou VN_OPEN=1 sur un réseau de confiance)' };
    }
  }
  if (method === 'POST' && p === '/api/notify' && mime(req) !== 'application/json') return { code: 415, error: 'Content-Type: application/json attendu' };
  if (method === 'POST' && p.startsWith('/reg/') && mime(req) !== 'application/json') return { code: 415, error: 'Content-Type: application/json attendu' };
  if (method === 'POST' && /^\/sync\/[^/]+\/upload\//.test(p) && mime(req) !== 'application/octet-stream') return { code: 415, error: 'Content-Type: application/octet-stream attendu' };
  return null;
}
