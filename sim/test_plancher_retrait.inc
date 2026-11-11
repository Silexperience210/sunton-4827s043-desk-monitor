// ── Plancher de retrait : le soir, on ne descend pas sous le pic du matin ──────────────────
// Ce que ce test protège, et pourquoi la règle a changé :
//  1. le plancher était lu au profil de DEMAIN 8 h, sur les seules stations `sens > 0`. Or à 8 h
//     la station est DÉJÀ entamée par la première heure de départs : mesuré sur 6 jours ouvrés
//     réels (3→10/11/2026), Gare Centrale 2 perd une médiane de 12 vélos entre 6 h et 9 h et son
//     niveau de 8 h vaut ≈ 10 sur 21 — un plancher à 8 h laisse donc repartir des vélos qui
//     manqueront avant 9 h. L'ancre devient 6 h, celle du motif pendulaire (`sensJour`) ;
//  2. le plancher ne s'appliquait PAS quand le besoin calculé était un retrait (`give = -need`
//     court-circuitait `keep`) : une station étiquetée « surplus » était vidée vers la cible 50 %
//     sans aucune référence au lendemain. Il s'applique maintenant à TOUS les prélèvements, et
//     aussi au retrait AFFICHÉ (`need`), sinon la fiche conseille plus que le planificateur n'ose.
static void testPlancherRetrait() {
  settingsDefaults(g_set);
  const int64_t M0 = lux(2027, 4, 6, 0, 0);        // mardi 6 avril 2027, hors férié
  InfoRec ir[3] = { mkInfo("1", 49.6f, 6.13f, 30), mkInfo("2", 49.61f, 6.13f, 20), mkInfo("3", 49.62f, 6.13f, 20) };

  // Trois semaines de relevés toutes les 5 min.
  //   1 : 24 vélos jusqu'à 7 h, 6 ensuite, 24 dès 17 h puis 28 à partir de 20 h
  //       → se vide le matin (sens +1) ; le pic du matin vaut 24, son niveau de 8 h vaut 6 :
  //         c'est ce grand écart qui distingue un plancher posé à 6 h d'un plancher posé à 8 h.
  //   2 : 18 vélos à 6 h puis 1 toute la journée → arrivées au travail, manque le soir.
  //   3 : 10 vélos en permanence → aucun motif.
  StatusRec sr[3];
  auto motif = [](int st, int h) -> int {
    if (st == 0) return (h < 7) ? 24 : (h < 17 ? 6 : (h < 20 ? 24 : 28));
    if (st == 1) return (h < 7) ? 18 : 1;
    return 10;
  };
  static Model m;
  m = Model(); m.init();
  m.applyInfo(ir, 3, M0);
  for (int w = 0; w < 3; w++)
    for (int k = 0; k < 24 * 12; k++) {
      const int64_t t = M0 + (int64_t)w * 7 * 86400 + (int64_t)k * 300;
      const int h = (k * 5) / 60;
      sr[0] = mkStat("1", motif(0, h), 30); sr[1] = mkStat("2", motif(1, h), 20); sr[2] = mkStat("3", motif(2, h), 20);
      m.applyStatus(sr, 3, t);
    }
  const int64_t fin = M0 + 2 * 7 * 86400 + (24 * 12 - 1) * 300;
  m.recompute(fin);
  CHECK(m.st[0].sens > 0, "plancher : station 1 lue pendulaire (%d)", m.st[0].sens);

  // ── Le plancher est lu au PIC DU MATIN (6 h), pas à 8 h ──────────────────────────────
  const float pl = m.plancherDemain(0, fin);
  printf("  plancher demain 6 h : %.1f vélos (attendu 24 : 60 %% de 30 = 18, profil 6 h = 24)\n", pl);
  CHECK(pl >= 22.f && pl <= 26.f,
        "plancher : lu au pic du matin (%.1f, attendu ≈ 24 ; 8 h aurait donné ≈ 18)", pl);
  CHECK(pl > m.st[0].capEff() * 0.6f + 2.f,
        "plancher : le profil de 6 h prime sur les 60 %% de capacité (%.1f > %.1f)",
        pl, m.st[0].capEff() * 0.6f);
  // Une station sans motif lisible n'a que les 60 % de capacité.
  CHECK(fabsf(m.plancherDemain(2, fin) - m.st[2].capEff() * 0.6f) < 0.01f,
        "plancher : sans pic appris, 60 %% de capacité seulement (%.1f)", m.plancherDemain(2, fin));

  // ── Soirée du dernier mardi, 20 h 30 : la station 1 est pleine (27/30) ───────────────
  static Model g;
  g = Model(); g.init();
  g.applyInfo(ir, 3, M0);
  const int64_t soir = M0 + 2 * 7 * 86400 + 20 * 3600 + 1800;
  for (int w = 0; w < 3; w++)
    for (int k = 0; k < 24 * 12; k++) {
      const int64_t t = M0 + (int64_t)w * 7 * 86400 + (int64_t)k * 300;
      if (t > soir) break;
      const int h = (k * 5) / 60;
      sr[0] = mkStat("1", motif(0, h), 30); sr[1] = mkStat("2", motif(1, h), 20); sr[2] = mkStat("3", motif(2, h), 20);
      g.applyStatus(sr, 3, t);
    }
  g.recompute(soir);
  const float plancher = g.plancherDemain(0, soir);
  const int brut = (int)lroundf((float)g.st[0].capEff() * 0.5f - g.st[0].fc[12]);
  const int plafonne = -((-brut + 1) / 2);          // ce que disait l'ancienne règle (moitié)
  const int laisse = (int)lroundf((float)g.st[0].bikes - plancher);
  printf("  20:30  st1 %d vélos (statut %d), formule nue %+d, moitié %+d, plancher laisse partir %d, conseil %+d\n",
         g.st[0].bikes, g.st[0].status, brut, plafonne, laisse, g.st[0].need);
  CHECK(brut < -6 && plafonne < 0, "plancher : la station est bien saturée le soir (brut %d)", brut);
  CHECK(g.st[0].need < 0, "plancher : le conseil reste un RETRAIT (%d)", g.st[0].need);
  CHECK(abs((int)g.st[0].need) <= (laisse > 0 ? laisse : 0),
        "plancher : le retrait AFFICHÉ ne descend pas sous demain matin (%d ≤ %d)",
        abs((int)g.st[0].need), laisse);
  CHECK(abs((int)g.st[0].need) < abs(plafonne),
        "plancher : plus serré que l'ancienne moitié (%d contre %d)", g.st[0].need, plafonne);

  // Le planificateur de transferts prélève sur la MÊME borne (la station 2 manque le soir).
  CHECK(g.st[1].need > 0, "plancher : la station 2 réclame un apport (%d)", g.st[1].need);
  CHECK(g.nTr >= 1 && g.tr[0].from == 0, "plancher : transfert depuis la station 1 (%d transferts)", g.nTr);
  if (g.nTr >= 1) {
    printf("  transfert : st1 → st2 de %d vélos (plancher laisse %d)\n", g.tr[0].n, laisse);
    CHECK((int)g.tr[0].n <= laisse,
          "plancher : le transfert ne prélève pas sous demain matin (%d ≤ %d)", g.tr[0].n, laisse);
  }

  // ── Dimanche soir : aucun motif ce jour-là, mais demain matin est un pic ─────────────
  // `sensJour` rend 0 le week-end, donc l'ancienne règle n'appliquait aucun plancher ; le
  // plancher lit DEMAIN (lundi) et protège quand même la station.
  static Model d;
  d = Model(); d.init();
  d.applyInfo(ir, 3, M0);
  const int64_t dim = lux(2027, 4, 25, 20, 30);     // dimanche soir, lundi 26 au matin
  for (int w = 0; w < 3; w++)
    for (int k = 0; k < 24 * 12; k++) {
      const int64_t t = M0 + (int64_t)w * 7 * 86400 + (int64_t)k * 300;
      if (t > dim) break;
      const int h = (k * 5) / 60;
      sr[0] = mkStat("1", motif(0, h), 30); sr[1] = mkStat("2", motif(1, h), 20); sr[2] = mkStat("3", motif(2, h), 20);
      d.applyStatus(sr, 3, t);
    }
  d.recompute(dim);
  const int laisseDim = (int)lroundf((float)d.st[0].bikes - d.plancherDemain(0, dim));
  printf("  dimanche 20:30  st1 %d vélos, motif %d, plancher laisse %d, conseil %+d\n",
         d.st[0].bikes, d.st[0].sens, laisseDim, d.st[0].need);
  CHECK(d.st[0].sens == 0, "plancher : pas de motif pendulaire un dimanche (%d)", d.st[0].sens);
  CHECK(d.plancherDemain(0, dim) >= 22.f,
        "plancher : le dimanche lit le pic du LUNDI (%.1f)", d.plancherDemain(0, dim));
  CHECK(abs((int)d.st[0].need) <= (laisseDim > 0 ? laisseDim : 0),
        "plancher : le dimanche soir ne vide pas la station (%d ≤ %d)", abs((int)d.st[0].need), laisseDim);
}
