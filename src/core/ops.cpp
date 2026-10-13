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

// ── Provenance ───────────────────────────────────────────────────────
// Les phrases ci-dessous décrivent Model::recompute (totaux) et wxScore (indice
// vélo) : test_core recalcule chaque somme indépendamment, station par station,
// pour vérifier que la règle écrite est bien celle qui est appliquée.
static void hhmm(char * out, size_t n, int64_t t) {
  struct tm lt;
  plat::localTime(t, &lt);
  snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
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
  const int histMin = t.histSlots * VN_RING_STEP_S / 60;
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
      p.rule = use ? "Parc estim\xC3\xA9 \xE2\x88\x92 (v\xC3\xA9los en station + v\xC3\xA9los HS) du dernier relev\xC3\xA9 : "
                     "num_vehicles_available + num_vehicles_disabled, toutes stations."
                   : "Plus grand total (v\xC3\xA9los en station + v\xC3\xA9los HS) relev\xC3\xA9 dans l'historique "
                     "5 min des derni\xC3\xA8res 24 h.";
      p.validity = "Estimation valable apr\xC3\xA8s un creux de nuit observ\xC3\xA9 : maximum relev\xC3\xA9 entre "
                   "01 h et 05 h, avec au moins 6 h d'historique.";
      p.validityMet = t.inUseValid;
      p.refAt = t.fleetAt;
      status();
      if (gbfs && !t.inUseValid) { p.available = false; p.missing = "aucun creux de nuit observ\xC3\xA9 : l'estimation serait fausse"; }
      p.value = (float)(use ? t.inUse : t.fleet);
      char hh[8];
      if (t.fleetAt) {
        hhmm(hh, sizeof(hh), t.fleetAt);
        snprintf(p.detail, sizeof(p.detail), "Maximum retenu \xC3\xA0 %s \xC2\xB7 historique %d h %02d (6 h requises).",
                 hh, histMin / 60, histMin % 60);
      } else {
        snprintf(p.detail, sizeof(p.detail), "Le maximum est le relev\xC3\xA9 actuel \xC2\xB7 historique %d h %02d (6 h requises).",
                 histMin / 60, histMin % 60);
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
               "actuel + c\xC2\xB7(profil(t+1 h) \xE2\x88\x92 profil(t)) + (1 \xE2\x88\x92 0,6c)\xC2\xB7tendance 30 min amortie.";
      p.validity = "Fiable quand le profil appris couvre ce cr\xC3\xA9neau : confiance moyenne \xE2\x89\xA5 50 %.";
      p.validityMet = m.netConf >= 50;
      status();
      p.value = (float)lroundf(m.netFc[12]);
      p.count = t.nRenting;
      snprintf(p.detail, sizeof(p.detail), "Confiance moyenne %d %% \xC2\xB7 profil hebdomadaire appris \xC3\xA0 %.0f %%.",
               m.netConf, m.learnPct());
      break;

    default:
      return false;
  }
  return true;
}

} // namespace vn
