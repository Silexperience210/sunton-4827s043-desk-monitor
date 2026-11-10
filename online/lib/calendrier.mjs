// ── Calendrier : type de chaque journée (diagnostic des prévisions par type de jour) ──
// Quatre types, dans cet ordre de priorité :
//   evenement  un événement DÉCLARÉ (saisi à la main : <données>/calendrier.json) ;
//   weekend    samedi, dimanche ou jour férié légal — la classe 1 du modèle (Model::fcClassAt), lue du moteur ;
//   vacances   jour ouvré pendant les vacances ou un congé scolaire de l'enseignement public luxembourgeois ;
//   ouvre      les autres jours ouvrés.
// Vacances et congés : calendrier officiel du ministère de l'Éducation nationale (men.public.lu, « Vacances
// scolaires », règlement grand-ducal du 3 juin 2026), relevé le 10/11/2026. Du samedi au dimanche inclus, comme
// le texte les donne. Rien n'est extrapolé : après la dernière période connue, un jour ouvré est « ouvre » et la
// page dit jusqu'où le calendrier est renseigné (SCHOOL_KNOWN_TO) — à compléter ici ou dans calendrier.json.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

export const SCHOOL_HOLIDAYS = [
  // année scolaire 2026/2027 (du mardi 15/09/2026 au jeudi 15/07/2027)
  ['2026-10-31', '2026-11-08', 'Toussaint'],
  ['2026-12-19', '2027-01-03', 'Noël'],
  ['2027-02-06', '2027-02-14', 'Carnaval'],
  ['2027-03-27', '2027-04-11', 'Pâques'],
  ['2027-05-29', '2027-06-06', 'Pentecôte'],
  ['2027-07-16', '2027-09-14', 'été'],
  // année scolaire 2027/2028 (rentrée le mercredi 15/09/2027)
  ['2027-10-30', '2027-11-07', 'Toussaint'],
  ['2027-12-18', '2028-01-02', 'Noël'],
  ['2028-02-12', '2028-02-20', 'Carnaval'],
  ['2028-04-01', '2028-04-16', 'Pâques'],
  ['2028-05-27', '2028-06-04', 'Pentecôte'],
];
export const SCHOOL_KNOWN_FROM = '2026-09-15', SCHOOL_KNOWN_TO = '2028-06-04';

const DAY = /^\d{4}-\d{2}-\d{2}$/;
const LABEL = { evenement: 'Événement déclaré', weekend: 'Week-end et fériés', vacances: 'Vacances scolaires (jours ouvrés)', ouvre: 'Jours ouvrés hors vacances' };

// <données>/calendrier.json, facultatif :
//   { "evenements": [ { "du": "2027-06-12", "au": "2027-06-13", "nom": "exemple : un événement saisi à la main" } ],
//     "vacances":   [ { "du": "2028-07-14", "au": "2028-09-14", "nom": "été" } ] }      (s'ajoutent aux officielles)
// Une entrée mal formée est ignorée et nommée dans `errors` (affiché par la page) — jamais corrigée en silence.
export function loadCalendar(dataDir) {
  const out = { events: [], school: SCHOOL_HOLIDAYS.map(([du, au, nom]) => ({ du, au, nom })), errors: [], file: false };
  let raw;
  try { raw = readFileSync(join(dataDir, 'calendrier.json'), 'utf8'); } catch { return out; }
  out.file = true;
  let j;
  try { j = JSON.parse(raw); } catch (err) { out.errors.push(`calendrier.json illisible : ${err.message}`); return out; }
  const take = (list, into, what) => {
    if (list === undefined) return;
    if (!Array.isArray(list)) { out.errors.push(`${what} : une liste est attendue`); return; }
    list.forEach((e, k) => {
      const du = e?.du, au = e?.au ?? e?.du, nom = String(e?.nom || '').slice(0, 60);
      if (!DAY.test(du || '') || !DAY.test(au || '') || au < du || Number.isNaN(Date.parse(du)) || Number.isNaN(Date.parse(au))) {
        out.errors.push(`${what} n° ${k + 1} : dates « du » / « au » attendues en AAAA-MM-JJ, dans l'ordre`);
        return;
      }
      into.push({ du, au, nom });
    });
  };
  take(j.evenements, out.events, 'evenements');
  take(j.vacances, out.school, 'vacances');
  return out;
}

const inside = (list, day) => list.find((p) => day >= p.du && day <= p.au);

// (jour, classe du modèle 0 | 1) → { type, label, name }
export function dayType(cal, day, modelClass) {
  const ev = inside(cal.events, day);
  if (ev) return { type: 'evenement', label: LABEL.evenement, name: ev.nom };
  if (modelClass === 1) return { type: 'weekend', label: LABEL.weekend, name: '' };
  const sc = inside(cal.school, day);
  if (sc) return { type: 'vacances', label: LABEL.vacances, name: sc.nom };
  return { type: 'ouvre', label: LABEL.ouvre, name: '' };
}
