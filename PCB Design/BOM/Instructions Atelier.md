# Instructions atelier · Révision 1.8

Fabriquer la carte à partir du dossier Fabrication. Utiliser les deux fichiers de perçage, y compris les lumières métallisées des prises et les quatre lumières non métallisées des modules USB. Imprimer la sérigraphie des deux faces. Carte FR-4 double face, 165,5 × 90,5 mm, épaisseur 1,5 mm, cuivre 35 µm par face.

Pour le montage par défaut, suivre Assembly\_BOM.csv et Placement\_Top\_SMT.csv. Monter deux barrettes femelles traversantes 1×5 au pas de 2,54 mm en J15 et J16. Monter une barrette mâle 1×4 en J17 et une barrette mâle 1×2 avec cavalier amovible en J18. Livrer J18 fermé. Pour alimenter uniquement le module ESP, couper toutes les alimentations, retirer le cavalier J18, débrancher les périphériques et appliquer 3,3V régulés (source ≥500mA) sur J17-4, masse sur J17-1. Utiliser des contacts prévus pour au moins 1,5A en J16. Poser D3 et D4 orange, D5 et D6 rouges et D7 verte. La pastille 1 des LED est la cathode.

Monter J1 (USB données), J3 (entrée 5V), R25 et R26 (0Ω, 0603) et R27 (0Ω, 1206, courant admissible ≥1,5A). Souder et inspecter les quatre pattes de blindage de chaque prise. Livrer J15 et J16 avec leurs barrettes femelles, sans module enfiché. Laisser également libres les emplacements J9, J10 et J12, dont les modules seront installés par le propriétaire. Les barrettes mâles des petits modules USB sont soudées sous les modules. Deux entretoises isolantes M2 par module reprennent les efforts d’insertion.

Le montage de série de J1 et J3 correspond au plan Amphenol 10118194-0001LF. Une prise générique nécessite un contrôle des cinq contacts, des quatre pattes de blindage, de la hauteur et de la position par rapport au bord. Si sa géométrie diffère, signaler l’écart avant assemblage pour valider la référence.
