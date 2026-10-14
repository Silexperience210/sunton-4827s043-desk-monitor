// ── Recherche Wi-Fi : construction de la liste (voir wifiscan.h) ──────
#include "wifiscan.h"
#include <string.h>

namespace vn {

void buildWifiList(const WifiRaw * raw, int nRaw, WifiList & out) {
  out.n = out.hidden = out.dropped = 0;
  // Liste de travail sans plafond arbitraire : au plus une entrée par borne.
  // Allouée sur le tas, pas sur la pile (la tâche réseau a une pile de 16 Ko).
  if (nRaw <= 0) return;
  WifiNet * all = new WifiNet[nRaw];
  int k = 0;
  for (int i = 0; i < nRaw; i++) {
    const WifiRaw & r = raw[i];
    if (!r.ssid[0]) { out.hidden++; continue; }
    int j = 0;
    while (j < k && strncmp(all[j].ssid, r.ssid, sizeof(all[j].ssid)) != 0) j++;
    if (j < k) {                                   // même nom : on garde la borne la plus forte
      if (all[j].aps < 255) all[j].aps++;
      if (r.rssi > all[j].rssi) { all[j].rssi = r.rssi; all[j].channel = r.channel; all[j].secure = r.secure; }
      continue;
    }
    WifiNet & w = all[k++];
    strncpy(w.ssid, r.ssid, sizeof(w.ssid) - 1);
    w.ssid[sizeof(w.ssid) - 1] = 0;
    w.rssi = r.rssi;
    w.channel = r.channel;
    w.secure = r.secure;
    w.aps = 1;
  }
  // Tri par insertion, puissance décroissante (stable : à RSSI égal, ordre de la radio)
  for (int a = 1; a < k; a++) {
    WifiNet v = all[a]; int b = a - 1;
    while (b >= 0 && all[b].rssi < v.rssi) { all[b + 1] = all[b]; b--; }
    all[b + 1] = v;
  }
  // Troncature APRÈS le tri : seuls les plus faibles peuvent tomber
  out.n = k < VN_SCAN_MAX ? k : VN_SCAN_MAX;
  out.dropped = k - out.n;
  memcpy(out.net, all, sizeof(WifiNet) * out.n);
  delete[] all;
}

ScanStep scanVerdict(int code, int attempt) {
  if (code >= 0) return SCAN_OK;
  if (code == -1 && attempt + 1 < VN_SCAN_TRIES) return SCAN_RETRY;
  return SCAN_FAIL;
}

} // namespace vn
