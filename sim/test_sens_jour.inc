// ── Motif pendulaire et garde de l'aller-retour ───────────────────────────────────────────
// Ce que ce test protège : une station qui se vide le matin et se remplit le soir ne doit pas
// être vidée « parce qu'elle est pleine ce soir ». C'est une règle de terrain, mesurée sur
// 12 jours ouvrés réels : 69 stations sur 139 s'inversent (Gare Centrale 2 : −94 le matin,
// +53 le soir, 12 jours sur 12). Sans le garde-fou, le conseil de 18 h retire la moitié de la
// capacité et prive les départs du lendemain.
static void testSensJour() {
  settingsDefaults(g_set);
  InfoRec ir[3] = { mkInfo("1", 49.6f, 6.13f, 30), mkInfo("2", 49.6f, 6.13f, 30), mkInfo("3", 49.6f, 6.13f, 30) };
  const int64_t M0 = lux(2027, 4, 6, 0, 0);        // mardi 6 avril 2027, hors férié
  static Model m;
  m = Model(); m.init();
  m.applyInfo(ir, 3, M0);

  // Trois semaines de relevés toutes les 5 min, trois motifs de journée :
  //   1 : 16 à 6 h, 7 ensuite, 25 le soir → on prend le matin, on rapporte le soir  → sens +1
  //   2 : 4 à 6 h, 22 ensuite, 3 le soir  → arrivées au travail, départs le soir     → sens −1
  //   3 : 15 vélos toute la journée                                                  → sens 0
  StatusRec sr[3];
  for (int w = 0; w < 3; w++)
    for (int k = 0; k < 24 * 12; k++) {
      const int64_t t = M0 + (int64_t)w * 7 * 86400 + (int64_t)k * 300;
      const int h = (k * 5) / 60;
      // Motif RÉEL mesuré sur le réseau (Gare Centrale 2 : 16 vélos à 6 h, 7 vers 9 h, 25 le
      // soir ; une station d'arrivées au travail fait exactement l'inverse).
      const int b1 = (h < 7) ? 16 : (h < 17 ? 7  : 25);
      const int b2 = (h < 7) ? 4  : (h < 17 ? 22 : 3);
      sr[0] = mkStat("1", b1, 30); sr[1] = mkStat("2", b2, 30); sr[2] = mkStat("3", 15, 30);
      m.applyStatus(sr, 3, t);
    }

  // Au dernier relevé (mardi, 23 h 55), la journée est terminée : le motif doit être lisible.
  const int64_t fin = M0 + 2 * 7 * 86400 + (24 * 12 - 1) * 300;
  m.recompute(fin);
  CHECK(m.st[0].sens > 0, "sens : station 1 (vide le matin, pleine le soir) → %d", m.st[0].sens);
  CHECK(m.st[1].sens < 0, "sens : station 2 (pleine le matin, vide le soir) → %d", m.st[1].sens);
  CHECK(m.st[2].sens == 0, "sens : station 3 (plate) → aucun motif (%d)", m.st[2].sens);

  // ── Le garde-fou : soirée du dernier mardi, 18 h 30 ────────────────────────────────────
  // Modèle séparé, nourri jusqu'à 18 h 30 INCLUS : les relevés doivent être monotones, sinon
  // applyStatus ignore un état plus ancien que le dernier appris. À 18 h 30 la station 1 est à
  // 25 vélos sur 30 (elle se remplit le soir) et la station 2 à 3 (elle se vide le soir).
  static Model g;
  g = Model(); g.init();
  g.applyInfo(ir, 3, M0);
  const int64_t soir = M0 + 2 * 7 * 86400 + 18 * 3600 + 1740;
  for (int w = 0; w < 3; w++)
    for (int k = 0; k < 24 * 12; k++) {
      const int64_t t = M0 + (int64_t)w * 7 * 86400 + (int64_t)k * 300;
      if (t > soir) break;
      const int h = (k * 5) / 60;
      const int b1 = (h < 7) ? 16 : (h < 17 ? 7  : 25);
      const int b2 = (h < 7) ? 4  : (h < 17 ? 22 : 3);
      sr[0] = mkStat("1", b1, 30 - b1); sr[1] = mkStat("2", b2, 30 - b2); sr[2] = mkStat("3", 15, 15);
      g.applyStatus(sr, 3, t);
    }
  // La station 2 tombe à ZÉRO à 18 h 30 : c'est là qu'elle s'écarte de son niveau habituel du
  // soir (3 vélos = « équilibrée » pour elle, donc sans conseil). Un relevé de plus, plus récent.
  sr[0] = mkStat("1", 25, 5); sr[1] = mkStat("2", 0, 30); sr[2] = mkStat("3", 15, 15);
  g.applyStatus(sr, 3, soir + 60);
  g.recompute(soir + 60);
  CHECK(g.st[0].sens > 0 && g.st[1].sens < 0,
        "garde-fou : motifs relus au soir (1 → %d, 2 → %d)", g.st[0].sens, g.st[1].sens);

  const float capE = (float)g.st[0].capEff();
  const int brut = (int)lroundf(capE * 0.5f - g.st[0].fc[12]);   // ce que dirait la formule nue
  CHECK(brut < -2, "garde-fou : la station 1 est bien saturée ce soir (formule nue = %d)", brut);
  CHECK(g.st[0].need < 0, "garde-fou : le conseil reste un RETRAIT (%d)", g.st[0].need);
  CHECK(abs((int)g.st[0].need) < abs(brut),
        "garde-fou : retrait plafonné (%d annoncés au lieu de %d)", g.st[0].need, brut);
  CHECK(abs((int)g.st[0].need) >= abs(brut) / 2 - 1,
        "garde-fou : le retrait n'est pas supprimé pour autant (%d pour %d)", g.st[0].need, brut);

  // La station au motif INVERSE (2) n'est pas concernée : elle manque de vélos le soir, le
  // conseil doit rester « apporter » et ne pas être réduit.
  CHECK(g.st[1].need > 0, "garde-fou : la station 2 doit rester « apporter » (%d)", g.st[1].need);

  // ── Le matin, le plafond ne s'applique PAS ────────────────────────────────────────────
  // Une station pendulaire pleine à 8 h doit se vider : c'est l'heure où les gens partent.
  // Le conseil de retrait y est donc rendu tel quel. (Modèle nourri jusqu'à 8 h seulement.)
  static Model h;
  h = Model(); h.init();
  h.applyInfo(ir, 3, M0);
  const int64_t matin = M0 + 2 * 7 * 86400 + 8 * 3600;
  for (int w = 0; w < 3; w++)
    for (int k = 0; k < 24 * 12; k++) {
      const int64_t t2 = M0 + (int64_t)w * 7 * 86400 + (int64_t)k * 300;
      if (t2 > matin) break;
      const int hh = (k * 5) / 60;
      const int b1 = (hh < 7) ? 16 : (hh < 17 ? 7 : 25);
      sr[0] = mkStat("1", b1, 30 - b1); sr[1] = mkStat("2", 20, 10); sr[2] = mkStat("3", 15, 15);
      h.applyStatus(sr, 3, t2);
    }
  // À 8 h la station 1 est PLEINE (elle est à 16 en temps normal à 7 h, on la met à 28 sur 30).
  sr[0] = mkStat("1", 28, 2); sr[1] = mkStat("2", 20, 10); sr[2] = mkStat("3", 15, 15);
  h.applyStatus(sr, 3, matin + 60);
  h.recompute(matin + 60);
  const int brutMatin = (int)lroundf((float)h.st[0].capEff() * 0.5f - h.st[0].fc[12]);
  CHECK(h.st[0].sens > 0, "garde-fou : motif toujours lu le matin (%d)", h.st[0].sens);
  int attenduMatin = brutMatin;
  if (attenduMatin > 60) attenduMatin = 60;
  if (attenduMatin < -60) attenduMatin = -60;
  if (abs(attenduMatin) < 2) attenduMatin = 0;
  CHECK(h.st[0].need == (int8_t)attenduMatin,
        "garde-fou : le matin, le retrait n'est PAS plafonné (conseil %d, formule nue %d)",
        h.st[0].need, attenduMatin);

  // Aucun relevé du tout (station 3 seule dans un modèle neuf) : sens 0, donc aucune réduction.
  static Model vide;
  vide = Model(); vide.init();
  CHECK(vide.sensJour(0, M0) == 0, "sens : un modèle sans relevé ne conclut rien (%d)", vide.sensJour(0, M0));

  // Le week-end ne porte pas de motif pendulaire : on ne conclut pas, on ne réduit pas.
  const int64_t samedi = lux(2027, 4, 10, 8, 0);   // samedi
  const int8_t sSam = m.sensJour(0, samedi);
  CHECK(sSam == 0, "sens : le samedi ne porte pas de motif pendulaire (%d)", sSam);
}
