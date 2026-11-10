// ── Jeton de synchro : le champ doit tenir un VN_TOKEN entier ─────────────────────
// Un VN_TOKEN de serveur fait 64 caractères hexadécimaux. Le champ du firmware en faisait 48 :
// le jeton était TRONQUÉ en silence (47 + NUL), la carte envoyait un faux jeton, le serveur
// répondait 401, et la carte ne se synchronisait plus — sans un seul message nulle part.
// Ces vérifications portent sur trois points :
//   1. la taille du champ suffit pour 64 caractères ;
//   2. un jeton de 64 caractères survit à un aller-retour d'écriture / relecture ;
//   3. la réparation ne touche QUE la signature d'une troncature (préfixe strict).
static void testSyncToken() {
  // 1. Le champ doit accepter un jeton de 64 caractères et son NUL.
  CHECK(sizeof(vn::g_sync.token) >= 65, "jeton : champ de %zu octets (>= 65 requis pour 64 caractères)",
        sizeof(vn::g_sync.token));

  // 2. Aller-retour d'un jeton de 64 caractères par le fichier /vn_sync.cfg.
  const char * J64 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  CHECK(strlen(J64) == 64, "jeton de test : 64 caractères");
  vn::SyncCfg sauve = vn::g_sync;
  snprintf(vn::g_sync.url, sizeof(vn::g_sync.url), "http://192.168.1.176:8790");
  snprintf(vn::g_sync.token, sizeof(vn::g_sync.token), "%s", J64);
  vn::g_sync.atBoot = true;
  vn::g_sync.push = true;
  sim::g_fsDir = "/tmp/vn_test_sync";
  if (system("rm -rf /tmp/vn_test_sync") != 0) {}
  CHECK(vn::syncCfgSave(), "jeton : écriture de /vn_sync.cfg");
  memset(vn::g_sync.token, 0, sizeof(vn::g_sync.token));
  CHECK(vn::syncCfgLoad(), "jeton : relecture de /vn_sync.cfg");
#ifdef VN_SYNC_TOKEN
  // Firmware provisionné : le jeton compilé reprend la main au chargement, et c'est le
  // comportement voulu (une rotation serveur ne laisse jamais la carte en 401).
  CHECK(!strcmp(vn::g_sync.token, VN_SYNC_TOKEN),
        "jeton : le jeton compilé fait foi après relecture (relu %zu)", strlen(vn::g_sync.token));
#else
  CHECK(strlen(vn::g_sync.token) == 64 && !strcmp(vn::g_sync.token, J64),
        "jeton : 64 caractères conservés (relu %zu)", strlen(vn::g_sync.token));
#endif

  // 3. La réparation ne se déclenche que sur un préfixe strict.
  const char * COMPILE = "0123456789abcdef0123456789abcdef0123456789abcdef";
  CHECK(vn::syncTokenTruncated("0123456789abcdef0123456789abcdef0123456789abcde", COMPILE),
        "jeton : 47 caractères = préfixe strict → troncature reconnue");
  CHECK(!vn::syncTokenTruncated(COMPILE, COMPILE), "jeton : jeton complet → aucune réparation");
  CHECK(!vn::syncTokenTruncated("ffffffffffffffffffffffffffffffff", COMPILE),
        "jeton : jeton différent (pas un préfixe) → laissé tel quel");
  CHECK(!vn::syncTokenTruncated("", COMPILE), "jeton : absence de jeton → rien à réparer");
  CHECK(!vn::syncTokenTruncated(COMPILE, ""), "jeton : pas de valeur compilée → rien à réparer");
  CHECK(!vn::syncTokenTruncated("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                COMPILE),
        "jeton : jeton PLUS LONG que celui compilé → pas une troncature");

  vn::g_sync = sauve;
}
