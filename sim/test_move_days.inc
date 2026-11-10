// ── Mouvements par jour : agrégat des échelles 1 semaine / 1 mois ────────────────
// Les barres « ACTIVITÉ · MOUVEMENTS / HEURE » tracent, heure par heure, les départs +
// retours relevés sur l'anneau 24 h. Pour tracer LA MÊME grandeur à 1 semaine et à 1 mois,
// chaque tranche relevée verse son compte de mouvements dans le résumé du jour
// (mvSum / mvN). Ces vérifications portent sur quatre points qui, s'ils cassent, donnent
// un graphe faux sans que rien ne plante :
//   1. le débit horaire d'un créneau et le total du jour ;
//   2. « aucune donnée » (mvN = 0) distinct de « zéro mouvement » ;
//   3. la persistance conserve l'agrégat ;
//   4. un fichier écrit par une version ANTÉRIEURE (DayNet de 204 octets, sans ces champs)
//      se relit et donne « aucune donnée » — surtout pas des barres plates à zéro.
static void testMoveDays() {
  settingsDefaults(g_set);
  static Model m;
  CHECK(m.init(), "mv : init");

  const int64_t T0 = 1791842400;   // mardi 13/10/2026 00:00 heure de Luxembourg
  InfoRec ir[2] = { mkInfo("1", 49.60f, 6.13f, 40), mkInfo("2", 49.61f, 6.14f, 40) };
  m.applyInfo(ir, 2, T0);
  CHECK(m.n == 2, "mv : 2 stations");

  // 07:00-10:00 : une navette entre les deux stations à chaque tranche (40 vélos passent de
  // l'une à l'autre) → 40 mouvements par tranche, donc 480 par heure. Les créneaux suivants
  // ne sont pas relevés : c'est le cas « aucune donnée » qu'on veut distinguer d'un zéro.
  StatusRec sr[2];
  int attendu = 0;
  for (int64_t t = T0 + 7 * 3600; t < T0 + 10 * 3600; t += 300) {
    const bool pair = ((t - T0) / 300) % 2 == 0;
    sr[0] = mkStat(ir[0].sid, pair ? 0 : 40, 40);
    sr[1] = mkStat(ir[1].sid, pair ? 40 : 0, 40);
    m.applyStatus(sr, 2, t);
    attendu += 40;
  }

  // Attendu calculé depuis l'ANNEAU, pas deviné : on additionne exactement les mouvements des
  // tranches de 7 h que le résumé du jour doit avoir accumulés. Comparer à une constante écrite
  // à la main avait produit une attente fausse (880 au lieu du total réel).
  double attendu7h = 0;
  int tranches7h = 0;
  for (int bk = 0; bk < m.count; bk++) {
    const int idx = m.ringIndex(bk);
    if (idx < 0) continue;
    const vn::NetSample & ns = m.net[idx];
    struct tm lt;
    plat::localTime(ns.t, &lt);
    if (lt.tm_hour != 7) continue;
    attendu7h += (double)ns.dep + (double)ns.ret;
    tranches7h++;
  }

  const DayNet * d0 = m.days.dayAt(0);
  CHECK(d0 != nullptr, "mv : le jour du relevé est enregistré");
  if (d0) {
    // 12 tranches par créneau, 40 mouvements par tranche (après la première, qui n'a pas de
    // prédécesseur immédiat… le débit est donc au plus 480/h et au moins 440/h).
    CHECK(d0->mvN[0] == 12, "mv : 12 tranches comptées à 07 h (vu %d)", d0->mvN[0]);
    CHECK(tranches7h > 0, "mv : l'anneau porte des tranches à 7 h (%d)", tranches7h);
    CHECK(d0->mvSum[0] == (uint16_t)attendu7h,
          "mv : créneau 0 = %d mouvements, anneau = %.0f", d0->mvSum[0], attendu7h);

    float debit = -1;
    CHECK(dayMovesSlot(*d0, 0, debit) && debit > 0.f,
          "mv : débit horaire du créneau 0 = %.0f mvt/h", debit);
    // Le débit est le total ramené à l'heure : moyenne par tranche × 12.
    const float attenduDebit = (float)(attendu7h / (double)tranches7h * 12.0);
    CHECK(fabsf(debit - attenduDebit) < 1.f,
          "mv : débit horaire = %.0f (attendu %.0f)", debit, attenduDebit);

    float total = -1;
    CHECK(dayMovesTotal(*d0, total) && total > 0.f, "mv : total du jour = %.0f mouvements", total);

    // Créneau sans relevé (10 h et au-delà) : aucune donnée, pas un zéro.
    CHECK(d0->mvN[3] == 0, "mv : créneau 10 h non relevé (mvN %d)", d0->mvN[3]);
    float vide = -1;
    CHECK(!dayMovesSlot(*d0, 3, vide), "mv : un créneau sans relevé n'a pas de débit");
    CHECK(!dayMovesSlot(*d0, DAY_NH - 1, vide), "mv : 21 h non relevé non plus");
  }

  // Persistance : l'agrégat doit traverser un enregistrement / relecture.
  Model::Blob blob;
  CHECK(m.serialize(blob), "mv : sérialisation");
  CHECK(blob.days != nullptr && blob.daysLen > 0, "mv : bloc de jours présent (%zu o)", blob.daysLen);
  if (blob.days) {
    static Model m2;
    CHECK(m2.init(), "mv : init du second modèle");
    const int16_t map[2] = { 0, 1 };
    CHECK(m2.days.load(blob.days, blob.daysLen, map, 2, T0 + 86400), "mv : relecture du fichier de jours");
    const DayNet * r0 = m2.days.dayAt(0);
    CHECK(r0 != nullptr, "mv : jour relu");
    if (r0) {
      CHECK(r0->mvN[0] == 12 && r0->mvSum[0] == d0->mvSum[0],
            "mv : agrégat conservé (n %d, somme %d)", r0->mvN[0], r0->mvSum[0]);
      float total = -1;
      CHECK(dayMovesTotal(*r0, total) && total > 0.f, "mv : total relu = %.0f mouvements", total);
    }

    // ── Compatibilité : fichier d'AVANT l'extension (DayNet 204 o) ──
    DaysFileHdr h;
    memcpy(&h, blob.days, sizeof(h));
    const size_t o0 = sizeof(DaysFileHdr) + (size_t)h.nSt * 8;
    const size_t recNew = h.netSz + (size_t)h.nSt * h.stSz;
    const size_t tail = (size_t)h.nSt * (DAY_HC + 1);
    std::vector<uint8_t> vieux;
    vieux.insert(vieux.end(), (uint8_t *)&h, (uint8_t *)&h + sizeof(h));
    vieux.insert(vieux.end(), blob.days + sizeof(h), blob.days + o0);
    for (int k = 0; k < h.nDays; k++) {
      const uint8_t * rec = blob.days + o0 + (size_t)k * recNew;
      vieux.insert(vieux.end(), rec, rec + 204);                        // DayNet tronqué à l'ancien format
      vieux.insert(vieux.end(), rec + h.netSz, rec + h.netSz + (size_t)h.nSt * h.stSz);
    }
    vieux.insert(vieux.end(), blob.days + blob.daysLen - tail, blob.days + blob.daysLen);
    DaysFileHdr hv;
    memcpy(&hv, vieux.data(), sizeof(hv));
    hv.netSz = 204;
    memcpy(vieux.data(), &hv, sizeof(hv));

    static Model m3;
    CHECK(m3.init(), "mv : init du troisième modèle");
    const bool ok = m3.days.load(vieux.data(), vieux.size(), map, 2, T0 + 86400);
    CHECK(ok, "mv : un fichier d'avant l'extension reste lisible (%zu o)", vieux.size());
    if (ok) {
      const DayNet * v0 = m3.days.dayAt(0);
      CHECK(v0 != nullptr, "mv : jour ancien relu");
      if (v0) {
        float total = -1;
        CHECK(v0->mvN[0] == 0, "mv : ancien fichier → mvN = 0 (aucune donnée), vu %d", v0->mvN[0]);
        CHECK(!dayMovesTotal(*v0, total), "mv : ancien fichier → aucun total inventé");
        CHECK(v0->obs[0] > 0, "mv : les compteurs de disponibilité de l'ancien fichier sont intacts");
      }
    }
  }
  if (blob.prof) plat::psFree(blob.prof);
  if (blob.hist) plat::psFree(blob.hist);
  if (blob.days) plat::psFree(blob.days);
}
