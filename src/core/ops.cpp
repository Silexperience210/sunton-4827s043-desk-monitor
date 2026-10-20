#include "ops.h"
#include "../config.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

namespace vn {

// ── Ingestion ────────────────────────────────────────────────────────
const char * feedName(int f) {
  switch (f) {
    case FEED_STATUS: return "station_status";
    case FEED_INFO: return "station_information";
    case FEED_TYPES: return "vehicle_types";
    case FEED_WEATHER: return "m\xC3\xA9t\xC3\xA9o (Open-Meteo)";
    default: return "?";
  }
}

const char * ingestErrText(int c) {
  if (c >= 200) {
    switch (c) {
      case 401: case 403: return "acc\xC3\xA8s refus\xC3\xA9";
      case 404: return "introuvable";
      case 429: return "trop de requ\xC3\xAAtes";
      case 500: return "erreur serveur";
      case 502: case 504: return "passerelle";
      case 503: return "service indisponible";
      default: return c >= 500 ? "erreur serveur" : c >= 400 ? "requ\xC3\xAAte refus\xC3\xA9" "e" : "r\xC3\xA9ponse inattendue";
    }
  }
  switch (c) {                                       // HTTPClient.h (HTTPC_ERROR_*)
    case -1: return "connexion refus\xC3\xA9" "e";
    case -2: return "envoi de l'en-t\xC3\xAAte";
    case -3: return "envoi de la requ\xC3\xAAte";
    case -4: return "non connect\xC3\xA9";
    case -5: return "connexion perdue";
    case -6: return "pas de flux";
    case -7: return "pas de serveur HTTP";
    case -8: return "m\xC3\xA9moire insuffisante";
    case -9: return "encodage non g\xC3\xA9r\xC3\xA9";
    case -10: return "\xC3\xA9" "criture du flux";
    case -11: return "d\xC3\xA9lai de lecture d\xC3\xA9pass\xC3\xA9";
    case ING_E_PARSE: return "JSON illisible";
    case ING_E_EMPTY: return "aucun enregistrement";
    case ING_E_TRUNC: return "r\xC3\xA9ponse tronqu\xC3\xA9" "e";
    case ING_E_BEGIN: return "URL refus\xC3\xA9" "e";
    default: return "\xC3\xA9" "chec";
  }
}

void ingestOk(FeedStat & f, int64_t now, uint32_t bytes, uint32_t ms) {
  f.okAt = now;
  f.fails = 0;
  f.bytes = bytes;
  f.ms = ms;
  f.okCount++;
}

void ingestErr(FeedStat & f, int64_t now, int code, uint32_t bytes, uint32_t ms) {
  f.errAt = now;
  f.errCode = (int16_t)code;
  if (f.fails < 0xFFFF) f.fails++;
  f.bytes = bytes;
  f.ms = ms;
  f.errCount++;
}

// ── Fraîcheur ────────────────────────────────────────────────────────
int staleThresholdS(const Settings & s) {
  if (s.staleMin) return s.staleMin * 60;
  int t = 3 * (int)s.refreshSec;
  return t < 180 ? 180 : t;
}

int wxStaleThresholdS() { return 3 * VN_WX_REFRESH_S; }

int64_t dataAgeS(int64_t okAt, int64_t now) {
  if (okAt <= 0 || now <= 0) return -1;
  int64_t a = now - okAt;
  return a < 0 ? 0 : a;
}

bool isStale(int64_t okAt, int64_t now, int thresholdS) {
  int64_t a = dataAgeS(okAt, now);
  return a >= 0 && a > thresholdS;
}

DataState dataState(int64_t lastUpdate, int64_t now, int thresholdS, bool wifiUp) {
  if (lastUpdate <= 0) return DS_NONE;
  if (!wifiUp) return DS_OFFLINE;
  return isStale(lastUpdate, now, thresholdS) ? DS_STALE : DS_LIVE;
}

// ── Association Wi-Fi ────────────────────────────────────────────────
// Codes de esp_wifi_types.h (ESP-IDF 4.4). Le framework Arduino les journalise en
// « warning », filtré par CORE_DEBUG_LEVEL=1 : sans cette traduction, un échec
// d'association ne laissait aucune trace.
AssocCause assocCause(int r) {
  switch (r) {
    case 0:                                          // aucun évènement : pas de réponse
    case 2: case 4: case 39: case 205:               // AUTH_EXPIRE, ASSOC_EXPIRE, TIMEOUT, CONNECTION_FAIL
      return AC_TIMEOUT;
    case 201: case 210: case 211: case 212:          // NO_AP_FOUND (et variantes IDF 5)
      return AC_NOT_FOUND;
    case 14: case 15: case 16: case 23: case 202: case 204:   // MIC, 4WAY/GROUP_KEY, 802.1X, AUTH_FAIL, HANDSHAKE
      return AC_AUTH;
    case 3: case 6: case 7: case 8: case 34: case 200:        // AUTH_LEAVE, NOT_AUTHED/ASSOCED, ASSOC_LEAVE, ACKS, BEACON
      return AC_LOST;
    default:
      return AC_REFUSED;
  }
}

const char * assocCauseText(int c) {
  switch (c) {
    case AC_NOT_FOUND: return "r\xC3\xA9seau introuvable";
    case AC_AUTH: return "mot de passe refus\xC3\xA9";
    case AC_TIMEOUT: return "d\xC3\xA9lai d\xC3\xA9pass\xC3\xA9";
    case AC_LOST: return "d\xC3\xA9" "connexion";
    default: return "refus\xC3\xA9 par la borne";
  }
}

void offlineCause(char * out, size_t n, int wifi, const char * ssid, int reason, bool timeOk, const FeedStat & st) {
  const char * s = ssid && *ssid ? ssid : "?";
  switch (wifi) {
    case 0: snprintf(out, n, "Wi-Fi non configur\xC3\xA9"); return;
    case 1:
      if (reason) snprintf(out, n, "Wi-Fi %s : connexion\xE2\x80\xA6 (refus : %s, code %d)", s, assocCauseText(assocCause(reason)), reason);
      else snprintf(out, n, "Wi-Fi %s : connexion\xE2\x80\xA6", s);
      return;
    case 3: snprintf(out, n, "Wi-Fi %s : %s (code %d)", s, assocCauseText(assocCause(reason)), reason); return;
    default: break;
  }
  if (!timeOk) snprintf(out, n, "Wi-Fi associ\xC3\xA9, heure NTP non synchronis\xC3\xA9" "e");
  else if (st.fails) snprintf(out, n, "station_status : \xC3\xA9" "chec %d (%s) \xC3\x97%u", st.errCode, ingestErrText(st.errCode), (unsigned)st.fails);
  else snprintf(out, n, "Wi-Fi associ\xC3\xA9, premier relev\xC3\xA9 en cours");
}

// ── Provenance ───────────────────────────────────────────────────────
// Les phrases ci-dessous décrivent Model::recompute (totaux) et wxScore (indice
// vélo) : test_core recalcule chaque somme indépendamment, station par station,
// pour vérifier que la règle écrite est bien celle qui est appliquée.
static void hhmm(char * out, size_t n, int64_t t) {
  struct tm lt;
  plat::localTime(t, &lt);
  snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

// Âge de la tranche de référence, en heures pleines. Après une coupure, « Réf. 04:55 » seul
// laissait croire à un maximum du matin même alors qu'il datait de la veille : l'âge le dit
// sans ambiguïté, y compris quand la référence reste dans la même journée.
static int ageHoursOf(int64_t t, int64_t now) {
  if (t <= 0 || now <= t) return 0;
  return (int)((now - t) / 3600);
}

bool provenance(const Model & m, int kpi, Provenance & p) {
  memset(&p, 0, sizeof(p));
  p.unit = "";
  p.count = -1;
  const Totals & t = m.tot;
  const bool gbfs = m.lastUpdate != 0;
  static const char * NO_STATUS = "aucun relev\xC3\xA9 station_status re\xC3\xA7u depuis le d\xC3\xA9marrage";
  // Chiffres tirés de station_status : même horodatage pour tous
  auto status = [&]() {
    p.feedAt = m.feedUpdated;
    p.recvAt = m.lastUpdate;
    p.available = gbfs;
    if (!gbfs) p.missing = NO_STATUS;
  };
  // Profondeur RÉELLEMENT observée : `histSlots` compte les lignes de l'anneau, trous compris —
  // l'afficher faisait dire « 1440 min » à une carte revenue après 12 h de coupure.
  const int histMin = t.histMinutes;
  switch (kpi) {
    case KPI_BIKES:
      p.name = "V\xC3\xA9los disponibles";
      p.kind = SRC_FEED; p.source = "GBFS"; p.feed = "station_status"; p.field = "num_vehicles_available";
      p.rule = "Somme du champ sur les stations o\xC3\xB9 is_installed et is_renting sont vrais ; "
               "les stations ferm\xC3\xA9" "es ne comptent pas.";
      status();
      p.value = (float)t.bikes;
      p.count = t.nRenting;
      snprintf(p.detail, sizeof(p.detail), "%d stations somm\xC3\xA9" "es sur %d publi\xC3\xA9" "es.", t.nRenting, t.stations);
      break;

    case KPI_DELTA1H:
      p.name = "Variation sur 1 h";
      p.kind = SRC_CALC; p.source = "calcul interne"; p.feed = "station_status";
      p.rule = "V\xC3\xA9los disponibles maintenant \xE2\x88\x92 v\xC3\xA9los disponibles dans la tranche de 5 min "
               "d'il y a 1 h (historique conserv\xC3\xA9 par la carte).";
      p.validity = "Exige le relev\xC3\xA9 d'il y a 1 h dans l'historique.";
      p.validityMet = t.deltaValid;
      status();
      if (gbfs && !t.deltaValid) { p.available = false; p.missing = "pas encore de relev\xC3\xA9 datant d'il y a 1 h"; }
      p.value = (float)t.delta1h;
      snprintf(p.detail, sizeof(p.detail), "Historique disponible : %d min.", histMin);
      break;

    case KPI_INUSE:
    case KPI_FLEET: {
      bool use = kpi == KPI_INUSE;
      p.name = use ? "V\xC3\xA9los en trajet" : "Parc estim\xC3\xA9";
      p.kind = SRC_CALC; p.source = "calcul interne"; p.feed = "station_status";
      p.estimate = true;
      p.rule = use ? "Parc estim\xC3\xA9 \xE2\x88\x92 (v\xC3\xA9los en station + v\xC3\xA9los HS) du relev\xC3\xA9 "
                     "courant. Ce sont les v\xC3\xA9los ABSENTS des bornes : en trajet, mais aussi "
                     "\xC3\xA0 l'atelier, dans la camionnette ou perdus."
                   : "Plus grand total (v\xC3\xA9los en station + v\xC3\xA9los HS) relev\xC3\xA9 dans "
                     "l'historique 5 min des derni\xC3\xA8res 24 h, sur les tranches o\xC3\xB9 autant de "
                     "stations r\xC3\xA9pondaient qu'aujourd'hui.";
      p.validity = "Valid\xC3\xA9 : au moins 6 h de tranches R\xC3\x89" "ELLEMENT OBSERV\xC3\x89" "ES et comparables "
                   "\xC3\xA0 la population courante (les coupures et les changements de couverture ne comptent pas), "
                   "la r\xC3\xA9f\xC3\xA9rence devant tomber dans le quart le moins actif de ces tranches, avant "
                   "comme apr\xC3\xA8s elle. Le parc est la SIXI\xC3\x88" "ME plus grande valeur comparable : jusqu'\xC3\xA0 "
                   "cinq relev\xC3\xA9s anormalement hauts (red\xC3\xA9marrage, cache, station fant\xC3\xB4me, deux "
                   "artefacts ind\xC3\xA9pendants) sont \xC3\xA9" "cart\xC3\xA9s au lieu de gonfler le chiffre. C'est une "
                   "ESTIMATION : une surestimation du parc la fait monter, une sous-estimation la fait "
                   "descendre ; sa provenance (\xC3\xA2ge de la r\xC3\xA9f\xC3\xA9rence, nombre de stations couvertes) "
                   "est \xC3\xA9crite \xC3\xA0 c\xC3\xB4t\xC3\xA9 du chiffre. Tant que les conditions ne sont pas r\xC3\xA9unies, "
                   "la valeur est marqu\xC3\xA9" "e provisoire ici, et le tableau de bord affiche un tiret.";
      p.validityMet = t.inUseValid;
      p.refAt = t.fleetAt;
      status();
      p.value = (float)(use ? t.inUse : t.fleet);
      // Estimation, pas un plancher : la revue a montré que deux artefacts indépendants surestiment
      // encore le parc (« ≥ 400 » pour 200 réels). On ne promet donc pas de borne.
      p.lowerBound = false;
      char hh[8];
      if (gbfs && !t.inUseValid && !t.inUseProvisional) {
        p.available = false;
        p.missing = "moins de 6 h de relev\xC3\xA9s : pas encore de quoi situer le parc";
      }
      if (t.fleetAt) {
        hhmm(hh, sizeof(hh), t.fleetAt);
        char age[16] = "";
        int ah = ageHoursOf(t.fleetAt, m.lastUpdate);
        if (ah >= 1) snprintf(age, sizeof(age), " (\xE2\x88\x92%d h)", ah);
        snprintf(p.detail, sizeof(p.detail), "R\xC3\xA9" "f. %s%s \xC2\xB7 hist. %dh%02d \xC2\xB7 %d st. couv.%s",
                 hh, age, t.histMinutes / 60, t.histMinutes % 60, t.refCover,
                 t.inUseValid ? "" : " \xC2\xB7 provisoire");
      } else {
        snprintf(p.detail, sizeof(p.detail), "Maximum = relev\xC3\xA9 actuel \xC2\xB7 hist. %dh%02d%s",
                 t.histMinutes / 60, t.histMinutes % 60,
                 t.inUseValid ? "" : " \xC2\xB7 provisoire");
      }
      break;
    }

    case KPI_DOCKS:
      p.name = "Bornes libres";
      p.kind = SRC_FEED; p.source = "GBFS"; p.feed = "station_status"; p.field = "num_docks_available";
      p.rule = "Somme du champ sur les stations o\xC3\xB9 is_installed et is_returning sont vrais ; "
               "les stations ferm\xC3\xA9" "es ne comptent pas.";
      status();
      p.value = (float)t.docks;
      p.count = t.nReturning;
      snprintf(p.detail, sizeof(p.detail), "%d stations somm\xC3\xA9" "es sur %d publi\xC3\xA9" "es.", t.nReturning, t.stations);
      break;

    case KPI_FILL:
      p.name = "Remplissage";
      p.kind = SRC_CALC; p.source = "calcul interne"; p.feed = "station_status";
      p.rule = "V\xC3\xA9los disponibles \xC3\xB7 (v\xC3\xA9los disponibles + bornes libres) \xC3\x97 100, "
               "sur les deux sommes ci-dessus.";
      p.isFloat = true; p.unit = "%";
      status();
      if (gbfs && t.bikes + t.docks == 0) { p.available = false; p.missing = "aucune station ouverte : division impossible"; }
      p.value = t.fillPct;
      snprintf(p.detail, sizeof(p.detail), "%d \xC3\xB7 (%d + %d).", t.bikes, t.bikes, t.docks);
      break;

    case KPI_BIKES_DIS:
    case KPI_DOCKS_DIS: {
      bool bk = kpi == KPI_BIKES_DIS;
      p.name = bk ? "V\xC3\xA9los hors service" : "Bornes hors service";
      p.kind = SRC_FEED; p.source = "GBFS"; p.feed = "station_status";
      p.field = bk ? "num_vehicles_disabled" : "num_docks_disabled";
      p.rule = "Somme du champ sur toutes les stations publi\xC3\xA9" "es, ouvertes ou ferm\xC3\xA9" "es.";
      status();
      p.value = (float)(bk ? t.bikesDis : t.docksDis);
      p.count = t.stations;
      snprintf(p.detail, sizeof(p.detail), "%d stations somm\xC3\xA9" "es.", t.stations);
      break;
    }

    case KPI_TEMP:
      p.name = "Temp\xC3\xA9rature";
      p.kind = SRC_FEED; p.source = "Open-Meteo"; p.feed = "api.open-meteo.com/v1/forecast";
      p.field = "current.temperature_2m";
      p.rule = "Valeur brute du champ (Luxembourg-Ville, 49,61 N 6,13 E), arrondie au degr\xC3\xA9 \xC3\xA0 l'affichage.";
      p.isFloat = true; p.unit = "\xC2\xB0" "C";
      p.available = m.wx.valid;
      if (!m.wx.valid) p.missing = "Open-Meteo n'a pas encore r\xC3\xA9pondu";
      p.value = m.wx.temp;
      p.feedAt = p.recvAt = m.wx.fetchedAt;
      if (m.wx.valid) {
        snprintf(p.detail, sizeof(p.detail), "Valeur brute : %.1f \xC2\xB0" "C.", m.wx.temp);
        for (char * c = p.detail; *c; c++) if (*c == '.' && c[1] >= '0' && c[1] <= '9') *c = ',';
      }
      break;

    case KPI_WXSCORE:
      p.name = "Indice v\xC3\xA9lo";
      p.kind = SRC_CALC; p.source = "calcul interne"; p.feed = "api.open-meteo.com/v1/forecast";
      p.rule = "10 \xE2\x88\x92 p\xC3\xA9nalit\xC3\xA9s : pluie (current.precipitation), vent > 20 km/h (current.wind_speed_10m), "
               "froid < 2 \xC2\xB0" "C, neige et orage (current.weather_code), aggravation sur +1\xE2\x80\xA6+3 h (hourly).";
      p.isFloat = true; p.unit = "/10";
      p.available = m.wx.valid;
      if (!m.wx.valid) p.missing = "Open-Meteo n'a pas encore r\xC3\xA9pondu";
      p.value = m.wx.score;
      p.feedAt = p.recvAt = m.wx.fetchedAt;
      if (m.wx.valid) snprintf(p.detail, sizeof(p.detail), "Facteur principal : %s.", m.wx.reason);
      break;

    case KPI_FC60:
      p.name = "Pr\xC3\xA9vision +1 h";
      p.kind = SRC_CALC; p.source = "calcul interne"; p.feed = "station_status";
      p.estimate = true;
      p.rule = "Somme des pr\xC3\xA9visions \xC3\xA0 +1 h des stations ouvertes en location ; par station : "
               "actuel + c\xC2\xB7(profil(t+1 h) \xE2\x88\x92 profil(t)) + part m\xC3\xA9t\xC3\xA9o + (1 \xE2\x88\x92 0,6c)\xC2\xB7tendance 30 min amortie. "
               "Part m\xC3\xA9t\xC3\xA9o : mouvement du profil \xC3\x97 \xE2\x88\x92" "coefficient \xC3\x97 (p\xC3\xA9nalit\xC3\xA9 m\xC3\xA9t\xC3\xA9o \xE2\x88\x92 sa normale \xC3\xA0 cette heure) "
               "si le temps est pire que la normale ; coefficient appris sur le parc, part born\xC3\xA9" "e \xC3\xA0 40 % de la capacit\xC3\xA9.";
      p.validity = "Fiable quand le profil appris couvre ce cr\xC3\xA9neau : confiance moyenne \xE2\x89\xA5 50 %.";
      p.validityMet = m.netConf >= 50;
      status();
      p.value = (float)lroundf(m.netFc[12]);
      p.count = t.nRenting;
      {
        float wxSum = 0;
        for (int i = 0; i < m.n; i++) if (m.st[i].active && m.st[i].renting && m.st[i].status != ST_CLOSED) wxSum += m.st[i].fcWx[12];
        const char * org = m.wxUse == WXU_NONE ? "indisponible" : m.wxUse == WXU_STALE ? "p\xC3\xA9rim\xC3\xA9" "e"
                         : m.wxSrc == WXC_DEFAULT ? "pas encore apprise" : m.wxSrc == WXC_NULL ? "sans effet mesurable" : "apprise";
        snprintf(p.detail, sizeof(p.detail), "Confiance %d %% \xC2\xB7 profil appris \xC3\xA0 %.0f %% \xC2\xB7 m\xC3\xA9t\xC3\xA9o %s : %+d v\xC3\xA9los.",
                 m.netConf, m.learnPct(), org, (int)lroundf(wxSum));
      }
      break;

    default:
      return false;
  }
  return true;
}

// ── Part météo d'une prévision ───────────────────────────────────────
static void commaDec(char * s) { for (; *s; s++) if (*s == '.' && s[1] >= '0' && s[1] <= '9') *s = ','; }

void wxOriginText(const Model & m, char * out, size_t n) {
  if (m.wxUse == WXU_NONE)
    snprintf(out, n, "m\xC3\xA9t\xC3\xA9o indisponible : pr\xC3\xA9vision sans m\xC3\xA9t\xC3\xA9o");
  else if (m.wxUse == WXU_STALE)
    snprintf(out, n, "m\xC3\xA9t\xC3\xA9o de plus de 45 min : pr\xC3\xA9vision sans m\xC3\xA9t\xC3\xA9o");
  else if (m.wxSrc == WXC_DEFAULT)
    snprintf(out, n, "effet de la m\xC3\xA9t\xC3\xA9o pas encore appris (trop peu de pluie observ\xC3\xA9" "e) : aucune correction");
  else if (m.wxSrc == WXC_NULL)
    snprintf(out, n, "effet de la m\xC3\xA9t\xC3\xA9o non mesurable sur ce parc : aucune correction");
  else {
    snprintf(out, n, "coefficient appris %.2f \xC2\xB1 %.2f (%.0f fen\xC3\xAAtres pluvieuses)", m.wxAlpha, m.wxAlphaSE, m.wxG.nRain);
    commaDec(out);
  }
}

void wxPartText(const Model & m, float part, bool forecast, int64_t at, char * out, size_t n) {
  char org[96], hh[8];
  wxOriginText(m, org, sizeof(org));
  hhmm(hh, sizeof(hh), at);
  int v = (int)lroundf(part);
  if (m.wxUse == WXU_OK && m.wxSrc == WXC_LEARNED && v != 0) {
    // la raison de la note (« Pluie 1,4 mm/h ») décrit la météo ACTUELLE : citée seulement pour elle
    char why[72] = "";
    if (!forecast) { snprintf(why, sizeof(why), " (%s)", m.wx.reason); commaDec(why); }
    snprintf(out, n, "M\xC3\xA9t\xC3\xA9o : dont %+d v\xC3\xA9lo%s \xC3\xA0 %s li\xC3\xA9%s \xC3\xA0 %s%s \xC2\xB7 %s",
             v, abs(v) > 1 ? "s" : "", hh, abs(v) > 1 ? "s" : "",
             forecast ? "la m\xC3\xA9t\xC3\xA9o annonc\xC3\xA9" "e" : "la m\xC3\xA9t\xC3\xA9o actuelle", why, org);
  } else if (m.wxUse == WXU_OK && m.wxSrc == WXC_LEARNED) {
    snprintf(out, n, "M\xC3\xA9t\xC3\xA9o : aucun effet notable \xC3\xA0 %s (temps normal pour l'heure ou pas de mouvement attendu) \xC2\xB7 %s", hh, org);
  } else {
    snprintf(out, n, "M\xC3\xA9t\xC3\xA9o : %s", org);
  }
}

// ── Écart au profil (« inhabituel ») ─────────────────────────────────
static const char * plural(int v) { return abs(v) > 1 ? "s" : ""; }

// L'écart habituel mesuré est donné pour information (il ne décide de rien) :
// « · écart habituel à cette heure ± 2,1, mesuré sur 34 jours (pour information) »
static void dispInfo(const Station & s, char * out, size_t n) {
  out[0] = 0;
  if (!s.anomDispOk) return;
  char days[40];
  if (s.anomDispDays >= 56) snprintf(days, sizeof(days), "sur plus de 8 semaines");
  else if (s.anomDispDays >= 1) snprintf(days, sizeof(days), "sur %d jour%s", s.anomDispDays, s.anomDispDays > 1 ? "s" : "");
  else snprintf(days, sizeof(days), "depuis moins d'un jour");
  snprintf(out, n, " \xC2\xB7 \xC3\xA9" "cart habituel \xC3\xA0 cette heure \xC2\xB1 %.1f, mesur\xC3\xA9 %s (pour information)",
           s.anomDisp10 / 10.0, days);
  commaDec(out);
}

void anomLineText(const Station & s, char * out, size_t n) {
  char info[112];
  dispInfo(s, info, sizeof(info));
  switch (s.anom) {
    case AN_HIGH: case AN_LOW:                   // la règle de la v1.0.8 (+ l'écart habituel, pour information)
      snprintf(out, n, "Inhabituel : %d v\xC3\xA9lo%s contre %d attendu%s (\xC2\xB1 %d) \xC2\xB7 confiance du profil %d %%%s",
               s.bikes, plural(s.bikes), s.anomExp, plural(s.anomExp), s.anomTol, s.anomConf, info);
      break;
    case AN_NORMAL:
      snprintf(out, n, "Habituel : %d v\xC3\xA9lo%s contre %d attendu%s (\xC2\xB1 %d) \xC2\xB7 confiance du profil %d %%%s",
               s.bikes, plural(s.bikes), s.anomExp, plural(s.anomExp), s.anomTol, s.anomConf, info);
      break;
    case AN_YOUNG:
      snprintf(out, n, "\xC3\x89" "cart au profil non \xC3\xA9valu\xC3\xA9 : profil trop jeune (confiance %d %%, 50 %% requis)", s.anomConf);
      break;
    case AN_NO_PROFILE:
      snprintf(out, n, "\xC3\x89" "cart au profil non \xC3\xA9valu\xC3\xA9 : aucun profil appris pour ce cr\xC3\xA9neau");
      break;
    case AN_OFF_HOURS:
      snprintf(out, n, "\xC3\x89" "cart au profil non \xC3\xA9valu\xC3\xA9 hors 7 h\xE2\x80\x93" "22 h");
      break;
    default:
      snprintf(out, n, "\xC3\x89" "cart au profil non \xC3\xA9valu\xC3\xA9 : station ferm\xC3\xA9" "e");
  }
}

void anomAlertText(const Station & s, char * out, size_t n) {
  snprintf(out, n, "Inhabituel : %d v\xC3\xA9lo%s contre %d attendu%s (\xC2\xB1 %d)",
           s.bikes, plural(s.bikes), s.anomExp, plural(s.anomExp), s.anomTol);
}

} // namespace vn
