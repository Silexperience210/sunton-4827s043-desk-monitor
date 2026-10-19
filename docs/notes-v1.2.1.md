## Ce qui change

**La recherche Wi-Fi fonctionne même sans réseau**, et **valider un mot de passe ne fait plus
redémarrer la carte**.

Deux défauts distincts, tous deux constatés sur la carte :

1. **Recherche impossible quand la carte n'était pas associée.** Le pilote refuse tout balayage
   pendant qu'une association est en cours (`ESP_ERR_WIFI_STATE`), et l'auto-reconnexion du framework
   relance `disconnect + begin` à chaque raison reconnectable. Avec un réseau enregistré injoignable
   ou un mot de passe erroné, la station passait donc son temps à s'associer : la recherche échouait
   systématiquement, et l'on ne pouvait plus découvrir — donc plus choisir — un autre réseau.
   La recherche **suspend maintenant l'association le temps du balayage** (une déconnexion volontaire
   n'est pas reconnectée par le framework), puis la relance. Un refus du pilote est réessayé au lieu
   d'être pris pour un échec définitif, avec un budget calculé pour couvrir le délai interne du
   framework.

2. **Plantage à la validation du mot de passe.** `doConnect()` demandait la suppression différée du
   clavier **puis** de la feuille qui le contient. LVGL exécute ces suppressions dans l'ordre inverse
   de leur création : la feuille partait d'abord, avec le clavier dans ses enfants, et le rappel du
   clavier écrivait dans un objet déjà libéré — `Guru Meditation (LoadProhibited)` sur le cœur 1,
   redémarrage de la carte **avant toute tentative d'association**. Une seule suppression suffit : la
   feuille emporte le clavier.

Correctifs liés :

- le réseau saisi à l'écran n'est **enregistré qu'une fois l'association réussie** : une faute de
  frappe ne devient plus le réseau du démarrage, l'ancien réseau est conservé ;
- le journal ne dit plus « déconnexion … délai dépassé » quand la carte coupe elle-même pour changer
  de réseau (code de raison 0 = non spécifiée) ;
- le journal dit ce qui s'est passé : `association suspendue`, `réseau enregistré : « X »`,
  `réseau « X » NON enregistré : l'ancien réseau reste celui du démarrage`.

## Mise à jour

**1.2.0 → 1.2.1 : aucune migration.** Aucun format de fichier ne change (`/vn_hist.bin` reste en v2,
`/vn_prof.bin` en v4) : flasher par-dessus, sans effacement, retrouve l'historique, les profils et
les résumés tels quels. **Retour arrière** : pas de perte de profil, le format ne dépend pas de cette
version.

## Recette faite sur la carte

- recherche associée : `recherche : 6 bornes, 5 réseaux listés … associé` ;
- changement pour un autre réseau, avec enregistrement après succès :
  `association ok : « Bitcoin », IP 10.70.221.50, -41 dBm, canal 11, en 1596 ms` puis
  `réseau enregistré : « Bitcoin » (ok)` ;
- mot de passe volontairement faux : `refusée : mot de passe refusé (code 15)` puis
  **`recherche : 5 bornes, 4 réseaux listés … non associé, association suspendue puis reprise`** —
  le cas qui était cassé — et `réseau « Bitcoin » NON enregistré : l'ancien réseau reste celui du
  démarrage` ;
- retour au réseau enregistré : association en 3,4 s, relevés `148 stations` repris ;
- aucun `Guru Meditation`, aucun incident au démarrage sur toute la session.

## Limite connue, dite telle quelle

Le banc du simulateur compte **954 vérifications, dont une en échec préexistant** :
« nuit en cours, relevé courant maximal » (`sim/test_core.cpp:359`). Ce test date de la 1.0.6 et
affirme l'ancienne règle de validité (creux de nuit dans une plage horaire), que la règle « quart le
moins actif » a remplacée en `46a944a`. Il est vérifié en échec sur `main` **sans** les changements de
cette version : c'est une dette de test, pas une régression de la 1.2.1. Les 953 autres passent.
