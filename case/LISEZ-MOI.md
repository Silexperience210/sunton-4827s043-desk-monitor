# Boîtier du moniteur Sunton 4827S043

## Ce qu'il y a ici

- `stl/sunton-4827s043-carte.stl` — **modèle de référence de la carte** : PCB 123 × 74 × 1,6, dalle
  105,5 × 67,2 × 3, quatre trous Ø3,2 sur entraxe 105,5 × 67,2, USB-C sur le chant gauche, module
  ESP32 au dos. 128 mm de large à cause du connecteur qui dépasse.
- `gen_boitier5.py` — générateur du boîtier courant (Blender 5, headless).
- `rendus/` — les images de chaque passe.
- `gen_boitier.py`, `gen_boitier2.py`, `gen_boitier3.py`, `gen_boitier4.py` — passes antérieures,
  conservées pour l'historique du dessin (éclair, coque debout, arrondis, façade à deux niveaux).

## Cotes du boîtier courant (passe 5)

| Élément | Valeur |
|---|---|
| Épaisseur totale | 9,5 mm (façade 4,0 + dos 5,5) |
| Encombrement | 127,2 × 78,2 mm |
| Parois | 1,6 mm · jeu carte 0,5 mm |
| Renversement | 22° vers l'arrière |
| Écran | ouverture module + 0,6 · interface à 1,2 mm sous la lèvre |
| Ventilation | 4 fentes dans le dos, 2,2 × 44 mm |

## ⚠️ À vérifier au pied à coulisse AVANT d'imprimer

Les cotes de la carte viennent de la fiche technique, pas d'un relevé. Mesurer sur la carte réelle :
longueur, largeur, épaisseur du PCB, **entraxe des quatre trous**, position exacte de l'USB-C par
rapport au bord et à la dalle. Toute correction se fait dans les constantes en tête de `gen_boitier5.py`,
jamais dans un STL.

## Ce qui reste à faire avant une impression

1. Géométrie de production : poches d'inserts M3, dépouille, congés compatibles impression sans support.
2. Contrôle d'imprimabilité (DfAM) et export STL du boîtier.
3. Tranchage et envoi à la Bambu.

## Régénérer

```bash
blender -b --factory-startup --python case/gen_boitier5.py -- 1 case/rendus/passe5.png 128
```

L'interface plaquée sur la dalle est une capture réelle du simulateur :
`sim/sim ../docs/donnees /tmp/cap` puis `02_reseau_activite.png`.
