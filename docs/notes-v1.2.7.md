## Ce qui change

Le KPI « vélos en trajet » repose sur une estimation du parc total. Cette version en corrige trois
failles mesurées, et rend visible ce qui était implicite.

### Le parc tolère deux artefacts indépendants

Le parc retenu était la **troisième** plus grande valeur comparable : deux tranches aberrantes sur
vingt-quatre heures suffisaient à le fausser, et deux artefacts indépendants de deux tranches (une
station fantôme le matin, un cache le soir) faisaient annoncer **400 vélos en trajet pour 200 réels,
validés, pendant vingt-quatre heures**. La valeur passe au **sixième** rang : cinq tranches aberrantes
sont absorbées. Vérifié par mutation — à K = 3 ou K = 5, les tests tombent.

### Le quart calme est mesuré sur tout l'anneau

L'activité de la référence ne se comparait qu'aux tranches **plus anciennes** qu'elle (le rattrapage
comparait à −1 : code mort). Une référence prise en pleine vidange au démarrage passait pour un creux,
et **−40 % s'affichaient comme validés le premier jour**. Le comptage porte sur toutes les tranches
comparables, avant comme après ; la couverture est bornée par le haut.

### Le chiffre porte sa provenance

La tuile écrit « ≈ 1 419 · −20h » (le label fait 80 px, mesuré) ou « parc ≈ 880 » quand la référence
est le relevé courant ; la fiche donne l'heure exacte, la profondeur d'historique, la couverture, et dit
que c'est une **estimation** — les deux sens d'erreur sont écrits. L'invariant « parc ≥ vélos garés »
tient désormais toujours, avec une tolérance de bruit de ±1 % qui ne jette plus une référence valable.

### Ce qui n'était pas censé partir avec

Trois défauts de chaînes livrés dans les binaires 1.2.0 à 1.2.6 sont éliminés (« rßérence », « Ürite ») :
une séquence `\xHH` suivie d'un chiffre hexadécimal déborde (`\xA9f` = `0xA9F` → `0x9F`) et avale la
lettre. Un test refuse maintenant toute fiche contenant cette famille d'octets, et un test du détecteur
vérifie qu'il les voit.

## Ce qui est vérifié

Banc : **1 037 vérifications, 0 échec**, dont les cas à vérité connue de la revue : deux artefacts
indépendants (parc 600, 200 en trajet), cinq tranches aberrantes, référence en pleine vidange
(provisoire), station fantôme, couverture réduite, coupure de douze heures, recul d'horloge, et le
détecteur de mojibake. Mutations : ancien `quiet` → 3 échecs ; K = 5 → le test des cinq tranches tombe.

**Non couvert** : rien n'a été mesuré sur la carte pour cette version avant publication ; la fréquence
réelle des artefacts dans le flux Vel'oh et le bruit de lecture nocturne restent des hypothèses (±3 et
±6 vélos) ; et sur une nuit **en rampe** sans palier de trente minutes, un biais permanent de quelques
vélos à une quinzaine subsiste (nul sur une nuit avec palier).

## Mise à jour

Depuis la 1.2.2 : aucune migration, aucun format de fichier ne change (`/vn_hist.bin` v2,
`/vn_prof.bin` v4, `/vn_days.bin` v1). Flash par-dessus, sans effacement, profils et historique
conservés. Les versions 1.2.3 à 1.2.6 n'ont jamais été publiées.
