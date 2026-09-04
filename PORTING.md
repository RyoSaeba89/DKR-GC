# Diddy Kong Racing — portage natif GameCube

Port natif du décompilé DKR vers la GameCube, avec **devkitPPC + libogc2**
(le fork d'Extrems). Aucune bibliothèque n'est réécrite : tout ce qui existe
déjà dans libogc2 est utilisé tel quel.

Ce document décrit l'architecture retenue, ce qui est fait, et ce qui reste.

---

## Build

```sh
make -f Makefile.gc            # -> build/gc/dkr.dol
make -f Makefile.gc assets     # -> build/gc/dkr.assets
make -f Makefile.gc dist       # les deux, dans dist/dkr/ a copier sur la carte SD
```

Trois reglages, tous documentes dans le Makefile :

| Variable | Defaut | Effet |
|---|---|---|
| `GC_EMBED_ASSETS` | `1` | Lie l'image d'assets dans l'executable. Indispensable sous Dolphin (voir plus bas) ; `0` produit le .dol de 1,3 Mo qui lit `dkr.assets` sur carte SD. |
| `GC_MAIN_POOL_MB` | `4` | Taille du tas du jeu. |
| `GC_DEBUG` | `0` | Active `gc_log` : trace de boot detaillee, compteurs par frame, diagnostics du walker de display list. |
| `GC_NO_CULL` | `0` | Dessine les deux enroulements au lieu de couper les faces arriere. Diagnostic, pas une option de rendu. |
| `GC_BILLBOARD` | `1` | Applique `G_MW_BILLBOARD` : un sommet devient un decalage par rapport au sommet 0, c'est ainsi que DKR place ses sprites. |
| `GC_RENDERMODE` | `1` | `0` fige le blend et la profondeur comme avant le travail sur le mode de rendu. |
| `GC_COMBINER` | `1` | `0` fige le TEV sur un modulate, comme avant la traduction de `G_SETCOMBINE`. |
| `GC_TEXTEST` | `0` | Remplace chaque texture par une mire de coordonnees : prouve conversion, pavage 4x4 et coordonnees d'un coup. |
| `GC_PROJ_TINT` | `0` | Vert = projection materielle, magenta = repli CPU. Teinte la couleur de sommet, donc un combineur qui ignore SHADE l'avale. |
| `GC_CRASHTEST` | `0` | Faute volontairement deux secondes apres le boot, pour exercer le gestionnaire de plantage. Il rapporte aussi sur la console, donc c'est validable hors materiel. Ne jamais livrer a 1. |
| `GC_FORCE_PAL` | `0` | Fait croire au portage que la console est PAL (640x576, EFB 528 lignes, +24 lignes du jeu). Diagnostic : le portage n'a jamais tourne qu'en NTSC et la console de l'utilisateur est PAL. Ne jamais livrer a 1. |
| `GC_MEMCARD` | `1` | Utilise une carte pour sauvegarder. `0` coupe **tout** le sous-systeme de stockage : rien ne sonde ni ne monte de carte memoire, le Controller Pak repond « absent » (donc le jeu reste hors du code pak et rumble de `save_data.c`) et l'EEPROM vit en RAM. C'est exactement l'etat du portage avant le 2026-09-04. Isolation, pas une option. |
| `GC_SDLOG` | `1` | Ecrit `sd:/dkr/dkr.log`. C'est le canal de diagnostic sur materiel reel, ou il n'y a pas d'USB Gecko. Sans carte montee, chaque appel est un compare et un retour. |
| `GC_AUDIO_FX` | `1` | Laisse le jeu allumer la reverberation. `0` la refuse : l'A/B en une compilation pour « la reverberation est-elle responsable de ce que j'entends ». |
| `GC_DYNLIT2` | `1` | `0` retransforme `calc_dynamic_lighting_for_object_2` en talon. Isolation, pas une option. |

Les objets dependent aussi du fichier `build/gc/.cflags`, qui memorise les
flags : changer `GC_DEBUG` ou `GC_MAIN_POOL_MB` reconstruit l'arbre au lieu de
relier des objets compiles avec l'autre reglage.

Utiliser le `make` de MSYS2 (`C:\msys64\usr\bin\make.exe`, GNU Make 4.4.1). Le
`make` 3.82 natif present sur le PATH via `C:\fpc322` fonctionne aussi, mais il
est ancien. Le Makefile trouve devkitPro tout seul et pose lui-meme `TMPDIR`
dans l'arbre de build : sans ca, le gcc natif de devkitPPC tente d'ecrire ses
fichiers intermediaires dans `C:\Windows` et echoue.

Le build N64 d'origine (`Makefile`) n'est pas touche.

### Assets

**Sous Dolphin, la carte SD n'existe pas.** La liste des peripheriques EXI
GameCube de Dolphin ne contient aucune carte SD -- son support SD est
Wii-seulement -- donc `sd:/` et `carda:/` ne peuvent jamais se resoudre en mode
GameCube, et `fatInitDefault` ne trouve rien. C'est la raison d'etre de
`GC_EMBED_ASSETS` : `platform/gc/assets_blob.S` fait un `.incbin` de l'image
dans une section `.rodata`, et `gc_assets_open_embedded()` la televerse en ARAM
par le meme chemin que la version SD. Les deux routes convergent donc sur un
seul lecteur, et celle de la carte continue de fonctionner telle quelle sur
materiel reel. Cout : 12 Mo des 24 de MEM1, et un .dol de 13,9 Mo.

Le jeu ne charge pas ses donnees au demarrage : il garde une table d'offsets ROM
et DMA les morceaux a la demande. Le portage conserve ce modele, et en tire une
simplification : **l'image d'assets, c'est la ROM elle-meme**, copiee telle
quelle. Chaque offset de la table tombe juste sans rebasage, et 12 Mo tiennent
dans les 16 Mo d'ARAM. `make -f Makefile.gc assets` ne fait donc que copier la
ROM depuis `baseroms/` — aucun outil d'extraction n'intervient.

Les deux symboles `__ASSETS_LUT_START` / `__ASSETS_LUT_END`, que
`asset_loading.c` lit comme des adresses ROM, sont fournis a l'edition de liens
depuis `ver/splat/dkr.us.v77.yaml` (`0xECB60` / `0xECC30`). Ils sont specifiques
a la US 1.0.

### `asset_enums.h` (fait -- a refaire seulement pour une autre revision de ROM)

`include/asset_enums.h` est genere par `dkr_assets_tool`, et **aucun** fichier
de `src/` ne compile sans lui.

Il n'est pas contournable : sur les 352 symboles `ASSET_*` que le code du jeu
reference, 128 se deduisent de `tools/dkr_assets_tool_extract.json`, mais les
224 autres (`ASSET_MENU_TEXT_*`, `ASSET_FONTS_*`) sont nommes d'apres des
chaines et des polices qui vivent *dans* les assets — ils n'existent qu'une fois
l'extraction faite.

L'outil se construit et se lance sous Windows, mais son extraction se termine en
silence sans rien ecrire. Le chemin sur est celui que le depot supporte
officiellement :

```sh
wsl                                              # une fois : wsl --install
cd /mnt/c/Users/jacqu/Documents/DKR-GC/dkr
./tools/gc/extract-asset-enums.sh
```

Le script verifie le SHA1 de la ROM, installe les dependances, construit
l'outil, lance splat puis l'extraction, et s'arrete des que `asset_enums.h`
existe. Ensuite tout se rebuild normalement cote Windows.

### Notes de construction sous Windows

Deja applique dans ce depot, mais bon a savoir si l'environnement bouge :

- Le depot etait en CRLF (`core.autocrlf=true`), ce que le parseur C de
  `dkr_assets_tool` refuse. Il est desormais en LF, avec `core.autocrlf=false`
  en local.
- splat doit tourner en mode UTF-8 (`PYTHONUTF8=1`), sinon il lit les sources
  avec la locale du systeme et echoue sur un octet 0x81.
- `dkr_assets_tool` se compile avec le g++ **POSIX** de MSYS2
  (`C:\msys64\usr\bin\g++`), pas celui de mingw64 : `std::filesystem::path` ne
  se convertit implicitement en `std::string` que sur POSIX.

---

## Deboguer le portage

> **Règle, posée le 2026-09-04 : on ne teste plus jamais rien sous Dolphin.**
> Pas même « juste pour vérifier qu'on n'a rien cassé ». L'émulateur est
> indulgent exactement là où ce portage se trompe — MMU, caches, alignement,
> EXI — donc un run vert n'y prouve rien, et **aucun** des défauts matériels du
> projet n'y était reproductible. Chaque compilation part sur la carte SD, et
> le journal est l'instrument. Ce qui suit sur Dolphin est conservé comme
> archive de ce qui a été appris, pas comme mode opératoire.

La console framebuffer ne sert que jusqu'a la premiere frame : des que GX
recopie son EFB, le texte disparait. Tout ce qui est interessant se passe
apres.

**Sur materiel reel, le canal de debug est `sd:/dkr/dkr.log`.** Tout ce que
`gc_log` et `gc_fatal` impriment y va aussi, et le gestionnaire de plantage y
ecrit son rapport complet. C'est la seule facon de recuperer quoi que ce soit
d'une session sur console. Voir « Le journal sur carte SD » plus bas.

**Et l'ecran de plantage est le second canal, sur console.** libogc affiche les
registres, `SRR0`, `DAR`, une trace de pile et un extrait de code. Une
photographie de la television, passee a `powerpc-eabi-addr2line` contre l'ELF de
la compilation exacte, a donne en une fois ce que quatre sessions instrumentees
n'avaient pas donne. **Figer l'ELF a chaque livraison** est ce qui rend ca
possible : le .dol seul ne se resout pas.

Attention en le lisant : **un registre que l'exception rapportee n'ecrit pas est
une trace de l'exception d'avant.** `DAR` et `DSISR` ne sont pas mis a jour par
une exception « Interrupt », « Decrementer » ou « Floating Point », et c'est
justement un `DAR` perime qui a nomme la seconde cause racine.

**Sous Dolphin, le canal de debug est la console USB Gecko** (la carte SD
n'existe pas en mode GameCube, donc le journal ne s'ouvre pas). `gc_main.c`
appelle `CON_EnableGecko(EXI_CHANNEL_1, TRUE)` **seulement si libfat n'occupe
pas ce slot** : sur la console de l'utilisateur, le lecteur SD *est* en slot B,
et un USB Gecko et un SD Gecko sont tous deux le peripherique EXI 0 de leur
canal. Dolphin emule le Gecko en serveur TCP sur `127.0.0.1:55020` ; cote
Dolphin il faut `SlotB = 7` dans `Config/Dolphin.ini`. Un client TCP qui se
connecte et vide le flux dans un fichier suffit ; le lancer *avant* Dolphin
evite de perdre la trace de boot.

**Apres la premiere frame, la console framebuffer est repointee sur un tampon
prive de 320x32.** Elle coutait 686 Ko de `memcpy` non cache par ligne defilee
dans le framebuffer que GX possede, ce qui a mesure la moitie de la machine ;
et `gc_console_set(FALSE)` fait taire les `printf` du portage quand aucun Gecko
n'ecoute. Ni l'un ni l'autre ne touche le journal SD, qui n'est jamais passe par
printf.

**Le stub GDB de Dolphin n'est pas utilisable ici.** Avec `GDBPort` positionne,
l'emulation passe en interprete : en 250 s le jeu n'atteint meme pas l'init
video, a cause du televersement de 12 Mo en ARAM. Les compteurs et la console
Gecko repondent aux memes questions en quelques secondes.

**Et l'avertissement le plus cher de tous : Dolphin est indulgent exactement la
ou le portage differe le plus du materiel.** Il n'emule pas la MMU pour du
homebrew — ecrire a l'adresse 4 ou a 0xDEADBEE0 y reussit tranquillement, et il
faut `__builtin_trap()` pour en tirer une exception. Les caches, l'alignement et
l'EXI sont dans le meme cas. **Quatre des defauts trouves le 2026-09-04
n'existaient que sur console et aucun n'etait reproductible sous l'emulateur.**
Quand un symptome n'apparait que sur materiel, ne pas insister avec Dolphin :
instrumenter la console.

**Ce que `GC_DEBUG=1` fait afficher**, une fois par seconde, depuis le thread de
boot :

- les taches que le scheduler a distribuees (gfx, audio) ;
- la chaine du message de retrace, comptee a chaque saut : l'interruption VI qui
  poste vers le scheduler, les posts perdus faute de place, ce que le scheduler
  a recu et ce qu'il a reexpedie a ses clients ;
- les entrees/sorties de `gc_gfx_run_dl`, `gc_video_swap`, `gc_gfx_copy_display`
  et `fb_update` : un compteur qui entre sans ressortir *nomme* l'appel qui a
  bloque ;
- ou le thread de jeu s'est endormi (adresse de retour de l'appelant d'un
  `osRecvMesg` bloquant, a resoudre avec `powerpc-eabi-addr2line`), et zero s'il
  n'est pas bloque mais en boucle ;
- un debordement de pile, via une sentinelle en bas de chaque pile.

C'est cette instrumentation qui a permis de localiser les quatre blocages
decrits dans « Corrige recemment » plus bas, sans debogueur.

---

## Architecture

Le code du jeu (`src/`, ~80 000 lignes) n'est pas modifié. Tout le portage vit
dans `platform/gc/`, qui remplace `libultra/` et les trois fichiers de `src/`
qui décrivent le matériel N64 plutôt que le jeu.

```
platform/gc/
  gc_main.c          point d'entrée, remplace src/main.c
  gc_video.c         VI + framebuffers, remplace src/video.c
  gc_assets.c        image d'assets en ARAM
  gc_ultra.h         surface interne de la couche
  include/PR/        headers qui masquent ceux de include/PR
  gc_storage.c       un blob nomme : carte memoire, puis carte SD
  gc_logfile.c       le journal sur carte SD
  gc_crash.c         le gestionnaire de plantage (vecteurs PowerPC)
  gc_n64io.c         les registres materiels N64, repondus au lieu d'etre lus
  gc_assert.c        `__assert` avec l'ordre d'arguments du decompile
  ultra/             libultra sur libogc2 (12 fichiers)
  gfx/               F3DDKR -> GX
  audio/             liste de commandes audio -> mixeur logiciel
```

### Trois décisions structurantes

**1. Le boutisme joue en notre faveur.** MIPS N64 et PowerPC Gekko sont tous
deux big-endian. Les données d'assets, les display lists et les échantillons
audio se lisent tels quels, sans byte-swap. C'est la raison principale pour
laquelle ce portage est nettement plus simple vers la GameCube que vers une
cible little-endian.

**2. Le scheduler est la couture du portage.** Sur N64, le thread scheduler
arbitre le RSP et le RDP : il met les tâches en file, les lance, attend les
interruptions de fin. Ici aucun des deux coprocesseurs n'existe, donc
`platform/gc/ultra/os_sched.c` exécute la tâche immédiatement — une display
list part dans l'interpréteur GX, une liste de commandes audio dans le mixeur —
et renvoie le message de fin. Tout l'arbitrage disparaît, la messagerie est
préservée à l'identique. C'est elle qui cadence le jeu.

**3. L'ARAM remplace la cartouche.** L'image d'assets fait ~12 Mo pour 24 Mo de
MEM1, et le jeu veut un gros tas. Les 16 Mo d'ARAM sont inutilisés dans un port
comme celui-ci, ne sont pas adressables par le CPU, et y accéder demande un DMA
— exactement l'opération qu'on émule. `osPiStartDma` devient une requête ARQ.
`asset_loading.c` et `memory.c` ne bougent pas.

### Collision de types PR / libogc

`PR/gbi.h` et `<ogc/gx.h>` définissent tous les deux `Mtx` et `Vtx`, avec des
sens incompatibles. Une unité de compilation ne peut pas voir les deux.

Le renderer vit donc entièrement du côté libogc : `platform/gc/gfx/gfx_gx.h`
n'expose que des types simples (`const void *`), et le scheduler — qui est du
côté PR — appelle à travers cette frontière. Les quelques valeurs d'opcodes GBI
nécessaires sont redéfinies dans `gfx_gx.c`.

Deux autres frictions de headers, réglées :

- `PR/ultratypes.h` écrit `u32` comme `unsigned long`, `<gctypes.h>` comme
  `unsigned int`. `platform/gc/include/PR/ultratypes.h` masque le header N64 et
  délègue à `<gctypes.h>`, ce qui aligne tout l'arbre sur la bibliothèque contre
  laquelle on édite les liens.
- `PR/os_libc.h` déclare `bcopy`/`bcmp`/`bzero` avec des `int`. Le décompilé
  prévoit déjà `-DMODERN_CC` pour basculer sur `size_t` ; c'est dans le Makefile.

---

## État

**Le jeu tourne, s'affiche et se navigue sous Dolphin.** Le .dol monte la
video, libultra, GX et la manette, televerse ses assets en ARAM, et la boucle de
jeu s'execute de façon stable sans un seul message de retrace perdu. Toute la
boucle d'attract rend : l'ecran-titre avec son logo, le survol d'introduction,
l'ecran de selection de personnage et les courses de demonstration. START amene
au menu puis au carrousel de personnages.

**Sur materiel reel (console PAL), au 2026-09-04 : il boote, il dessine, et il
tourne a la vitesse nominale.** Sept sessions sur console, chacune documentee
plus bas avec ce qu'elle a appris. Quatre defauts que Dolphin ne pouvait pas
montrer ont ete trouves et corriges :

1. **La console USB Gecko ecrivait dans le SD Gecko** — meme canal EXI, depuis
   le premier jour du portage.
2. **Le message de fin de tache graphique** : le portage renvoyait `task->msg`
   tel quel, alors que le vrai scheduler substitue un tableau quand la tache
   n'en porte pas — ce qui est le cas de *toutes* les taches graphiques de DKR.
   Le jeu dereferencait NULL a chaque image.
3. **Un registre materiel N64** lu en dur par `cam_init`, non mappe ici.
4. **`__assert`**, dont la signature du decompile et celle de newlib ont le meme
   nom, trois arguments chacune, et **l'ordre inverse**.

Plus deux corrections de performance et de fiabilite : la console framebuffer
coutait la moitie de la machine (mesure : 1659 ms pour 60 retraces, contre 1200
apres), et le journal corrompait la FAT en s'allongeant.

**Aucun de ces quatre defauts ne se reproduisait sous l'emulateur.** C'est le
point de methode central de la journee ; voir la fin de « Reste a faire ».

Etat mesure a la huitieme session, la premiere ou le jeu tourne vraiment :

    clock: 1200 ms since last beat (60 VSyncs)     <- exact pour du 50 Hz
    dkr-gc: 180 retr | task gfx 129 | vi 180/0 | dl 129/129 swap 129/129 copy 129/129
    ignored:          <- vide
    aud-ign:          <- vide
    n64 io: 3 reads, 0 unknown | asserts 0

Cent vingt-neuf images rendues en cent quatre-vingts retraces, **zero message de
retrace perdu** (contre plus de perdus que recus deux sessions plus tot),
entrees egales aux sorties partout, et les trois assertions de `env.c` ne se
declenchent pas -- le piege `__assert` etait latent, pas actif.

**Le defaut ouvert que cette session a revele** est dans le chargement des
assets, pas dans le rendu : voir « Un asset ne se decompresse pas ».

**L'audio, au 2026-09-04 : la saturation est corrigée, cause racine trouvée.**
La moitié de chaque trame audio était du bruit — `a_interleave` n'écrivait que
la moitié des échantillons attendus et le reste du tampon partait en DRAM tel
quel, c'est-à-dire la sortie brute du rééchantillonneur à pleine échelle. Voir
« `a_interleave` » plus bas. Les quinze opcodes de l'ABI sont maintenant
implémentés, `A_POLEF` compris, donc **la réverbération est rallumée**
(`GC_AUDIO_FX ?= 1`). Mesuré après correction : 0 à 64 échantillons écrêtés sur
18 000 par tâche audio, contre un pic collé à 32767 à presque chaque battement
avant. Reste à valider à l'oreille sur matériel réel.

**Aucun talon ne subsiste.** `platform/gc/stubs.c` ne contient plus que des
symboles inertes par construction (registres RSP/RDP, images de microcode). Le
Controller Pak, le gestionnaire de plantage et les sept points d'entrée de
`thread0_epc.c` sont portés, pas répondus.

**Un journal est écrit sur la carte SD** (`sd:/dkr/dkr.log`) : c'est le canal de
diagnostic sur matériel réel, où il n'y a pas d'USB Gecko. Voir « Le journal sur
carte SD ».

### La mesure d'avancement, et comment la relire (2026-09-02)

Le walker avale en silence toute commande qu'il ne connait pas (`default:
break;`), donc « il reste beaucoup de bugs graphiques » n'avait pas de liste. Il
**compte desormais ce qu'il jette**, par opcode, et le heartbeat l'imprime :

    ignored: b4:1 f9:1

C'est la mesure d'avancement du renderer. Vide, le rendu est complet vis-a-vis
de ce que le jeu envoie ; chaque entree est une fonctionnalite manquante, avec
la frequence qui dit ce qu'elle coute.

Releve sur 84 frames couvrant intro, menus et une course de demo : **le jeu
emet 33 opcodes, le port en traite 31, il en ignore 2.** Le releve du matin en
comptait sept ; les cinq autres (`b6` `b7` `f8` `fe` `ff`) ont ete traites dans
la journee -- voir le brouillard et les cibles de rendu plus bas.

| Opcode | Nom | /frame | Ce qui reste a faire |
|---|---|---|---|
| `b4` | `G_PERSPNORMALIZE` | 1,3 | echelle sur `w` avant division. Impact attendu faible : la projection materielle calcule ses coefficients exactement depuis la matrice, donc l'echelle n'a rien a corriger. |
| `f9` | `G_SETBLENDCOLOR` | 1,0 | lue par un blender dont `P = G_BL_CLR_BL`. Aucun mode de rendu mesure ne le fait : tous les `omL` releves ont `P = CLR_IN` ou `CLR_FOG`. |

Les trois syncs (`e6` `e7` `e9`) sont nommes explicitement dans le `switch`
plutot que laisses au `default`, pour qu'ils n'apparaissent pas comme du
travail : ils ordonnancent le pipeline du RDP contre lui-meme et il n'y a pas
de RDP ici.

**Pour refaire le releve** : `tools/gc/run.ps1` fait toute la boucle -- il tue
les Dolphin qui trainent, lance le .dol, se connecte au socket Gecko, pilote la
manette, prend des captures a intervalle et detecte une sortie de Dolphin.

    tools/gc/run.ps1 -Tag gap -Seconds 130 -Presses "28:ENTER,34:ENTER,42:X"

Les touches se donnent en `seconde:touche` (ENTER = START, X = A, Z = B). Les
captures et le log atterrissent dans `build/gc/capture/`. Puis

    grep "ignored:" build/gc/capture/gap.log | tr ' ' '\n' | grep ":" | grep -v ignored \
      | awk -F: '{c[$1]+=$2; n[$1]++} END {for (o in c) printf "%s %d %d\n", o, c[o], n[o]}' \
      | sort

Hors renderer, il reste le mixeur audio (zero opcode de l'ABI ecrit), la carte
memoire, le gestionnaire de plantage, et deux fichiers d'assembleur MIPS --
`obj_animate.s` et `obj_shade_fast.s`, les deux qui se voient a l'ecran.

### Correction du 2026-09-02 : `G_FILLRECT` ne veut pas dire « aplat »

**C'est ce qui peignait le jeu en noir**, et le diagnostic a tenu en une ligne
d'instrumentation.

Ce que fait `G_FILLRECT` depend du **type de cycle**. En `G_CYC_FILL` le RDP
court-circuite le combineur et le blender et ecrit la couleur de remplissage
directement dans le framebuffer -- c'est le clear en tete de frame. En
`G_CYC_1CYCLE` ou `G_CYC_2CYCLE` il ne fait rien de tel : le rectangle est une
primitive ordinaire qui passe par le combineur et le blender, et la couleur de
remplissage n'est jamais consultee.

DKR se sert de la seconde forme pour ses fondus, et le dit mot pour mot --
`transition_render_fullscreen`, `src/fade_transition.c` :

```c
gSPDisplayList(dTransitionFadeSettings);   /* G_CYC_1CYCLE, G_RM_CLD_SURF */
gDPSetPrimColor(..., gCurFadeRed, gCurFadeGreen, gCurFadeBlue, gCurFadeAlpha);
gDPSetCombineMode(G_CC_PRIMITIVE, G_CC_PRIMITIVE);
gDPFillRectangle(0, 0, w, h);
```

La couleur du rectangle est donc PRIMITIVE et son alpha celui du fondu. Hors
transition cet alpha vaut **zero** et le rectangle est invisible. Le port le
dessinait en aplat opaque : un rectangle noir opaque sur toute la surface, en
**derniere primitive de chaque frame de jeu**, par-dessus dix-sept cents
triangles correctement dessines en dessous.

Mesure, avec le releveur `cover` ajouté pour l'occasion :

    cover kind4 area 1000/1000 (0,0)-(1000,1000) seq 1725/1725
      | cc fcffffff fffdf6fb omH 00802c0f omL 00504340
      | col 000000ff prim 00000000

soit : un fill plein ecran, dernier de la liste, en un-cycle (`omH` bits 20..21
a 0), mode de rendu `G_RM_CLD_SURF`, couleur primitive noire d'alpha zero. Les
frames magenta etaient le meme rectangle avec une autre couleur de remplissage.

Correctif dans `gfx_fill_rect` : cycle FILL -> aplat comme avant, precede d'un
`gfx_set_2d_state()` explicite parce que le lot precedent a pu laisser le
blender allume ; sinon quad blanc a travers `apply_combiner_current(FALSE)` et
`apply_render_mode()`, la couleur venant du combineur.

Resultat : plus une seule frame noire ni magenta sur 170 s d'attract. Le logo
de l'ecran-titre, les huit personnages de PLAYER SELECT et les courses de
demonstration apparaissent, tous invisibles jusque-la.

### Brouillard, mode geometrie et cibles de rendu (2026-09-02)

Trois des sept opcodes ignores traites ensemble, parce que la mesure a montre
qu'ils n'en font qu'un.

**Le mode geometrie ne sert qu'a une chose ici.** Releve sur 84 frames :
`geo set 00010205 clear 001f3205`. Les bits `G_CULL_FRONT` (0x1000) et
`G_CULL_BACK` (0x2000) n'apparaissent **que dans le masque d'effacement,
jamais dans celui de pose** -- le culling par mode geometrie ne s'active donc
jamais, et le drapeau `BACKFACE_DRAW` par triangle, deja honore, est toute
l'histoire. `G_LIGHTING` est lui aussi seulement efface (DKR fait son ombrage
lui-meme, dans `obj_shade_fast`). Le seul bit qui compte est **`G_FOG`**, pose
et efface pour de bon. Cela reduit « implementer le mode geometrie » a
« implementer le brouillard », ce qui n'etait pas evident depuis la source.

**Le brouillard.** Sur N64 le RSP ecrit le facteur de brouillard **dans l'alpha
du sommet**, et le blender le consomme en cycle 1 via `G_RM_FOG_SHADE_A`. Le
port fait pareil : `gSPFogPosition` arrive en `G_MW_FOG` et donne les deux
coefficients (`fogMul = (s16)(w1 >> 16)`, `fogOff = (s16)w1`), chaque sommet
recoit `alpha = z/w * fogMul + fogOff` borne a un octet, et un etage TEV
supplementaire interpole `CPREV` vers une couleur konst par `RASA`.

GX a bien une unite de brouillard, mais sa courbe est la sienne et ne
reproduirait pas le facteur lineaire du RSP -- d'ou le passage par l'alpha,
qui est de toute façon ce que fait le materiel d'origine.

L'etage n'est ajoute que si les trois conditions tiennent : `G_FOG` pose, mode
deux cycles, et les muxes du cycle 1 valant `P = G_BL_CLR_FOG`,
`A = G_BL_A_SHADE`. Les mots mesures le confirment : `omL c8112078` a bits
31..30 = `11` et bits 27..26 = `10`. `apply_render_mode` lit deliberement le
cycle 2 (c'est lui qui ecrit en memoire) et jetait donc le brouillard en
entier.

**`G_SETCIMG` / `G_SETZIMG` : c'etait le vidage du Z, pas une cible hors
ecran.** Mesure : exactement deux cibles couleur par frame, `81000000` et
`82000000`, et `zimg 82000000`. Une des deux `G_SETCIMG` pointe donc l'image
couleur **sur le tampon de profondeur** -- l'idiome N64 ordinaire pour vider le
Z. Il n'y a aucune cible hors ecran dans ce jeu : rien a rediriger, tout a
empecher. `drawing_to_color()` ecarte le remplissage quand `CIMG == ZIMG`, les
deux etats etant remis a zero par liste pour que l'inconnu dessine. Mesure :
`fills` passe de 3 a 2, avec `1 vers le Z` compte. Sur GX la profondeur est
videe par la recopie EFB (`gc_gfx_copy_display` force l'etat d'ecriture), donc
il n'y a rien a faire a la place.

### `obj_animate` et `obj_shade_fast` traduits (2026-09-02)

Deux des trois talons de `platform/gc/stubs.c`, ecrits en C dans
`src/hasm/obj_animate.c` et `src/hasm/obj_shade_fast.c` a partir des `.s`. Ce
ne sont pas des originaux retrouves : le portage n'assemble jamais les `.s`, et
la seule chose qui doit tenir est le comportement.

**Le format d'animation**, tel que l'assembleur le decrit. Un bloc d'octets
plat, decoupe en cles de taille fixe
`keyStride = 3 * numberOfAnimatedVertices + 12` ; la cle k est a
`animData + keyStride * (k + 2)`. Les trois octets par sommet sont un **delta
signe**, pas une position : une pose s'obtient en partant de la pose de repos
et en ajoutant toutes les cles jusqu'a celle voulue, et passer a une frame
voisine ne coute qu'une cle d'additions ou de soustractions. Cette somme
courante vit dans `ModelInstance::vertices[2]` -- un tableau de Vec3s, pas le
tableau de `Vertex` que son type declare.

`obj->animFrame` est en 12.4 : la partie entiere choisit la cle, les quatre
bits bas interpolent vers la suivante. La partie fractionnaire est construite a
part dans `D_8011D644` (0xC00 octets alloues au pool, `object_models.c:57`) et
ajoutee seulement a la copie d'affichage, pour que la somme courante reste
exacte. Les douze octets d'en-tete d'une cle sont quatre s16 gros-boutiens que
le RSP ne voit jamais : l'offset x/y/z de l'instance et l'inclinaison de tete,
interpoles pareil. La sortie double-tamponne entre `vertices[0]` et
`vertices[1]` via `animationTaskNum`.

**`obj_shade_fast`** est un ombrage diffuse par sommet, ecrit en gris directement
dans le tampon de sommets vivant : un produit scalaire de la normale contre une
direction fixe, mis a l'echelle et biaise par un niveau ambiant
`shading->unk0 * intensity * 160` tronque, puis `r = g = b` et alpha force
opaque. Il n'y a pas de lumiere coloree : `ShadeProperties` porte bien
`lightR/G/B`, mais la fonction ne les lit jamais. La direction utilisee est
`shadowDirX/Y/Z` -- les noms du decomp disent « shadow », l'assembleur s'en
sert comme direction d'ombrage. Les lots dont `miscData` vaut `BATCH_VTX_COL`
portent leurs propres couleurs et sont sautes.

### Le tampon de rebond ARAM partage entre deux threads (2026-09-03)

**Le defaut structurel de la journee, et il ne vient pas de l'audio.**

Toute l'image d'assets vit en ARAM et `osPiStartDma` devient un transfert ARQ
via `gc_assets_read`. Ce chemin a **deux appelants sur deux threads** :

- le thread de jeu (priorite 10) -- pistes, modeles, textures ;
- le thread **audio** (priorite **12**, donc preemptif) via `__amDMA`
  (`src/audiomgr.c:465`), une fois par voix ayant besoin d'echantillons,
  jusqu'a cinquante fois par trame audio.

Les deux prenaient le chemin lent, a travers **un unique `sBounce` statique, sans
aucun verrou**. Et le chemin lent n'est pas l'exception qu'il parait : les
lectures audio utilisent un offset ROM que `__amDMA` n'arrondit qu'a une adresse
**paire**, jamais a 32 octets. Mesure :

    aram reads 6207, slow 5620, contended 20

**90 % des lectures passent par le tampon partage.** Le scenario est direct : le
thread de jeu DMA un modele dans `sBounce`, le thread audio preempte et
l'ecrase avec des echantillons, le thread de jeu reprend et recopie **des
echantillons audio dans son tampon de sommets**. Geometrie corrompue, normales
corrompues, en-tetes corrompus -- et un plantage le jour ou un mauvais pointeur
est finalement dereference.

Le commentaire de `aram_dma` **raisonnait deja jusqu'a ce danger exact** pour la
structure `ARQRequest` et la mettait sur la pile. Le meme raisonnement n'avait
simplement pas ete reporte sur le tampon. Corrige par un mutex LWP qui rend le
DMA et la recopie qui le lit indivisibles.

**Ce que la mesure permet et ne permet pas de conclure.** 20 collisions en 90
secondes, avec un pic de 4 pendant un chargement : c'est une explication solide
pour une **corruption rare et cumulative, donc pour le plantage**, et pas pour
un craquement continu. Le compteur sous-estime (il n'attrape la collision que si
le verrou est tenu a cet instant precis), mais l'ordre de grandeur est des
dizaines, pas des milliers. Ne pas surinterpreter.

### La cadence audio : le jeu tournait a deux fois la vitesse (2026-09-03)

**Le mixeur n'y etait pour rien, et mes rustines sur l'anneau non plus.** La
mesure qui a tout tranche est l'horloge du synthetiseur lui-meme :

    curSamples/s 44640   (temps reel = 22050)   ratio 2.02

`drvr->curSamples` avance de `frameSamples` par trame audio, et le sequenceur
convertit les durees de notes en echantillons via ce meme `outputRate`. A une
trame audio par retrace, l'horloge tourne a 44160 ech/s contre un `outputRate`
de 22050 : **chaque note deux fois trop courte, toute la partition au double de
la vitesse**, et un surplus de production de 2x qu'il fallait bien jeter quelque
part -- d'ou les craquements par-dessus.

Le `* 2` de `fsize = outputRate * 2 / gVideoRefreshRate` dit exactement cela :
une trame audio vaut **deux** trames video. Le client audio recoit donc un
retrace sur deux (`ultra/os_sched.c`, route par `OS_SC_ID_AUDIO` ; le client
video garde les siens, le cadencement du jeu en depend).

Verification, trois compteurs independants dans une seule execution :

| | avant | apres |
|---|---|---|
| horloge synthe | ratio 2.02 | **0.979** |
| offres/s | 58-63 | **30** |
| `rejected` / `refused` | 35/s, 29/s | **0 / 0** |

Plus rien n'est jete, donc plus de craquement d'origine « tampon abandonne ».

**Et une lecon de methode sur l'instrument lui-meme.** Le heartbeat bat tous les
60 VSyncs et j'ai longtemps divise par « une seconde ». Ce n'en est pas une :

    clock: 1101 ms since last beat (60 VSyncs)

Sous Dolphin les VSyncs et les retraces VI divergent de 10 %, et **deux mesures
audio ont ete mal lues a cause de ca**. Le heartbeat imprime desormais les
millisecondes reelles (base de temps Gekko) ; tout taux tire de ce log doit etre
mis a l'echelle par `1000/elapsedMs`.

**Dimensionnement de l'anneau, entierement empirique.** Un tampon entier fait
1568 trames de sortie ; si l'anneau n'a pas cette place d'un coup, la poussee est
tronquee -- on joue le debut du tampon puis on saute au suivant, ce qui hache la
forme d'onde. Mesures :

    2048 :  tronque au tiers    -> 12.5 % de silence, synthe a 0.89 x
    4096 :  ~995 sur 1568       ->  6.8 % de silence
    8192 :  jamais tronque      ->  2.4 % de silence, synthe a 0.979 x

Les 2.4 % restants sont le deficit d'offre (`frameSamples` colle a son plancher
de 720 au lieu de 736), pas de la troncature. Cout : ~112 ms de latence.

### `calc_dynamic_lighting_for_object_2` traduit (2026-09-03)

Le dernier des trois talons, ecrit dans `src/hasm/obj_shade_fast.c` sous
`obj_shade_fast`. **`platform/gc/stubs.c` n'a plus aucun talon de cette
categorie.**

Il n'a pas fallu le lire a l'aveugle, et c'est le point important : son frere
`calc_dynamic_lighting_for_object_1` est **deja decompile en C** a
`src/objects.c:7963`, et les deux sont les branches d'un seul `if/else` a
`src/objects.c:7949` -- eclairage directionnel d'un cote (Diddy de l'intro,
Taj, T.T., les boss), ambiant de l'autre (les racers, le logo Rare, la tete de
Wizpig). Chaque grandeur a donc pu etre confrontee a une contrepartie
decompilee qui calcule la meme chose depuis la meme structure, et chaque offset
touche par l'assembleur a ete verifie dans `include/structs.h` plutot que
suppose :

| Offset | Champ | Verifie dans |
|---|---|---|
| `ShadeProperties 0x00 / 0x1C-0x20 / 0x28 / 0x2C` | `unk0`, `shadowDirX/Y/Z`, `ambient`, `diffuse` | `structs.h:890` |
| `Object 0x00 / 0x44 / 0x54` | `trans`, `curVertData`, `shading` | `structs.h:1540` |
| `ObjectModel 0x28 / 0x38 / 0x40` | `numberOfBatches`, `batches`, `normals` | `structs.h:644` |
| `TriangleBatchInfo` = 12 o, `0x06` / `0x08` | `miscData`, `flags` | `structs.h:618` |
| `Vertex` = 10 o, `0x06`-`0x09` | `r`, `g`, `b`, `a` | `structs.h:574` |
| `0x8000` dans `flags` | `RENDER_ENVMAP` | `textures_sprites.h:61` |

**Ce qu'il fait, et ses trois ecarts avec `_1`.** Meme gris par sommet : un
produit scalaire de la normale contre une direction, mis a l'echelle par un
facteur diffus et biaise par un facteur ambiant. Mais :

- `_1` fait tourner la direction dans l'espace objet avec `vec3f_rotate_ypr` ;
  celui-ci construit la transformee inverse de l'objet comme matrice
  (`mtxf_from_inverse_transform` sur les trois angles negatifs, position nulle)
  et multiplie par elle. Meme intention, chemin different -- c'est le chemin de
  l'assembleur qui est reproduit.
- `_1` calcule un `shadeStrength` separe pour l'alpha ; celui-ci force l'alpha
  opaque et ecrit `r = g = b` = le meme gris.
- `_1` lit `lightDir` **et** `shadowDir` ; celui-ci ne lit jamais que
  `shadowDir`.

**Deux details numeriques qui ne sont pas interchangeables.** Le decalage de 21
bits apres `dot * diffuseFactor` est un `srl`, **logique**, pas arithmetique :
un produit qui deborde dans le bit de signe revient en grand positif puis se
sature a 255. La multiplication se fait donc en `u32`. Et l'ordre des flottants
est celui de l'assembleur, pas celui de `_1` : la sous-expression commune
`(unk0 * intensity * 255)` est formee une fois et chacun de `ambient` et
`diffuse` la multiplie.

**Deux divergences assumees, verifiees avant d'etre prises.** La boucle sur les
lots est un `for` ici et un do-while dans l'original -- l'original executerait
donc son corps une fois meme avec `numberOfBatches == 0` et lirait un lot au
dela du tableau ; le cas est inatteignable, l'appelant n'entre ici qu'apres
avoir trouve un lot dont `miscData != BATCH_VTX_COL`. Et l'original laisse
`ObjectTransform.flags` non initialise sur la pile :
`mtxf_from_inverse_transform` ne lit que `rotation` et la position
(`src/hasm/math_util.c:422`), le champ est mort, il est initialise ici plutot
que laisse indetermine.

**L'instrument, pose avant de regarder l'ecran.** `dynlit2` dans le heartbeat :

    dynlit2: obj 137 (nosh 0), verts 9412, grey 31..255

`obj` = les objets passes par la branche dans la derniere seconde, `nosh` ceux
repartis faute de `ShadeProperties`, `grey` l'etendue de la valeur ecrite. Les
trois separent trois pannes differentes : une branche qui ne tourne jamais, une
qui tourne sans atteindre de sommet, et une qui ecrit un gris colle a 0 ou a
255. Remis a zero par le heartbeat apres impression, donc chaque ligne vaut une
seconde.

### `a_interleave` : la moitie de chaque trame audio etait du bruit (2026-09-04)

**C'etait LE defaut audio.** Le pic du mixeur restait colle a 32767 depuis deux
jours, la reverberation avait ete exoneree par mesure, l'envmixer relu ligne a
ligne contre `ref-sm64gc` — et la cause etait un opcode que personne n'avait
regarde, parce qu'il ne fait que recopier des echantillons.

`aInterleave` entrelace deux tampons mono en un tampon stereo. Son `nbytes` est
la taille **d'un seul canal**, donc la commande produit `nbytes/2` paires, soit
deux fois `nbytes` octets de sortie. Le portage ecrivait :

```c
int count = ROUND_UP_16(sRspa.nbytes) / sizeof(s16) / 2;   /* faux */
```

soit `nbytes/4` iterations d'une paire chacune : **la moitie des paires**. La
reference (`ref-sm64gc/src/pc/mixer.c`, `aInterleaveImpl`) fait
`ROUND_UP_16(nbytes) / sizeof(int16_t) / 8` iterations de **huit** paires — le
meme compte ecrit autrement, `nbytes/2` paires. Le deroulage par huit avait
masque le facteur.

Consequence exacte, et elle explique tout ce qui avait ete observe. `save.c`
enchaine :

```c
aSetBuffer (ptr++, 0, 0, 0, outCount<<1);
aInterleave(ptr++, AL_MAIN_L_OUT, AL_MAIN_R_OUT);
aSetBuffer (ptr++, 0, 0, 0, outCount<<2);
aSaveBuffer(ptr++, f->dramout);
```

Le `aSaveBuffer` recopie `outCount<<2` octets depuis DMEM 0 quoi qu'il arrive.
L'entrelacement n'en remplissait que la premiere moitie ; la seconde moitie,
c'est la zone `AL_TEMP_1` (offset 320), c'est-a-dire `AL_DECODER_OUT` — la
sortie du decodeur ADPCM et du reechantillonneur, **avant enveloppe**, donc a
pleine amplitude. Chaque trame audio etait donc 160 echantillons stereo
corrects suivis de 160 echantillons de bruit non attenue. A 60 trames par
seconde. « Inaudible, ca craque dans tous les sens » est la description exacte
de ce signal.

Correction : `count = ROUND_UP_16(nbytes) / sizeof(s16)` paires, une paire par
iteration.

**Lecon de methode, la meme que celle du 2026-09-03 mais dans l'autre sens.**
Le compteur `peak` — le plus grand echantillon absolu atteignant la DRAM — a
correctement designe le probleme, puis a cesse d'etre utile : c'est un maximum
sur un demi-million d'echantillons par seconde, donc **un seul** echantillon
ecrete l'epingle a 32767 et se lit exactement comme un mixage qui sature en
permanence. Apres correction il touchait encore la pleine echelle sur un quart
des battements, ce qui ressemblait a un defaut restant. Le heartbeat compte
desormais aussi les echantillons ecretes :

    aud 1397 cmds, 127 saves, peak 32768, clipped 19/18000

19 sur 18 000, c'est un jeu fort, pas une distorsion. **Un maximum ne dit rien
sur une distribution ; ajouter le compte a cote du maximum a coute trois
lignes.**

### `A_POLEF` implemente, reverberation rallumee (2026-09-04)

Le quinzieme opcode de l'ABI, et le seul que `ref-sm64gc` n'implemente pas : il
a fallu le reconstruire. L'appelant le contraint entierement, et c'est ce qui
rend la reconstruction sure plutot que devinatoire. `_filterBuffer`
(`libultra/src/audio/mips1/reverb.c:429`) emet exactement trois commandes :

```c
aSetBuffer (ptr++, 0, buff, buff, count<<1);      /* en place */
aLoadADPCM (ptr++, 32, lp->fcvec.fccoef);         /* 16 coefficients s16 */
aPoleFilter(ptr++, lp->first, lp->fgain, lp->fstate);
```

et `_init_lpfilter` (`drvrnew.c`) remplit ces seize coefficients :

    fc           = (lp->fc * 16384) >> 15
    lp->fgain    = 16384 - fc
    fccoef[0..7] = 0
    fccoef[8+k]  = 16384 * (fc/16384)^(k+1)

Trois consequences, et ensemble elles ne laissent aucune liberte :

1. Charger les coefficients par `aLoadADPCM` les range dans le book ADPCM, que
   le decodeur lit comme `book[0][0..7]` (predicteur a deux echantillons) et
   `book[1][0..7]` (a un echantillon). Ici la premiere moitie est nulle : le
   second pole est eteint, la reverberation demande en fait **un filtre a un
   pole**. Les huit puissances de la seconde moitie sont la vectorisation RSP
   (huit sorties par pas) ; un CPU scalaire n'a besoin que de la premiere.
2. La base en virgule fixe est 16384 (`SCALE` dans `drvrnew.c`), pas les 2048
   du decodeur.
3. `fgain + fc == 16384` exactement, donc gain unite en continu. C'est la
   propriete qui rend le filtre sur dans la boucle de retard, et c'est la
   verification que le decalage est le bon : tout autre decalage ferait mourir
   ou diverger la reverberation.

Donc, par echantillon :
`y = (fcoef[0] * y[-2] + fcoef[8] * y[-1] + gain * x) >> 14`.

Le terme a deux echantillons est conserve bien que ce jeu y charge toujours
zero : il coute une multiplication et c'est lui qui fait de ce code un filtre a
**poles** plutot qu'un cas particulier qui marche par accident.

`GC_AUDIO_FX` repasse donc a 1 par defaut. Le knob reste, comme A/B en une
compilation : « la reverberation est-elle responsable de ce que j'entends »,
question que ce portage a deja du poser deux fois.

### `G_PERSPNORMALIZE` et `G_SETBLENDCOLOR` : `ignored:` est vide (2026-09-04)

Les deux derniers opcodes que le walker jetait, `b4` et `f9`. `G_IMMFIRST`
valant -65 (0xBF), `b4` est `G_PERSPNORMALIZE` et non un `G_RDPHALF`.

- **`G_PERSPNORMALIZE`** donne au RSP un facteur 16 bits par lequel multiplier
  w avant la division perspective, uniquement pour garder la division dans la
  plage 16 bits ; `guPerspectiveF` le calcule en parametre de sortie a cote de
  la matrice (`src/camera.c:155`) et il ne fait pas partie de la projection. Le
  GP fait ici la division en virgule flottante, a partir de la matrice seule :
  il n'y a rien a mettre a l'echelle et rien a perdre. C'est donc un veritable
  no-op — ecrit comme un `case` et non laisse au `default`, pour que le
  recensement `ignored:` continue de vouloir dire « non traite ».
- **`G_SETBLENDCOLOR`** sert au RDP a deux endroits : l'entree `CLR_BL` du
  blender, et la reference du test alpha sous `G_AC_THRESHOLD`. DKR l'emet une
  seule fois par trame rendue — `gDPSetBlendColor(0, 0, 0, 100)` a
  `src/tracks.c:351` — et ne selectionne jamais `CLR_BL` dans un mux, donc la
  reference alpha est tout son effet. `apply_render_mode` honore desormais
  `G_AC_THRESHOLD` avec cette valeur au lieu de la confondre avec le 128 fixe
  de `CVG_X_ALPHA`.

Le heartbeat affiche maintenant `ignored:` vide et `aud-ign:` vide : **le
portage traite la totalite des opcodes que le jeu emet, graphiques et audio.**

### Le journal sur carte SD (2026-09-04)

`platform/gc/gc_logfile.c`. Tout ce que `gc_log` et `gc_fatal` impriment part
aussi dans `sd:/dkr/dkr.log` (ou `carda:/`, `cardb:/`).

La raison est simple et elle etait bloquante : **la console USB Gecko n'existe
pas sur du materiel reel.** Toute l'instrumentation du portage — opcodes jetes,
pic audio, adresse de blocage du thread de jeu, sentinelle de pile — devenait
invisible exactement la ou les defauts restants doivent etre trouves.

Trois proprietes, et comment chacune est obtenue :

1. **Survivre a l'interrupteur.** Personne ne quitte un jeu GameCube, on eteint
   la console. Un `FILE*` garde ouvert perdrait le cache de libfat et n'aurait
   jamais mis a jour l'entree de repertoire. Une vidange est donc un cycle
   complet ouvrir/ajouter/fermer : au retour, ce qui a ete journalise est sur
   la carte, taille comprise.
2. **Ne pas bloquer le jeu.** Le texte va dans un tampon de 16 Ko, vide a la
   fin de chaque battement (le thread de boot a 59 VSync de rien a faire
   ensuite), quand il approche du plein, et immediatement sur un `gc_fatal`.
   Rien dans le rendu ou l'audio n'attend la carte.
3. **Etre appelable depuis n'importe quel thread.** Mutex LWP, avec une sortie
   sans verrou pour le gestionnaire de plantage — qui ne peut pas se permettre
   de bloquer sur un mutex que le thread fautif detient peut-etre.

Sans carte montee, tout degrade a un compare et un retour : c'est le cas
Dolphin, ou le Gecko reste le meilleur canal.

**Effet de bord corrige au passage :** `fatInitDefault` n'etait appele que
depuis `gc_assets_open`, donc une compilation avec assets embarques — celle qui
tourne sous Dolphin, et celle que produit `dist` — **ne montait aucun systeme
de fichiers**. Le fichier de sauvegarde n'avait nulle part ou aller. Le montage
est desormais une operation a part (`gc_fat_mount`, mise en cache), faite par
`gc_main` avant tout ce qui pourrait vouloir une carte.

Le knob : `GC_SDLOG ?= 1`.

### Premier run sur materiel reel : ce que le journal a dit (2026-09-04)

Le journal a fonctionne. Il contenait ceci, en entier :

```
=== DKR-GC ===
built Sep  4 2026 09:17:36
log   cardb:/dkr/dkr.log
boot: video ok (640x576)
boot: ultra ok
boot: gx ok
boot: pad ok
boot: assets ok
boot: game running
```

Neuf lignes, et trois faits utiles.

**1. Le canal de diagnostic marche.** `cardb:` : le lecteur SD est en slot B.
La console a bien ecrit sur la carte, avec un horodatage FAT fantaisiste
(« Sep 21 2084 ») parce que la GameCube n'a pas d'horloge que libfat sache
lire.

**2. `640x576` : la console est PAL.** 50 retraces par seconde, pas 60. Le
heartbeat bat tous les 60 VSync, donc **1,2 s** la-bas contre 1,0 s sous
Dolphin. Encore un denominateur different — voir le piege de methode du
2026-09-03, qui portait deja sur ce compteur.

**3. Le crash arrive avant le premier battement.** Aucune ligne apres
`game running`, et le .dol de la carte est bien la compilation `GC_DEBUG=1`
(md5 verifie, chaines du heartbeat presentes dans le binaire). Donc moins de
1,2 s de jeu.

**Et surtout : le rapport de plantage n'est pas arrive.** Pas de bloc `CRASH`
dans le journal, pas de `dkr.crash` a cote. L'utilisateur voyait bien le vidage
ecran de libogc, donc `__wrap_c_default_exceptionhandler` s'etait execute et
avait rendu la main — mais rien n'avait ete ecrit.

#### La cause : `MSR[EE]` est a zero dans le handler

Une exception PowerPC efface `MSR[EE]`, et le vecteur de libogc ne le remet
pas. Il n'en a pas besoin : tout ce que fait `c_default_exceptionhandler` est
en scrutation — `kprintf` vers une console framebuffer, `SI_Sync`, `PAD_Sync`,
`udelay`, puis une boucle qui interroge la manette. Verifie au desassemblage.

Ecrire un fichier ne l'est pas. libfat prend un mutex LWP par partition,
`fopen` de newlib alloue sous un autre, et les transferts EXI de la carte SD
se terminent sur une interruption. Les trois echouent ou ne se terminent
jamais avec `EE` a zero. `fopen` renvoyait NULL, `flush_locked` mettait
`sReady = FALSE` et rendait la main : exactement le journal observe.

#### Trois corrections, parce qu'il n'y a pas de run de rechange

1. **`_CPU_ISR_Enable()` avant toute E/S**, et `mtmsr` pour rendre a libogc
   l'etat que son vecteur avait laisse. Le risque assume : avec les
   interruptions rendues, l'ordonnanceur peut faire tourner d'autres threads
   pendant que celui-ci est dans le handler, sur une machine deja fautee. Ca
   vaut mieux que zero information, et les threads qui continuent (boot, audio)
   sont justement ceux dont on veut la sortie.

2. **Le mode plantage du journal.** Le handler ecrit deux cents lignes par
   `gc_logfile_printf`, chacune prenant le mutex du journal. Si le thread
   fautif etait lui-meme dans un `gc_log` — le thread de boot en train
   d'imprimer un battement, cas evident — le mutex est detenu par un thread qui
   ne le rendra jamais, et le handler se bloque des la premiere ligne. Gel
   complet, aucun rapport : strictement pire que le journal vide qu'on
   corrigeait. `gc_logfile_set_crash_mode()` arrete de prendre le verrou ; il
   ne reste qu'un ecrivain, donc le verrou ne protege plus rien.

3. **Une seconde tentative depuis le thread de boot.** Le handler pose
   `sCrashPending`, tente l'ecriture, et n'efface le drapeau que si elle a
   vraiment atteint la carte. `gc_crash_poll()`, appele a chaque retrace depuis
   `gc_main`, l'ecrit sinon — dans un contexte de thread parfaitement ordinaire.
   Le handler attend jusqu'a deux secondes que ca arrive avant de laisser
   libogc prendre l'ecran. Deux tentatives independantes, et celle qui tourne
   en contexte normal est la plus susceptible d'aboutir.

#### Et ce qui manquait pour lire un crash d'une seconde

- **Battements precoces** aux retraces 5, 15 et 30, puis la cadence habituelle.
  Trois ecritures carte de plus par demarrage, et un crash dans la premiere
  seconde laisse desormais le recensement d'opcodes, les compteurs de taches et
  l'etat audio derriere lui.
- **`gc_logfile_mark()`** : une ligne ecrite *et* vidangee immediatement, pour
  les evenements uniques qui doivent survivre a un crash une milliseconde plus
  tard. Posee sur les deux chemins que cette compilation a allumes et qui
  n'avaient jamais tourne :
  - `pfs: first osPfsIsPlug -> pack present` — repondre « oui » est ce qui fait
    entrer le jeu dans le code Controller Pak **et** rumble de `save_data.c`,
    dont rien n'avait jamais ete execute dans ce portage ;
  - `aud: first A_POLEF, reverb is running` — la reverberation etait refusee
    jusqu'a ce jour.
- **`GC_MEMCARD ?= 1`**, l'A/B en une compilation : `0` remet le jeu ou il
  etait, sans pak, donc hors de tout ce code.

**Bogue trouve en chemin :** `osMotorInit` ne renseignait pas `pfs->channel`,
alors que `osMotorStart`/`osMotorStop` adressent la manette par ce champ. Sans
consequence tant qu'`osPfsIsPlug` repondait « pas de pak » — la boucle rumble de
`save_data.c` ne tournait jamais. Elle tourne maintenant.

### Deuxieme run materiel : la console Gecko ecrivait dans la carte SD (2026-09-04)

Les balises ont fait leur travail. Le journal du second run :

```
=== DKR-GC ===
built Sep  4 2026 09:55:23
log   cardb:/dkr/dkr.log
boot: video ok (640x576)
boot: ultra ok
boot: gx ok
boot: pad ok
boot: assets ok
boot: game running
pfs: first osPfsIsPlug -> pack present
pfs: osPfsInit ch0 ok, 0/123 pages used
```

Deux lignes de plus, et elles disent : le jeu est bien entre dans le code
Controller Pak, `osPfsInit` a reussi, puis plus rien — ni le battement du tick 5
(100 ms), ni le bloc `CRASH`, **toujours pas de `dkr.crash`**.

Le rapport de plantage n'arrivait toujours pas, malgre `MSR[EE]` remis. Ce
n'etait donc pas (seulement) les interruptions.

#### La cause : `CON_EnableGecko(EXI_CHANNEL_1, FALSE)`

Presente depuis le premier jour du portage, une seule ligne, jamais suspectee
parce qu'elle est *la raison pour laquelle on peut deboguer sous Dolphin*.

**EXI canal 1, c'est le slot carte memoire B. Le lecteur SD de l'utilisateur est
un SD Gecko, en slot B** — le journal le dit lui-meme : `log cardb:`. Un USB
Gecko et un SD Gecko sont tous deux le peripherique EXI 0 de leur canal. Donc
**chaque `printf` selectionne l'adaptateur SD et lui envoie des octets de
protocole USB Gecko.** Avec `safe` a `FALSE`, libogc ne verifie meme pas qu'un
Gecko est present : il ecrit.

Sous Dolphin c'est inoffensif, et c'est ainsi qu'a ete fait tout le diagnostic
depuis le debut — le slot B de Dolphin *est* vraiment un Gecko. Sur materiel, ca
met du trafic parasite sur le bus dont le jeu demarre. Ca n'a rien casse tant
que le portage ne lisait la carte qu'au boot. **Le jour ou le journal et la
sauvegarde ont commence a s'en servir pendant que le jeu tourne, ca a cesse
d'etre theorique** : les deux runs materiel se sont termines sur une carte qui
ne pouvait plus etre ecrite, ce qui est exactement pourquoi le rapport de
plantage n'arrivait jamais.

Correction : monter libfat **avant** d'activer le Gecko, ne jamais l'activer sur
un slot que libfat detient, et passer `safe` a `TRUE` pour que libogc verifie la
presence d'un Gecko avant d'ecrire. Le boot trace affiche desormais
`fat slots 0x2 ..., gecko off (slot B is libfat)` sur cette console, et
`fat slots 0x0 ..., gecko on` sous Dolphin.

#### Trois autres corrections de la meme famille

Le meme run a rendu visible que **trois threads touchent maintenant une carte**
la ou un seul le faisait : le thread de boot vidange le journal, le thread de
jeu lit et ecrit la sauvegarde par `gc_storage.c`, et le thread de jeu depose
aussi des balises. libfat et `CARD_*` pilotent les memes canaux EXI.

1. **`gc_fs_lock()` / `gc_fs_unlock()`** (`gc_assets.c`) : un seul verrou autour
   de tout acces carte, libfat comme `CARD_*`. Grossier a dessein — un acces
   carte se compte en millisecondes et arrive aux points de sauvegarde et une
   fois par seconde, jamais par image.

2. **Plus aucune E/S depuis un thread autre que celui de boot.** Le journal ne
   vidangeait plus seulement au battement : `gc_logfile_write` vidangeait des
   que le tampon atteignait les trois quarts, et `gc_logfile_mark` vidangeait
   toujours — depuis le thread appelant. Desormais tout ecrit dans le tampon, et
   `gc_main` vidange **a chaque retrace** (gratuit quand le tampon est vide, une
   fois par seconde en pratique). Une balise atteint donc la carte en 20 ms sans
   que le thread de jeu n'ouvre jamais de fichier.

3. **`gc_storage` ne touche plus un slot que libfat detient.** `gc_fat_mount()`
   note les volumes montes (`carda:` = slot A, `cardb:` = slot B) et
   `card_slot_has_card` refuse ces canaux avant meme de sonder. Sonder le slot B
   avec `CARD_*` revient a donner au pilote de carte memoire le canal EXI que le
   pilote SD est en train d'utiliser.

#### Et de quoi trancher au prochain run

Toutes les entrees `osPfs*` posent desormais une balise a leur premier appel, et
le scheduler en pose une sur la premiere tache graphique et la premiere tache
audio. Sous Dolphin la sequence est :

```
pfs: osPfsIsPlug -> pack present
sched: first audio task
pfs: osPfsInit ch0 ok, 0/123 pages used
sched: first graphics task
pfs: osPfsFreeBlocks
pfs: osPfsNumFiles
pfs: osPfsFileState no0
```

**Lecon de methode : une balise qui n'existe que sur materiel ne peut pas etre
testee avant d'en avoir besoin.** `gc_logfile_mark` n'ecrivait au depart que
dans le fichier, donc invisible sous Dolphin, donc invérifiable — sur des
balises ajoutees precisement parce qu'un run materiel avait du etre depense pour
apprendre ce qu'un run Dolphin aurait pu dire. Elles passent maintenant aussi
par la console.

### Troisieme run materiel : ni le Gecko seul, ni le Controller Pak (2026-09-04)

Les deux .dol plantent. Journal du dernier (`dkr-nopak.dol`, `GC_MEMCARD=0`) :

```
built Sep  4 2026 10:22:37
log   cardb:/dkr/dkr.log
boot: fat slots 0x2 (bit0 carda/slotA, bit1 cardb/slotB), gecko off (slot B is libfat)
boot: video ok (640x576)
...
boot: game running
```

Deux enseignements, et une erreur a moi.

**1. `gecko off` et ca plante quand meme.** La console Gecko ecrivant dans le SD
Gecko etait un vrai bogue et reste corrigee, mais ce n'etait pas la cause.

**2. La version sans Controller Pak plante aussi.** Ce n'est donc pas le code
pak de `save_data.c` non plus.

**3. Erreur : j'avais perdu l'instrumentation.** Le run precedent avait produit
deux lignes `pfs:` ; celui-ci n'en a aucune, alors que la version sans pak en
emet une (`no pack`). La difference n'est pas dans le jeu, elle est dans le
portage : j'avais retire la vidange de `gc_logfile_mark` pour deplacer toute
l'E/S sur le thread de boot. Le raisonnement etait bon — trois threads touchaient
la carte — mais **si le thread de jeu se bloque a une priorite superieure, ou si
la machine s'arrete interruptions coupees, le thread de boot ne tourne plus et
tout ce qui reste dans le tampon est perdu.** `gc_fs_lock` etait la bonne
reponse, pas le tamponnage. La vidange immediate est revenue.

**Consequence de methode : une correction qui deplace ou l'instrument ecrit est
un changement de l'instrument.** Elle merite le meme scepticisme qu'une
correction du code mesure — et ici elle a coute un run materiel entier.

#### Ce que la compilation suivante ajoute

- **`gc_logfile_mark` vidange de nouveau depuis le thread appelant**, sous
  `gc_fs_lock`.
- **Une echelle de balises sur le chemin d'init du jeu**, tiree de `init_game`
  (`src/thread3_main.c:182`) et posee sur les points d'entree que le portage
  possede. Ordre observe sous Dolphin :

```
init: osCreateScheduler
init: first osPiStartDma (asset DMA)
init: first osAiSetFrequency
init: osContInit
pfs: osPfsIsPlug -> pack present
sched: first audio task
pfs: osPfsInit ch0 ok, 0/123 pages used
init: osEepromProbe, loading save
init: osEepromProbe done
sched: first graphics task
```

  `osEepromProbe` est marque **des deux cotes** de `save_load`, parce que ce qui
  se passe entre les deux est le code le plus recent du chemin de boot : la
  sauvegarde passe desormais par `gc_storage.c`, qui atteint une carte memoire
  et un systeme de fichiers.

- **`GC_MEMCARD=0` coupe maintenant tout le sous-systeme de stockage**, pas
  seulement le pak : `gc_storage` echoue en lecture comme en ecriture, rien ne
  sonde ni ne monte de carte memoire, et l'EEPROM vit en RAM pour la session.
  C'est exactement l'etat du portage avant le 2026-09-04, ce qui en fait une
  vraie compilation d'isolation.

### Quatrieme run : ca meurt dans la premiere display list (2026-09-04)

L'echelle de balises a fait exactement son travail. Journal materiel, build
`10:41:34` (stockage complet) :

```
boot: fat slots 0x2 (...), gecko off (slot B is libfat)
boot: game running
init: osCreateScheduler
init: first osPiStartDma (asset DMA)
init: first osAiSetFrequency
init: osContInit
pfs: osPfsIsPlug -> pack present
pfs: osPfsInit ch0 ok, 0/123 pages used
init: osEepromProbe, loading save
init: osEepromProbe done
sched: first graphics task
```

**Le jeu va nettement plus loin qu'au run precedent** — la correction Gecko et le
verrou carte ont donc bien servi — et il s'arrete sur `sched: first graphics
task`, marque emise juste avant `gc_gfx_run_dl`. La mort est donc dans la
premiere liste d'affichage, ou juste apres.

Deux details a noter : `sched: first audio task` **n'apparait jamais** alors que
sous Dolphin il precede `osPfsInit` ; et `pfs: osPfsFreeBlocks` et la suite, qui
sous Dolphin viennent apres la premiere tache graphique, manquent aussi.

**C'est une vraie exception CPU** : l'utilisateur voit le pave de registres de
libogc (confirme). Donc le handler s'execute — il n'arrive simplement pas a
ecrire.

#### PAL elimine par mesure

Seule difference systematique avec Dolphin : la console est PAL (`640x576`), et
PAL n'est pas cosmetique ici — le jeu ajoute `PAL_HEIGHT_DIFFERENCE` a chaque
resolution dans `video_init`, donc il dessine dans un espace ecran de 320x264 au
lieu de 320x240, et le mode PAL de libogc a un EFB de 528 lignes recopie vers un
framebuffer externe de 576 la ou NTSC fait 480 vers 480. **Tous** les facteurs
d'echelle entre les coordonnees du jeu et l'EFB changent.

`FallbackRegion = 2` dans Dolphin ne deplace pas `VIDEO_GetPreferredMode` hors
NTSC, donc le knob **`GC_FORCE_PAL`** a ete ajoute : il impose
`TVPal576IntDfScale` et `osTvType = OS_TV_PAL` (libogc lit le standard TV dans
les globales SRAM, pas dans le mode configure, donc forcer le mode seul
laisserait les deux moities du portage en desaccord sur la hauteur d'ecran).

**Resultat : sous Dolphin en PAL force, le jeu tourne.** 29 battements, aucune
exception, `640x576` confirme dans la trace. Le chemin PAL change bien des
choses — `trin proj: hw 141, cpu fallback 44 (no-w 44)` la ou NTSC fait
`hw 152, cpu fallback 0` — mais il ne plante pas. **PAL seul n'est pas la
cause**, et c'est une mesure, pas une impression.

#### Un defaut dans mon propre chemin de plantage

`__wrap_c_default_exceptionhandler` remettait `MSR[EE] = 0` avant d'appeler le
handler de libogc. Ca annulait tout le mecanisme de seconde chance : le vidage
de libogc **ne revient jamais**, il boucle en scrutant la manette, donc avec les
interruptions coupees aucun autre thread ne tourne plus et `gc_crash_poll` ne
pouvait par construction jamais ecrire le rapport. Les interruptions restent
desormais actives — la boucle de libogc est scrutee de toute facon — et le
thread de boot peut finir ce que le handler n'a pas pu.

Ajoute aussi : `sched: first graphics task done`, pour que le journal distingue
« mort dans la premiere liste » de « passe au-dela ».

### La carte SD corrompue, et le journal preattribue (2026-09-04)

La carte de l'utilisateur a fini corrompue au point d'exiger un reformatage,
apres plusieurs runs se terminant chacun par une exception CPU. Le journal en
est la cause plausible, et sa forme etait mauvaise.

**Ajouter etait la forme dangereuse.** Chaque `fopen(path, "a")` qui allonge le
fichier fait allouer un cluster a libfat et **reecrit la FAT**, et le portage
faisait ca environ une fois par seconde sur une machine qui meurt en cours de
route. Une faute entre la mise a jour de la FAT et celle de l'entree de
repertoire laisse le volume incoherent — pas seulement ce fichier.

**Le fichier est desormais cree une fois a sa taille definitive et ne grandit
plus.** La chaine de clusters est ecrite au boot, avant que le moindre code de
jeu ne tourne ; chaque vidange ensuite est `fopen("r+b")`, `fseek`, ecriture,
`fclose` — ca touche les *donnees* du fichier et une entree de repertoire,
jamais la table d'allocation. Le pire qu'une faute en pleine vidange puisse
faire maintenant, c'est abimer la fin du journal.

256 Ko : environ une minute de heartbeat `GC_DEBUG`, bien plus que ce qu'aucun
run n'a atteint, pour une a deux secondes de boot sur un SD Gecko. Le
remplissage est fait de sauts de ligne plutot que de zeros, pour que le fichier
s'ouvre proprement comme du texte.

**Rien de tout ceci n'est testable sous Dolphin**, qui n'a pas de carte SD
GameCube — exactement le piege qui avait deja coute un run sur
`gc_logfile_mark`. Donc chaque etape **degrade vers le chemin d'ajout qui
fonctionnait deja** plutot que vers l'absence de journal : une ecriture courte a
la creation, ou un `fopen` qui echoue a la vidange, bascule `sAppendMode` et le
portage continue a l'ancienne.

### LA cause du plantage materiel : le message de fin de tache (2026-09-04)

Trouvee sur une **photographie de l'ecran de plantage**, apres cinq runs. Le
journal a designe l'endroit, la photo a donne les registres, et la reponse etait
dans ce depot depuis le debut.

#### Ce que la photo disait

```
Exception (Floating Point) occurred!
LR 801151C8  SRR0 80115238  SRR1 00009030  MSR 00001000
DAR 00000004  DSISR 04000000
STACK DUMP:
80115238 --> 801151c8 --> 8000599c --> 8010383c --> 80097844 --> 800a5d84 --> ...
CODE DUMP:
80115238: C00A321C ...          (lfs f0, 0x321C(r10))
```

`addr2line` sur l'ELF fige :

| adresse | symbole |
|---|---|
| `80115238` | `_svfprintf_r` |
| `8000599c` | `__wrap_c_default_exceptionhandler` |
| `8010383c` | `default_exceptionhandler` |
| `80097844` | `gfxtask_wait` + 0x38, juste apres `bl osRecvMesg` |
| `800a5d84` | `main_game_loop` |
| `800a5ed0` | `thread3_main` |

Deux plantages en un, et il faut les separer.

#### Le plantage secondaire : mon gestionnaire detruisait la preuve

`SRR1 = 00009030` : `MSR_FP` (0x2000) **absent**. Une exception PowerPC efface
`MSR[FP]` et le vecteur de libogc ne le restaure pas. La toute premiere
`gc_logfile_printf` du handler atteint `_svfprintf_r` de newlib, qui touche un
registre flottant quelle que soit la chaine de format — donc **seconde
exception**, « Floating Point unavailable », qui **ecrase `SRR0`/`SRR1` avec les
siens**. Voila pourquoi cinq runs n'ont jamais produit de rapport et pourquoi
l'ecran montrait le gestionnaire de plantage en train de planter.

Corrige de deux facons, ceinture et bretelles :

- **`mtmsr(msr | MSR_FP | MSR_EE)`** en tete du handler.
- **Le rapport ne passe plus par printf du tout** : `crash_puts`, `crash_hex` et
  `crash_dec`, une trentaine de lignes, aucune dependance a la bibliotheque C.
  Le rapport est la seule chose qui ne doit pas dependre de l'etat d'une machine
  qui vient de fauter. Le `fprintf` du chemin de vidange du journal est parti
  aussi, pour la meme raison.

#### Le plantage reel : `task->msg` est NULL et le jeu le dereference

`DAR 00000004` — une lecture a l'adresse 4 — et la trame interrompue est
`gfxtask_wait`. Le desassemblage donne l'instruction :

```
80097840:  bl   osRecvMesg
80097844:  lwz  r8, 8(r1)          ; le message recu
80097854:  lwz  r3, 4(r8)          ; <-- mesg[1], adresse 4 si r8 == NULL
```

et la source (`src/rcp_dkr.c:365`) :

```c
s32 gfxtask_wait(void) {
    OSMesg *mesg = NULL;
    if (gGfxTaskIsRunning == FALSE) return 0;
    osRecvMesg(&gGfxTaskMesgQueue, (OSMesg) &mesg, OS_MESG_BLOCK);
    gGfxTaskIsRunning = FALSE;
    return (s32) mesg[1];
}
```

Le message recu etait NULL. Pourquoi : **`gfxtask_run_xbus`
(`src/rcp_dkr.c:166`), la seule fonction de soumission qui tourne — c'est elle
qui met `gGfxTaskIsRunning = TRUE` — renseigne `mesgQueue` mais ne renseigne
jamais `mesg`.** `gGfxTaskBuf` est un tableau en BSS, donc `task->msg` vaut NULL
sur *chaque* tache graphique que DKR soumet. Les deux autres soumetteurs, ceux
qui posent `dkrtask->mesg = &gGfxTaskMesgNums[0]`, sont marques `UNUSED`.

Et le vrai scheduler prevoit exactement ce cas. `libultra/src/sc/sched.c:522`,
**dans ce depot** :

```c
s32 __scTaskComplete(OSSched *sc, OSScTask *t) {
    ...
    if (t->unk68 || t->msg) {
        osSendMesg(t->msgQ, t->msg, OS_MESG_BLOCK);
    } else {
        osSendMesg(t->msgQ, &D_800DE730, OS_MESG_BLOCK);
    }
```

avec `s32 D_800DE730[] = { OSMESG_SWAP_BUFFER, OSMESG_SWAP_BUFFER }`
(`sched.c:68`). Le waiter lit le **second** mot et le passe a `fb_update` comme
`gScreenStatus`, ou `OSMESG_SWAP_BUFFER` (0) veut dire « presente cette image »
et `MESG_SKIP_BUFFER_SWAP` (8) « non ».

`run_task` faisait `osSendMesg(task->msgQ, task->msg, ...)` sans condition. Il
envoyait donc NULL, une fois par image, et le jeu le dereferencait. Correction :
la substitution ci-dessus, transcrite.

#### Pourquoi Dolphin n'a jamais rien vu

**Dolphin n'emule pas la MMU pour du homebrew.** Une lecture a une adresse non
mappee y renvoie simplement des donnees au lieu de fauter, donc `mesg[1]`
ramenait zero et le jeu continuait. Verifie deliberement : le portage a ete fait
ecrire a l'adresse 4 **et** a 0xDEADBEE0, et il a poursuivi les deux fois sans
broncher. Il a fallu `__builtin_trap()` pour obtenir une exception sous
l'emulateur.

Sur Gekko les BAT couvrent 0x80000000-0x8FFFFFFF et 0xC0000000-0xCFFFFFFF ;
l'adresse 4 n'est dans aucun, et il n'y a pas de table de pages. DSI immediat.

**C'est la limite de Dolphin comme instrument, et elle est structurelle** : ce
sur quoi le portage differe le plus du materiel — la MMU, les caches,
l'alignement, l'EXI — est precisement ce que l'emulateur est le plus indulgent a
propos. Trois defauts trouves ce jour sont dans cette categorie : la console
Gecko ecrivant dans le SD Gecko, la corruption de la FAT par ajout, et celui-ci.

#### Trois ecarts de fidelite releves en chemin, non corriges

Lus dans `libultra/src/sc/sched.c` et notes ici plutot que changes dans le meme
commit qu'une correction de plantage :

1. **`sc->frameCount` s'incremente a chaque retrace** dans l'original
   (`sched.c:369`), pas par tache comme ici.
2. **`OS_SC_LAST_TASK` differe sa reponse.** `__scTaskComplete` range la tache
   dans `sc->unkTask` et ne repond pas tant que `frameCount < 2` ; la reponse
   part plus tard, sur un retrace. C'est le cadencement a 30 images/seconde du
   jeu — le commentaire de `sched.c:370` le dit : « If you want to make the game
   60FPS, change this to 2 ». `gfxtask_run_xbus` pose bien `OS_SC_LAST_TASK`.
   Le portage repond immediatement, donc il ne plafonne pas la cadence de la
   meme facon.
3. **Le message de retrace est `sc` dans l'original**, pas `&sc->retraceMsg` —
   mais `OSSched` commence par `OSScMsg retraceMsg`, donc c'est la meme adresse.
   Verifie, aucun ecart.

### Le jeu lisait un registre materiel N64 (2026-09-04)

Correction du message de fin de tache livree, seconde photographie de l'ecran de
plantage. Le jeu **va nettement plus loin** : la trame interrompue est
`load_menu_with_level_background`, donc `gfxtask_wait` est franchi et le portage
est entre dans les menus.

```
Exception (Interrupt) occurred!
LR 800D8434  SRR0 80109E10  SRR1 0000B030  MSR 00001000
DAR A4600010  DSISR 04000000
STACK DUMP:
80109e10 --> 800d8434 --> 0000001a --> 800a4b48 --> 800a64d8 --> 800a6804 --> ...
```

| adresse | symbole |
|---|---|
| `80109e10` | `memcpy` |
| `800d8434` | `__console_write` (libogc, console.c:586) |
| `800a4b48` | `load_menu_with_level_background`, retour de `bl cam_init` |
| `800a64d8` | `main_game_loop` |

**`SRR1 = 0000B030` porte `MSR_FP`** : le gestionnaire de plantage ne se saborde
plus, la correction de la FPU tient.

#### Lire le dump correctement : ce n'etait pas la premiere exception

`SRR0` pointe sur `lwz r7,4(r4)` dans `memcpy`, avec `r4` dans le framebuffer
non cache — un chargement qui ne peut pas fauter. Et `DAR` ne correspond pas a
`r4`. Le nom de l'exception est « Interrupt », qui n'ecrit ni `DAR` ni `DSISR`.
**Donc `DAR` est perime, et il vient de l'exception d'avant.**

`0xA4600010` = `PHYS_TO_K1(PI_BASE_REG + 0x10)` = **`PI_STATUS_REG` du N64**.

Et la pile nomme le coupable : `cam_init`. `src/camera.c:148` fait
`WAIT_ON_IOBUSY(stat)` avant de lire le mot de cartouche que son controle
anti-piratage veut. Le macro (`include/PRinternal/piint.h:127`) tourne sur
`IO_READ(PI_STATUS_REG)` jusqu'a ce que le PI se declare libre. `IO_READ` est
`*(vu32 *) PHYS_TO_K1(addr)` : un dereferencement d'un registre memoire-mappe du
N64. Les BAT du Gekko couvrent `0x80000000-0x8FFFFFFF` et
`0xC0000000-0xCFFFFFFF`, il n'y a pas de table de pages, donc `0xA4600010` n'est
mappe nulle part et la toute premiere lecture est un **DSI**.

Verifie dans le binaire : `8001bcac: lis r7,-23456` puis `ori r7,r7,16` donne
`r7 = 0xA4600010`, suivi de `lwz r9,0(r7)` / `andi. r9,r9,3` / `bne` — la boucle
d'attente, en clair, dans `cam_init`.

#### Correction : les registres N64 sont repondus, pas dereferences

`platform/gc/include/PR/rcp.h` reprend le vrai en-tete par `#include_next` puis
redefinit `IO_READ` et `IO_WRITE` vers `gc_io_read` / `gc_io_write`
(`platform/gc/gc_n64io.c`). Le code du jeu n'est pas touche, tous les sites
d'appel restent intacts, et la decision se place la ou vivent deja les autres
substitutions materielles.

Ce que le portage repond, et d'ou ca vient — meme principe que `D_B0000578` dans
`stubs.c`, ou le controle passe *parce que la reponse est juste*, pas parce
qu'il a ete neutralise :

| registre | reponse | pourquoi |
|---|---|---|
| `PI_STATUS_REG` | `0` | Il n'y a pas de PI et aucun DMA cartouche en vol : `osPiStartDma` est un transfert ARQ deja termine quand il rend la main. « Libre » est la verite sur cette machine, et toute autre reponse bloquerait `WAIT_ON_IOBUSY` a jamais. |
| `SP_DMEM_START` | `0xFFFFFFFF` | Ce que `drm_validate_dmem` attend d'un N64 demarre. |
| `SP_IMEM_START` | `6102` | Le CIC, comme `osCicId` dans `ultra/os_system.c`. |
| `0x200`, `0x284` | mots ROM | Les deux autres controles anti-piratage. |
| tout le reste | `0`, **compte** | Un registre non modelise doit apparaitre dans le heartbeat comme un nombre, pas sur une television comme une exception. |

Les quatre derniers sont sous `#ifdef ANTI_TAMPER`, que `Makefile.gc` ne definit
pas — ils sont repondus quand meme pour qu'activer le drapeau ne soit pas une
nouvelle facon de planter.

Verification apres reconstruction complete : **zero immediat `0xA4600000` dans
tout le binaire**, et deux appels a `gc_io_read` (les deux `IO_READ` du macro).
Le heartbeat porte `n64 io: N reads, N writes, N unknown (last %08x)`.

**Piege de dependances :** ajouter un en-tete que le suivi `-MF` ne connaissait
pas ne reconstruit rien. Le premier build apres la creation du shadow avait
encore `lis r7,-23456` dans `cam_init`. Un ajout d'en-tete de shadow demande un
`rm -rf build/gc/{src,platform,libultra}`.

#### Et une correction de ma correction : ne plus laisser tourner la machine

Le handler laissait `MSR[EE]` actif avant de passer la main au vidage de libogc.
Ce raisonnement etait bon tant que le handler etait peu fiable — il donnait au
thread de boot une seconde chance d'ecrire le rapport — et il a cesse de l'etre
des que le handler a ete repare. Ce qu'il coutait, en attendant, c'est la preuve :
le vidage de libogc **ne revient jamais**, il boucle en scrutant la manette, donc
avec les interruptions actives tous les autres threads continuent sur une machine
deja fautee — et **l'exception suivante dessine son propre vidage par-dessus le
premier**.

C'est exactement ce que la photo montrait : un « Exception (Interrupt) » dans la
console framebuffer, sur le thread de jeu, alors que `DAR` portait encore le
`A4600010` du DSI qui avait tout declenche. Un plantage, un rapport, et plus rien
qui tourne ensuite pour l'ecraser. La FPU reste active : libogc s'apprete a
formater un ecran de registres.

#### Note ouverte : la console framebuffer coute tres cher

Le desassemblage de `__console_write` donne son defilement :
`src = destbuffer + stride*16`, `len = stride*con_yres - 16`. Avec le PAL
640x576 de l'utilisateur, **737 264 octets de `memcpy` non cache par ligne
defilee** — et `gc_log` en imprime une soixantaine par battement. C'est aussi
une lecture de 20 Ko *au-dela* du framebuffer, parce que `CON_Init` recoit
`ystart = 20` et `yres = xfbHeight` : la console deborde de 20 lignes en bas.
A corriger (geometrie, et sortir la console du framebuffer que GX possede), mais
pas dans le meme commit qu'un plantage.

### Le jeu tourne, et le rapport de plantage marche enfin (2026-09-04)

Sixieme run. Le journal contient **plusieurs battements** puis un bloc `CRASH`
complet -- le premier que ce portage ait jamais reussi a ecrire. Trois choses en
sortent.

#### 1. Le portage passe le boot et dessine

`n64 io: 2 reads, 0 unknown` : le spin `WAIT_ON_IOBUSY` de `cam_init` passe
desormais par `gc_io_read`, et rien d'autre ne touche un registre N64. Le
heartbeat montre `game 320x264` (les +24 lignes PAL du jeu), `am frameSamples
880` (22050*2/50, l'audio suit le 50 Hz tout seul), des display lists reelles,
`fills 2 ... at (0,0)-(319,263)`.

#### 2. Ce qui l'arretait, c'etait moi

```
=================== CRASH ===================
cause    mempool_alloc_safe failed: 0 bytes, tag 0x7f7f7fff
```

`0x7F7F7FFF` est **`COLOUR_TAG_GREY`** (`src/memory.h:54`) -- un tag
parfaitement ordinaire, pas de la memoire corrompue. C'est donc une demande
d'allocation de **zero octet**, depuis l'un des trois sites de
`asset_loading.c`, c'est-a-dire un asset dont la taille enregistree est nulle.

Et le jeu tolere ca. `dump_memory_to_cpak` (`src/thread0_epc.c:160`) n'ecrit son
fichier et ne boucle **que** `if (get_filtered_cheats() &
CHEAT_EPC_LOCK_UP_DISPLAY)`. Sans le cheat -- donc toujours, en session normale
-- la fonction entiere est un no-op et `mempool_alloc_safe` poursuit vers
`mempool_slot_find`.

Le portage, lui, appelait `gc_fatal` a tous les coups. J'avais ecrit dans le
commentaire que c'etait deliberate ; c'etait un changement de comportement que
ce portage n'avait aucune raison de faire, et il a coute un run. **Le rapport
reste, l'arret disparait**, et il est ecrit une seule fois pour qu'un appelant
en boucle ne remplisse pas la carte.

Le rapport nommait aussi `caller 00000000`, aussi inutile que ca en a l'air :
il enregistrait `epc`, que `mempool_alloc_safe` construit comme
`stack_pointer()->sp`. Il enregistre desormais
`__builtin_return_address(1)` -- le code de jeu qui a demande l'allocation --
et `(0)`, l'interieur de `mempool_alloc_safe`.

#### 3. La console framebuffer coute la moitie de la machine

`clock: 759 ms since last beat` au premier battement, puis **1659 ms** et
**1600 ms** une fois la console remplie et en train de defiler. Soixante
retraces valent 1200 ms sur une console 50 Hz.

Le desassemblage de `__console_write` donne le defilement :
`src = destbuffer + stride*16`, `len = stride*con_yres - 16`. En PAL 640x576
cela fait **737 264 octets de `memcpy` non cache par ligne defilee**, dans le
framebuffer meme que GX recopie -- et le heartbeat `GC_DEBUG` imprime une
soixantaine de lignes par battement.

Deux corrections :

- **La geometrie.** `CON_Init(fb, 20, 20, rmode->fbWidth, rmode->xfbHeight, ...)`
  -- les arguments canoniques de libogc -- decrit une region qui deborde de
  vingt lignes en bas du framebuffer. Le defilement lit alors 25 Ko *au-dela* de
  la fin du tampon a chaque ligne. Soustraire les marges rend ca impossible.
- **L'extinction.** Apres `gc_gfx_init`, GX possede l'ecran : la console est du
  bruit en ecriture seule, que personne ne peut lire, et sur cette console il
  n'y a pas non plus d'USB Gecko (slot B = carte SD). Le portage **cesse
  d'imprimer** -- `gc_console_set(FALSE)`, respecte par `gc_log`,
  `gc_logfile_mark`, `osSyncPrintf` et `rmonPrintf`. Le journal SD n'est pas
  concerne : il n'est jamais passe par printf.

`gc_fatal` et le rapport de plantage continuent d'imprimer : ils sont terminaux,
rares, et l'ecran est le seul endroit ou un utilisateur sans lecteur de carte
les verra.

### `__assert` : le meme nom, trois arguments, l'ordre inverse (2026-09-04)

Septieme run. Deux resultats, dont une confirmation nette.

#### La console eteinte rend la machine a sa vitesse

    clock: 1200 ms since last beat (60 VSyncs)
    clock: 1199 ms since last beat (60 VSyncs)

Soixante retraces valent exactement 1200 ms sur une console 50 Hz. Le run
precedent en donnait 1659 et 1600. **La console framebuffer coutait bien la
moitie de la machine.** (Les battements de boot -- ticks 5, 15 et 30 -- ne sont
pas espaces de 60 VSync, donc leurs valeurs plus courtes sont normales ; c'est
l'etiquette de la ligne qui est trompeuse pour eux.)

#### Le rapport d'allocation nomme enfin son appelant

    cause    mempool_alloc_safe failed: 0 bytes, tag 0x7f7f7fff
    caller   80012cbc   <- addr2line this
    inside   80032ad0

`80012cbc` est **`asset_table_load`**, `80032ad0` est `mempool_alloc_safe`. Donc
la table d'assets est chargee avec une taille nulle a un moment. Le jeu le
tolere (voir plus haut), c'est note et ca ne l'arrete plus.

#### Et le vrai defaut : deux `__assert` differents portent le meme nom

La pile du plantage :

```
memcpy <- __console_write <- ... <- alAudioFrame <- __amMain <- thread_trampoline
```

Le thread **audio**, dans le `memcpy` de la console. Ce qui imprime, c'est
`__assert` -- et il y en a deux :

| declaration | ordre des arguments |
|---|---|
| `libultra/src/debug/assert.h` (le decompile) | `(const char *exp, const char *filename, int line)` |
| `powerpc-eabi/include/assert.h` (newlib) | `(const char *file, int line, const char *failedexpr)` |

Meme nom, trois arguments chacun, **ordre different**. Sans definition propre,
le portage resolvait les appels du jeu vers la fonction de newlib. Donc
`__assert("samples >= 0", "env.c", 104)` arrivait avec `file = "samples >= 0"`,
`line` = l'adresse de `"env.c"` imprimee en decimal, et
`failedexpr` = **l'entier 104**, que newlib passe a `fiprintf("%s")` : une
lecture de l'adresse 104. Et apres avoir imprime, l'assert de newlib appelle
`abort()`.

Trois appels de ce genre existent dans le binaire lie, tous dans
`libultra/src/audio/env.c` (lignes 107, 109 et 378), et le decompile dit
lui-meme ce qu'ils sont : « Something must have gone wrong when compiling this
file, and the asserts got left in ». Ils sont ecrits
`if (cond) {} else { __assert(...); }` plutot que par le macro, donc `-DNDEBUG`
ne les supprime pas. `env.c` tourne sur le thread audio, une fois par voix et
par trame.

**Correction :** `platform/gc/gc_assert.c` definit `__assert` avec l'ordre du
jeu. Il enregistre l'echec dans le journal SD, **une fois par site**, et
**retourne**. Retourner est delibere, et c'est la deuxieme fois de la journee
que ce portage doit l'apprendre : rendre fatale une condition que l'original
tolerait est un changement de comportement, et `dump_memory_to_cpak` qui
s'arretait sur une allocation de zero octet avait deja coute un run. Une
assertion « restee la par accident » est un diagnostic, pas un contrat.

Le journal dira laquelle des trois se declenche -- et si c'est le cas, c'est un
signal direct sur l'audio, parce que les trois portent sur ce que l'envmixer
recoit.

#### Et la console, pour de bon

`gc_console_set(FALSE)` ne fait taire que les `printf` du portage. Il ne peut
rien contre newlib : un assert, un `fprintf` du jeu, n'importe quoi qui atteint
`stdout` ou `stderr` finit dans `__console_write`. Apres `gc_gfx_init`, la
console est donc **repointee sur un tampon prive de 320x32** (20 Ko). Chaque
ecrivain coute desormais vingt kilo-octets au lieu de sept cents, et plus rien
n'ecrit dans le framebuffer que GX possede. Un USB Gecko, quand il y en a un,
recoit toujours le texte complet : libogc l'emet depuis `__console_write` quel
que soit le tampon.

### Un asset ne se decompresse pas (2026-09-04, ouvert)

Huitieme session. Le jeu tourne -- c'est la premiere ou c'est vrai -- et le
journal contient un signal neuf :

```
gzip: inflate -3, 2 of 1256016605 bytes, head dd 4a dd 4a dd 0a
```

`-3` est `Z_DATA_ERROR`. `1256016605` est `0x4ADD4ADD`, et la tete affichee est
`dd 4a dd 4a dd 0a` : **les quatre octets lus comme taille non compressee sont
le meme motif repete que les donnees**. Autrement dit `compressedInput` ne
pointe pas sur ce que `gzip_inflate` attend -- il n'y a ni prefixe de taille ni
flux DEFLATE a cet endroit.

Le second symptome de la meme session va dans le meme sens : le rapport
d'allocation nomme

```
cause    mempool_alloc_safe failed: 0 bytes, tag 0x7f7f7fff
caller   80012dfc     -> asset_table_load
```

`asset_table_load` demande **zero octet**, ce qui veut dire que la taille qu'il
a lue dans la table est nulle. Une table lue de travers et un asset qui n'est
pas la ou on le croit sont la meme hypothese.

**Ce que ce n'est probablement pas :** une decompression cassee en general. Le
jeu affiche ses textures et son geometrie, `tex asks = hits + converts` est
propre, et `zlib` a decompresse des centaines d'assets depuis le 2026-08-31. Ce
qui echoue, c'est un asset particulier, ou un offset particulier.

**Instrument ajoute avant toute hypothese** -- le message porte desormais
l'adresse source et l'appelant :

```c
gc_log("gzip: inflate %d, %lu of %lu bytes, src %08x, from %08x, head ...")
```

`src` se compare directement a l'image d'assets en ARAM (qui *est* la ROM,
octet pour octet, sans rebasage -- voir « Assets »), et `from` se resout a
`addr2line`. Avec les deux, la question « quel asset, a quel offset » devient
arithmetique au lieu de conjecturale.

`aram reads 1095, slow 1022, contended 0` : 93 % des lectures passent par le
tampon de rebond, ce qui est normal, et aucune collision. Le chemin ARAM n'est
donc pas en cause a priori -- mais l'adresse le dira.

#### Ce que la neuvième session a établi (2026-09-04, hors console)

Le journal du run avec `src` et `from` a donné :

```
gzip: inflate -3, 2 of 1256016605 bytes, src 80efcb80, from 80062ad0, head dd 4a dd 4a dd 0a
```

`80062ad0` est `object_model_init +408` (vérifié par `nm`, pas seulement par
`addr2line`) : c'est la ligne

```c
gzip_inflate((u8 *) compressedData, (u8 *) objMdl);   // src/object_models.c
```

`80efcb80` tombe bien dans `gMainMemoryPool` (`80daa4a0`..`811aa4a0`).

**Quatre faits, tous mesurés hors console contre la ROM elle-même** (elle est
identique octet pour octet à `dkr.assets` -- `md5 4f0e07f0…` des deux côtés) :

1. **Le format du conteneur est juste.** Les 390 modèles d'objets se
   décompressent **tous** hors ligne avec exactement ce que fait `gc_gzip.c` :
   taille non compressée en petit-boutien sur 4 octets, en-tête de 5 octets,
   puis un flux DEFLATE brut. Les longueurs obtenues correspondent au champ de
   taille pour les 390. `GZIP_HEADER_SIZE 5` est donc confirmé, et le
   cinquième octet vaut 0x09 partout ; le bloc DEFLATE qui suit est un bloc
   final à Huffman dynamique dans tous les cas.
2. **`dd 4a dd 4a dd 0a` n'existe nulle part dans les 12 Mo de la ROM.** Même
   `dd 4a dd` n'y est pas. Le tampon ne contient donc pas *un autre asset* ni
   *le bon asset à un mauvais offset* : il ne contient rien qui soit sorti de
   l'image d'assets.
3. **La table des modèles d'objets est saine.** 391 entrées puis `0xFFFFFFFF`,
   aucune entrée de longueur nulle, aucune de longueur négative. Le retour
   anticipé `if (size == 0)` d'`asset_load` n'est donc pas atteignable par un
   `modelID` légitime.
4. **Le `mempool_alloc_safe(0)` du rapport de plantage n'est pas un défaut.**
   `80012e28` est `asset_table_load +84`, et la table d'assets de la ROM US 1.0
   contient **trois sections de longueur nulle** : `SCREENS`, `EMPTY_14` et
   `EMPTY_37`. Le jeu en charge une à chaque démarrage. Voir la section
   suivante : c'est le rapport qui était en tort, pas le jeu.

Il reste donc trois possibilités, et une seule mesure les sépare : la lecture
qui devait remplir ce tampon **n'a jamais eu lieu** (ou a atterri ailleurs) ;
elle a eu lieu et l'ARAM répond correctement maintenant (donc quelque chose a
écrasé le tampon, ou le cache rend une ligne périmée) ; elle a eu lieu et
l'ARAM répond la même chose (donc l'image en ARAM est fausse à cet offset).

**L'instrument qui tranche, ajouté pour le run suivant :**

- `gc_assets.c` garde un anneau des 32 dernières lectures (offset ROM,
  destination, longueur, chemin direct ou rebond, numéro d'ordre) et
  `gc_assets_find_read()` retrouve celle dont la destination couvre une adresse
  donnée. Quatre mots par lecture, aucune E/S, donc gardé hors `GC_DEBUG`.
- En cas d'échec, `gzip_inflate` nomme cette lecture **et relit l'ARAM au même
  offset**, puis imprime les deux :

```
gzip: filled by read #N: rom XXXXXXXX -> YYYYYYYY, L bytes, bounce path; src is +K into it, so rom ZZZZZZZZ
gzip: aram now says xx xx xx ...
```

  ou, si rien n'a rempli ce tampon :

```
gzip: no asset read covers 80efcb80 (N reads served) -- nothing ever filled this buffer
```

- `gc_assets_verify()`, au démarrage : relit **toute** l'image depuis l'ARAM et
  compare son empreinte à celle prise du côté source pendant le téléversement.
  Une ligne dans le journal, `identical` ou `*** DIFFERENT ***`. Environ 0,4 s
  une fois au boot. L'hypothèse « l'image en ARAM est fausse » n'a jamais été
  vérifiée depuis le début du portage ; elle l'est maintenant, avant toute
  conjecture.

### `osInvalDCache` jetait les données du voisin (2026-09-04)

Trouvé en lisant le chemin d'`asset_load` pour l'asset qui ne se décompresse
pas. Ce n'est **pas** démontré comme étant sa cause -- c'est un défaut distinct,
de la même classe que les quatre défauts matériels de la veille (MMU, EXI,
caches, alignement), et invisible sous Dolphin.

`dmacopy_internal` (`src/asset_loading.c`, code du jeu, inchangé) commence par :

```c
osInvalDCache((u32 *) ramAddress, numBytes);
```

Les deux arguments n'obéissent à **aucune** règle d'alignement :
`object_model_init` lui passe `objMdl + modelSize - compressedSize` et un
nombre d'octets lu tel quel dans la table des assets. Sur MIPS c'était gratuit :
la plage était arrondie aux lignes de cache et le jeu était seul propriétaire
de la mémoire.

Ici, non. Le portage traduisait cet appel en `DCInvalidateRange`, c'est-à-dire
`dcbi`, qui **jette une ligne de 32 octets sans la réécrire**. Les deux lignes
aux extrémités d'une plage non alignée sont partagées avec ce que l'allocateur
a distribué de part et d'autre : jusqu'à 31 octets d'un voisin vivant et
récemment écrit repartaient silencieusement à ce que la mémoire principale
contenait avant. À chaque chargement d'asset. Sans aucune trace.

`platform/gc/ultra/os_cache.c` écrit maintenant les deux lignes partielles en
mémoire (`dcbf` réécrit *et* invalide, ce qui est correct pour les deux
propriétaires) et n'invalide que les lignes que la plage possède entièrement.

Le chemin rapide de `gc_assets_read` garde `DCInvalidateRange` : là, l'offset,
la destination et la longueur sont tous alignés sur 32 par construction, donc
aucune ligne n'est partagée et l'invalidation est exactement la bonne opération.

**La leçon, la même que celle du canal de debug :** demander ce qu'une
opération *touche*, pas seulement ce qu'elle fait. `dcbi` ne fait pas ce que le
nom de l'appel libultra laisse croire — il ne « rafraîchit » pas une plage, il
détruit ce qu'il recouvre.

### Le rapport d'allocation nulle mangeait le seul rapport disponible (2026-09-04)

`dump_memory_to_cpak` était appelé dans deux cas très différents et les
traitait pareil :

```c
void *mempool_alloc_safe(s32 size, u32 colourTag) {   // src/memory.c
    if (size == 0) dump_memory_to_cpak(...);          // toléré
    addr = mempool_slot_find(POOL_MAIN, size, colourTag);
    if (addr == NULL) dump_memory_to_cpak(...);       // vraie panne
    return addr;
}
```

Le premier cas se produit **à chaque démarrage** : la ROM US 1.0 a trois
sections d'assets de longueur nulle et `asset_table_load` en charge une. Il
écrivait donc le rapport binaire complet sur la carte, ce qui affichait une
page `CRASH` au démarrage suivant pour une condition que le jeu tolère — et,
bien pire, il verrouillait `sReported`, donc **une vraie pénurie de mémoire
plus loin dans le run n'aurait rien écrit du tout**. L'instrument se
consommait lui-même sur un faux positif garanti.

Le cas `size == 0` est maintenant séparé : une ligne dans le journal, une seule
fois, avec l'appelant, et rien sur la carte. Le rapport complet et la page
d'écran restent pour ce qu'ils décrivent — `mempool_slot_find` qui rend NULL.

C'est le troisième piège d'instrumentation de ce portage, après la console
framebuffer qui coûtait la moitié de la machine et le journal qui corrompait la
carte SD. **Un instrument qui se déclenche à coup sûr sur une condition normale
n'est pas un instrument.**

### Le gestionnaire de plantage, porte (2026-09-04)

`platform/gc/gc_crash.c`. DKR en avait un : `src/thread0_epc.c` fait tourner un
thread de priorite 0 en attente de `OS_EVENT_FAULT`, ecrit le contexte du
thread fautif dans un fichier « CORE » sur le Controller Pak, et boucle. Au
**boot suivant**, `get_lockup_status` retrouve ce fichier, le relit, l'efface,
et `thread3_main` affiche le vidage de registres en quatre pages.

La forme est bonne et elle est conservee. Trois substitutions font tout le
travail :

| N64 | portage |
|---|---|
| vecteurs d'exception MIPS | les vecteurs PowerPC que libogc installe deja |
| `OS_EVENT_FAULT` | `c_default_exceptionhandler`, intercepte a l'edition de liens |
| le Controller Pak | la carte SD : rapport lisible dans `dkr.log`, enregistrement binaire dans `dkr.crash` |

**Comment l'interception fonctionne, et pourquoi pas autrement.** libogc recopie
un stub sur chaque vecteur d'exception, et ce stub finit par `mtsrr0 ; rfi` vers
l'adresse trouvee dans `_exceptionhandlertable`. Les entrees de cette table sont
donc des **routines de vecteur brutes**, pas des fonctions C : enregistrer une
fonction C via `__exception_sethandler` y sauterait sans aucune trame sauvee.
C'est le desassemblage de `exception.o` qui a tranche, et ce n'est pas ce que
raconte l'extrait qui circule dans le homebrew.

Le crochet se place donc un niveau au-dessus. La routine de vecteur de libogc,
`default_exceptionhandler`, sauve la `frame_context` complete puis fait **un
appel C ordinaire** a `c_default_exceptionhandler`, depuis un autre fichier
objet. `-Wl,--wrap=c_default_exceptionhandler` intercepte exactement cela :
`__wrap_...` ecrit le rapport puis passe la trame a `__real_...`, donc le vidage
ecran de libogc a lieu aussi. Rien n'est reimplemente, rien n'est patche a
l'execution.

Ce que le rapport contient : l'exception nommee, `srr0`/`srr1`/`dsisr`/`dar`,
`lr`/`ctr`/`cr`/`xer`, les 32 GPR, la pile brute filtree sur ce qui ressemble a
une adresse de retour (a passer a `powerpc-eabi-addr2line -e build/gc/dkr.elf`),
et **`gObjectStackTrace`** — quel objet le jeu etait en train de creer, mettre a
jour ou dessiner. Ce dernier point est la seule information de niveau jeu qu'un
vidage de registres possede, et le talon precedent l'enregistrait dans le vide.

Les sept points d'entree que le jeu appelle encore sont maintenant reels :
`update_object_stack_trace` (neuf sites dans `objects.c`), `stack_pointer` (r1),
`thread3_verify_stack` (la sentinelle de pile du portage, signalee une fois),
`dump_memory_to_cpak` (echec d'allocation : rapport nomme au lieu d'un plantage
mysterieux), `get_lockup_status`, `mode_lockup`, `render_epc_lock_up_display`.

### Le Controller Pak et la carte memoire (2026-09-04)

`platform/gc/ultra/os_pfs.c` et `platform/gc/gc_storage.c`. Les douze `osPfs*`
renvoyaient `PFS_ERR_NOPACK` ; le jeu tournait mais ne proposait jamais
d'enregistrer un fantome.

**L'API est emulee, pas le support.** Rien hors de libultra ne lit la table
d'inodes d'un pak : `save_data.c` ne touche qu'un seul champ d'`OSPfs`
(`status & PFS_INITIALIZED`, ligne 1184) et passe par les douze appels pour tout
le reste. L'implementation est donc un repertoire et un allocateur de pages. La
comptabilite des pages est exacte (123 pages de 256 octets), parce que le jeu
montre a l'utilisateur combien il reste et refuse un fantome qui ne tient pas.

**Le pak est un seul blob, pas seize fichiers de carte.** Une carte memoire
GameCube alloue par blocs de 8 Ko ; seize fantomes de quelques kilo-octets
prendraient 128 Ko et seize entrees de repertoire pour un seul jeu. L'image de
32 Ko part donc en un unique fichier de sauvegarde.

`gc_storage.c` est la porte commune : un blob nomme, sur carte memoire (les deux
slots) s'il y en a une, sur carte SD sinon. **L'EEPROM y passe aussi** — ce que
la section « Reste a faire » annoncait, et c'etait exact : seuls `save_load` et
`save_store` ont change, quatre lignes chacun.

`gc_storage_present()` (la reponse a `osPfsIsPlug`) est mise en cache une
seconde : `save_data.c` la demande au fil des menus, et un `CARD_ProbeEx` sur les
deux slots par image mettrait une transaction EXI dans le temps de trame pour
aucune information nouvelle.

**`CARD_ProbeEx`, pas `CARD_Probe`.** libogc definit `CARD_Probe` comme un
simple branchement vers `EXI_Probe` — verifie en desassemblant `card.o` — donc
il repond « un peripherique EXI est attache », ce qui sur le materiel de
l'utilisateur est tres probablement un SD Gecko en slot B et non une carte
memoire. `CARD_ProbeEx` lit l'identite du peripherique et renvoie
`CARD_ERROR_WRONGDEVICE` pour tout ce qui n'est pas une carte. Se tromper la
n'aurait pas perdu de donnees — le repli SD aurait rattrape l'ecriture — mais
`osPfsIsPlug` repond au jeu avec cette valeur, et annoncer un Controller Pak
parce qu'un *lecteur* de cartes est branche est exactement le genre de
quasi-succes qu'on ne diagnostique plus ensuite.

### Instrumentation ajoutee le 2026-09-02

- **`cover`** -- la primitive la plus large de la frame, en pour-mille d'ecran,
  quel que soit son espace d'origine : triangle materiel, triangle du repli
  CPU, texrect ou fill sont comparables. Avec son rang dans la liste, son
  combineur, son mode de rendu, sa texture et les couleurs PRIM/ENV. Les
  egalites vont a la primitive la plus tardive, parce que ce qui est au-dessus
  est ce qui a ete dessine en dernier. C'est elle qui a nomme le rectangle
  ci-dessus en une execution.
- **`tex magenta`** -- combien de textures sont passees par les deux chemins
  qui peignent du magenta (format sans cas, palette absente). `unhandled 0,
  no tlut 0` a elimine toute la piste « texture » pour l'ecran magenta, en une
  ligne.
- **`ignored:`** -- le recensement des opcodes que le walker jette. Voir
  « La mesure d'avancement » plus haut.

### Corrections du 2026-09-01

Dans l'ordre de ce qu'elles ont rapporte.

**1. Le signe du back-face culling etait inverse, sur les deux chemins.** Le
port rejetait les faces avant et gardait les faces arriere. `GC_NO_CULL=1` a
fait gagner ses nuages au ciel, sa ligne d'arbres a l'horizon et a fait cesser
au terrain d'etre un coin brun plat -- toute cette geometrie etait jetee.

Les deux chemins etaient coherents **entre eux** et tous deux faux, ce qui est
la raison pour laquelle les comparer ne prouvait rien : le chemin logiciel
mesure l'aire en espace ecran (y vers le bas) et rejetait `area <= 0` ; le
chemin materiel la mesure en NDC (y vers le haut) et rejetait `area >= 0`. Une
symetrie en y nie une aire signee, donc ces comparaisons opposees sont la meme
regle ecrite deux fois. Mesure : `tris in 923 -> cull 267, out 656` est devenu
`tris in 1143 -> cull 280, out 863`.

**2. Le cache de textures se pietinait.** `tex asks 243 = hits 153 +
converts 90` contre `TEX_CACHE_SIZE 64`, evince en tourniquet : au moins 26
emplacements etaient reutilises **deux fois dans la meme frame**. Chaque
manque faisait `free()` + `memalign()` + reecriture pendant que le CPU court
devant le GP a travers le FIFO -- donc des tampons liberes et reecrits pendant
que le GP les lisait encore. Cache a 256 entrees, tampons conserves quand ils
sont deja assez grands (champ `capacity`) : `tex asks 286 = hits 286 +
converts 0 | held 805 KB`.

**3. `load_3d_projection` renvoyait FALSE sans charger de projection.** Le
repli CPU soumettait alors des coordonnees ecran pendant que le GP tenait
encore la matrice **perspective** du lot precedent. C'est le « grand aplat de
couleur » poursuivi depuis le debut. Il appelle desormais `load_2d_projection()`.

**4. `GX_InvalidateTexAll()` n'etait appele nulle part.** `DCFlushRange` pousse
la copie du CPU dehors ; rien n'invalidait le cache de textures du GP lui-meme.
Appele apres chaque conversion et, avec `GX_InvVtxCache()`, par liste.
(`ref-sm64gc/src/pc/gfx/gfx_gx.c:1809` le fait a chaque frame.)

**5. `G_SETCOMBINE` -> TEV, avec le mode de rendu et les couleurs constantes.**
Decodage generique des seize champs du mux plutot qu'une table des dix-sept
modes `G_CC_*` -- verifie contre des mots reels releves sur la machine :
`cc fc127eac f00ff23f` se decode en `G_CC_MODULATEIDECALA` +
`G_CC_BLENDI_ENV_ALPHA_PRIM2`, exactement la paire de `src/textures_sprites.c`.
Quatre formes de TEV couvrent tous les combineurs de DKR. PRIM et ENV vivent
dans `GX_TEVREG0/1` parce qu'un etage peut lire plusieurs registres mais une
seule konst ; un combineur deux-cycles gare le resultat du cycle 1 dans
`GX_TEVREG2` pour que `COMBINED` ne soit pas ecrase par les etages du second.

**`G_RDPSETOTHERMODE` (0xEF) est la façon dont DKR pose son etat de rendu**, et
non `G_SETOTHERMODE_H/_L` : chaque entree de table de dessin est un
`gsDPSetCombineLERP` suivi d'un `gsDPSetOtherMode` (`src/textures_sprites.h`).
Le port n'avait aucun cas pour 0xEF, donc le type de cycle, le filtre de
texture et tout le mode de rendu n'etaient jamais lus. En deux cycles ce sont
les muxes du **second** cycle qui ecrivent en memoire -- DKR appaire
`G_RM_FOG_SHADE_A` en cycle 1 avec le vrai mode en cycle 2 dans toutes ses
tables.

**6. Resolution d'adresse : pas de decoupage par octet de poids fort.**
`set_rsp_segment.h` dit sans detour « they don't use segments for assets », et
`thread3_main.c:258` met le segment 0 a `K0BASE`. Le `table[addr >> 24] +
(addr & 0xFFFFFF)` du RSP est sans perte sur N64 parce que la RDRAM fait 4 Mo ;
ici les assets sont en `.rodata`, `gMainMemoryPool` est lie a `0x80d87800` et
court jusqu'a `0x81187800` -- 25 bits physiques. Tout ce qui depassait
`0x81000000` (38 % du pool) etait lu comme segment 1 avec le bit 24 perdu.
Desormais `sSegments[0] + addr`, pleine largeur.

**7. Pipeline de textures, entierement deduit du code du jeu.** Le pas de la
source est le `line * 8` de la tuile, pas `width * bpp` (les macros de
chargement alignent les lignes sur huit octets ; `width * bpp` cisaille toute
texture 4 bits dont la largeur n'est pas multiple de 16) ; le 32 bits est
l'exception a `width * 4` parce que son `LINE_BYTES` vaut 2. Les chargements
sont enregistres contre leur **adresse TMEM** et une tuile trouve ses texels
par la sienne -- l'idiome charge par la tuile 7 et dessine par la tuile 0.
CI4/CI8 a travers une TLUT, et **la palette n'arrive pas en `G_LOADTLUT`** :
`gbi.h:3360` redefinit `gDPLoadTLUT_pal16` en un `G_LOADBLOCK` ordinaire vers
`tmem = 256 + pal*16`. `uls`/`ult`, `shifts`/`shiftt` et l'echelle S/T de
`G_TEXTURE` atteignent maintenant les coordonnees.

L'echange de mots des lignes impaires en 32 bits est implemente (a appliquer
exactement quand le `dxt` du chargement par bloc vaut 0) -- **mais il ne se
declenche jamais en pratique** : tout chargement mesure a `dxt != 0`, `swap0`.
Ne pas y revenir.

**8. `G_VTX_APPEND` partait de la mauvaise base.** f3ddkr.h : un chargement
avec le drapeau pose est « appended after those written **with flag 0** » --
apres le compte du dernier chargement non-appendant, pas apres le cumul
courant. Le constructeur de sprites appende par groupes de cinq quads et remet
son propre index a zero entre les groupes (`textures_sprites.c:1399`), donc
chaque groupe doit retomber a l'indice 1 par-dessus le precedent ; cumuler
mettait le second groupe a 21 alors que ses triangles referencaient encore
1..20. Tout sprite de plus de cinq tuiles etait faux. Mesure : la boite ecran
d'un lot est passee de `(7883,568)-(9365,767)` a `(86,46)-(184,58)`.

**9. La convention de profondeur du repli CPU etait inversee.** Le chemin
materiel met le plan proche a -1 et le plan lointain a 0 (la plage propre a
GX) ; `project_corner` envoyait `ndc * 0.5 + 0.5`, et `gfx_ortho` avec n = -1,
f = 0 donne `mt[2][2] = -1` : la profondeur composee valait 0 au plan proche et
-1 au lointain, la meme plage lue a l'envers. Sous `GX_LEQUAL` un sprite
lointain recevait la profondeur la plus proche qui soit et passait devant tout.
Tous les sprites passent par ce chemin, d'ou des sprites -- et seulement eux --
devant le personnage. Desormais `0.5 - ndc * 0.5`, et le biais de decal change
de signe avec.

### Textures : modes d'adressage, filtrage, bord (2026-08-31)

Le descripteur de tuile n'etait lu qu'a moitie. `G_SETTILE` ne gardait que
`fmt` et `siz` et jetait `cms`/`cmt`, `masks`/`maskt`, `shifts`/`shiftt`, et
`texture_get` codait en dur `GX_CLAMP, GX_CLAMP` et `GX_NEAR, GX_NEAR`.

Trois consequences, toutes visibles :

1. **Tout etait borne.** Mesure du masque `(cmt<<2)|cms` sur une frame :
   `0x401` et `0x005`, soit les bits 0, 2 et 10 -- **WRAP/WRAP** (le cas
   majoritaire), CLAMP/CLAMP et CLAMP/WRAP. Chaque surface repetee (route, eau,
   parois) montrait donc une colonne de bord etiree au lieu de se repeter.
2. **Rien n'etait filtre.** DKR demande explicitement
   `gsDPSetTextureFilter(G_TF_BILERP)` dans `rcp_dkr.c`, `menu.c`, `printf.c`,
   `weather.c` et `fade_transition.c`. `G_SETOTHERMODE_H` etait ignore en bloc ;
   le port suit maintenant le registre (decalage en bits 8..15 de w0, longueur
   en 0..7, `w1` deja en place) et lit `G_MDSFT_TEXTFILT`.
3. **Le remplissage de bord etait du noir transparent.** GX range le RGBA8 en
   blocs de 4x4, donc une texture dont la taille n'est pas multiple de 4 a des
   texels au-dela de son bord. Ils etaient a zero ; en bilineaire, ca tire un
   liseré sombre dans la derniere ligne et la derniere colonne. Le bord est
   desormais replique -- le swizzle de `ref-sm64gc` porte la meme note.

Le cache de textures porte maintenant l'etat d'echantillonnage dans sa cle, et
sur un succes avec des modes differents il redecrit l'objet GX en place plutot
que de reconvertir les memes texels.

Table clamp/mirror/wrap, reprise de `gfx_gx_cm_to_gx` : le clamp l'emporte sur
le mirror, sinon mirror, sinon repeat.

### LA cause racine (2026-08-31) : signed/unsigned dans `load_matrix`

Une ligne. Le decodage virgule fixe des matrices faisait :

```c
s32 whole = (s16) src[i * 4 + j];
u32 frac  = src[16 + i * 4 + j];
dst[i][j] = (f32) ((whole << 16) | frac) / 65536.0f;   /* faux */
```

`frac` est **non signe**, donc C convertit les deux operandes vers le type
commun -- `unsigned int` gagne -- et `(whole << 16) | frac` vaut `unsigned`.
Convertir ca en float lit tout coefficient negatif comme 2^32 plus lui-meme :
**-2,45 revient a 65533,55**. Chaque element negatif d'une matrice devenait un
enorme positif.

Ca explique tout ce qu'on a poursuivi pendant deux jours : la geometrie
projetee tres loin de l'ecran (les grands aplats de couleur), les profondeurs
ou `z/w` collait au plan lointain pour tout, les modeles absents.

**Et ca s'est tres bien cache.** La matrice billboard de `mtxf_billboard` n'a,
dans le cas courant, aucun element negatif -- elle se decodait donc
parfaitement pendant que toutes les matrices de camera a cote d'elle etaient
fausses. Le defaut ressemblait a un probleme de billboard.

Correctif :

```c
s32 fixed = (s32) (((u32) whole << 16) | frac);
dst[i][j] = (f32) fixed / 65536.0f;
```

Comment il a ete trouve, et c'est la lecon de methode : en **dumpant la
matrice decodee** au lieu de raisonner dessus. Quatre echantillons du `w` de
l'ancre d'un sprite donnaient 1310802, 17104740, 3473462, 1572941 ; divises par
65536 : **20, 261, 53, 24** -- des entiers exacts, quatre fois de suite. Une
telle coincidence n'existe pas. Le dump de la matrice a ensuite montre des
lignes entieres a `65533,5` et `64319,4`, soit `65536 - petite valeur`.

Resultat : l'ecran de selection affiche Conker sur son kart devant le canyon,
l'eau, le sable et son ombre portee ; le survol d'introduction affiche Pipsy
sur l'eau avec l'ile, la plage, le ponton et les vagues ombrees. Les replis
CPU tombent a `cpu fallback 0` sur la plupart des frames.

### `G_MW_BILLBOARD` : actif (GC_BILLBOARD=1)

L'ancre est le **sommet 0**, lu dans le jeu et non deduit du header : les deux
sites d'emission (`camera.c:1157` et `camera.c:1240`) la poussent avec
`gSPVertexDKR(..., 1, 0)` -- un sommet, `G_VTX_APPEND` a zero, donc index 0 --
*avant* de charger le slot 2 et *avant* d'activer le billboarding. Le
commentaire du decomp a cote de la branche « vehicle part » le dit d'ailleurs :
« sprite vertices are hardcoded to start from index 1 ». La mesure confirme :
le lot de sprite arrive en `n=4, append=1, base=1`.

Ecrit une premiere fois, ca semblait degrader fortement l'image et ca avait ete
desactive. C'etait un faux proces : `load_matrix` corrompait les coefficients
negatifs, donc les coordonnees de clip de l'ancre etaient absurdes et les
additionner a quoi que ce soit faisait un carnage. Le bug corrige, le
billboarding se comporte et il est actif par defaut.

### Correction du 2026-08-31 : la division perspective passe au GP

Le renderer divisait par `w` sur le CPU et donnait a GX des coordonnees ecran.
Ca marche, mais ca coute deux choses qui se voient :

- les coordonnees de texture s'interpolent alors **en affine**, lineairement en
  espace ecran et non en profondeur, donc les grandes surfaces s'etirent -- le
  ciel et le sol tordus du survol d'introduction ;
- un sommet derriere l'oeil doit etre decoupe a la main, et ce que la decoupe
  maison rate est **retourne par la division** et projete au loin, trainant son
  triangle en **grand coin plat**.

Ces deux defauts ont un nom et une cause connue dans `ref-sm64gc` : la legende
de son `GFX_GX_DEBUG_PROJ_TINT` decrit le chemin CPU comme « no near-plane
clipping, affine interpolation -- **produces both the wedge and the smear** ».
C'est mot pour mot ce qu'on avait a l'ecran.

Leur correction est de nourrir le GP en espace vue et de le laisser diviser.
GX ne prend que trois composantes de position, donc `w` doit sortir de la
matrice de projection -- et la forme perspective de GX n'est pas une 4x4
generale : le materiel ne garde que six coefficients, donc `clip.z` ne peut
etre qu'une fonction **affine** de `clip.w`. Eux doivent l'ajuster par moindres
carres a chaque lot, parce que `gfx_pc` leur donne des sommets deja projetes et
aucune matrice a lire.

**Nous avons la matrice, donc on calcule les coefficients exactement.**
`guPerspectiveF` (libultra/src/gu/perspective.c) ecrit `mf[2][2] = (n+f)/(n-f)`,
`mf[2][3] = -1`, `mf[3][2] = 2nf/(n-f)`. En vecteurs-lignes, la colonne j
produit la composante j, et la modelview premultipliee est affine (quatrieme
colonne (0,0,0,1)). En developpant le produit :

    clip.z = alpha * clip.w + beta
    alpha  = M[i][2] / M[i][3]        pour i = 0, 1, 2
    beta   = M[3][2] - alpha * M[3][3]

Verification : en substituant z = -n puis z = -f, la profondeur normalisee vaut
-1 puis +1 -- c'est la convention OpenGL. GX veut -1 au plan proche et **0** au
plan lointain (le port SM64 l'etablit noir sur blanc), donc
`z_gx = (z_gl - 1)/2`, et comme GX calcule `clip.z' = P22*z_vue + P23` avec
`z_vue = -w` :

    P22 = (1 - alpha) / 2       P23 = beta / 2

Le viewport passe aussi a GX, lu du `G_MOVEMEM` et mis a l'echelle de l'EFB.
GX et le N64 placent tous deux y normalise = +1 en haut, donc aucune inversion
n'est necessaire : la negation explicite du chemin logiciel faisait le meme
travail a la main.

Mesure : la decoupe logicielle ne se declenche plus du tout (`clip 0`), aucun
sommet ne ressort derriere l'oeil, et beaucoup de frames tournent a
`trin proj: hw 71, cpu fallback 0`.

### Les deux projets de recompilation PC : rien a recuperer

`ThatGuyMcd/DKR-R` et `Rainchus/Donkey-Kong-64-Recompiled` sont des
**recompilations statiques** adossees a RT64. Verifie plutot que suppose :
DKR-R contient `RecompiledRSP/` et `rsp/`, et **zero occurrence de `G_TRIN`**.
Ils recompilent le microcode RSP lui-meme en code natif et le laissent produire
des commandes RDP que RT64 consomme -- ils n'ont donc jamais eu besoin d'une
table de traduction F3DDKR, qui est precisement le probleme qu'on resout ici.
C'est une approche inverse de la notre et elle ne se transpose pas sur
GameCube. La source utile reste `ref-sm64gc`.

### Correction du 2026-08-31 : le depth buffer n'etait jamais vide

**C'est la correction qui a debloque le rendu 3D**, et elle vient du portage
SM64 de `ref-sm64gc` -- `gfx_ogc_copy_to_xfb` dans `src/pc/gfx/gfx_ogc.c`, dont
le commentaire de `gfx_gx_start_frame` decrit exactement le piege :

> The EFB -> XFB copy has to force `GX_SetZMode(GX_TRUE, ..., GX_TRUE)` and
> `GX_SetColorUpdate(GX_TRUE)`, otherwise `GX_CopyDisp` does not clear the EFB.

L'argument « clear » de `GX_CopyDisp` ne vide que ce que l'etat d'ecriture
courant l'autorise a ecrire. Or **toute** display list du port se termine par
`gfx_set_2d_state`, qui pose `GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE)` : au
moment de la copie, les ecritures de profondeur sont desactivees, donc la
couleur etait bien effacee et **la profondeur jamais**.

Le symptome n'est pas un ecran noir, il est bien plus trompeur : le depth
buffer s'accumule d'une frame a l'autre. La geometrie 3D, qui teste
`GX_LEQUAL`, perd contre des profondeurs ecrites par des frames precedentes
depuis une autre position de camera, et disparait -- pendant que le chemin 2D,
qui ne teste pas la profondeur, continue de dessiner. D'ou l'image constatee
pendant des jours : **fonds unis et texte toujours la, ciel, decor et modeles
absents ou clignotants**.

`gc_gfx_copy_display` force desormais les deux mises a jour le temps de la
copie, et fait un `GX_DrawDone` avant elle. Resultat immediat, sans aucune
autre modification : l'ecran-titre affiche son decor 3D complet, le survol
d'introduction affiche iles, ciel et nuages, et **les modeles de personnage
s'affichent** sur l'ecran de selection.

Lecon a retenir : sur GX, l'etat de rendu au moment de la copie fait partie de
la copie. Un `GX_CopyDisp(xfb, GX_TRUE)` ne suffit pas a garantir un EFB propre.

### Correction du 2026-08-31 : `G_MTX` selectionne le slot qu'il charge

Le port traitait `gSPMatrixDKR` comme un chargement pur, et
`gSPSelectMatrixDKR` comme le seul moyen de rendre un slot courant. C'est la
lecture evidente des deux noms, et elle est fausse. Trois sites d'appel de
`camera.c` ne s'expliquent que dans l'autre sens :

- `mtx_cam_push` (1429) televerse le MVP propre a **chaque objet** dans le slot
  1 et ne le selectionne jamais. Sans selection implicite, l'objet etait
  dessine avec la matrice laissee par le decor -- et c'est exactement ce qui se
  passait : les modeles sortaient a `w <= 0` et etaient jetes comme « derriere
  la camera ».
- `mtx_pop` (1552) restaure la transformation parente en la **rechargeant**
  dans le slot 1 ; si charger ne selectionnait pas, depiler ne ferait rien.
  Son autre branche, pile vide, utilise bien un `select` explicite.
- `mtx_head_push` (1520) charge la matrice de tete dans le slot 2 puis
  reselectionne le slot 1. Ce `select` n'est necessaire que parce que le
  chargement vient de rendre le slot 2 courant.

`gSPSelectMatrixDKR` sert donc a revenir a un slot deja televerse, pas a armer
un televersement.

Mesure sur une frame **identique** (memes 181 commandes `G_TRIN`, memes 1141
triangles demandes), avant / apres : triangles emis 543 -> 667, jetes au plan
proche 167 -> 142, et surtout la repartition des sommets par slot
1628/28/0 -> 317/1139/200. Les slots 1 et 2 ne servaient pratiquement a rien.

### Instrumentation : l'entonnoir des triangles

`GC_DEBUG=1` recense maintenant, par frame, le sort de chaque triangle demande
par `G_TRIN` : indices hors bornes, jetes au plan proche (en distinguant
« tous les sommets derriere l'oeil » de « trop pres, dans `NEAR_W` »), coupes
par le test de face arriere, ou reellement emis. Plus, par slot de matrice, les
sommets transformes, ceux qui sortent derriere l'oeil, les chargements et les
selections.

« Rien ne s'affiche » a quatre causes tres differentes et indiscernables a
l'image ; une seule execution dit maintenant laquelle, au lieu d'un build par
hypothese. C'est ce qui a localise le bug ci-dessus : le slot 2 recevait 16 a
18 matrices par frame sans jamais transformer un seul sommet.

`GC_NO_CULL=1` (nouveau reglage de `Makefile.gc`) dessine les deux
enroulements. Le depot dit quelle valeur du drapeau `Triangle` signifie
« dessiner la face arriere » mais jamais quel enroulement est la face avant :
desactiver la coupe est le seul moyen de distinguer une surface absente parce
que coupee d'une surface jamais soumise.

### Deux hypotheses eliminees (2026-08-31)

Le modele de personnage manquant sur l'ecran de selection avait deux suspects.
Les deux sont ecartes, avec preuve :

- **`obj_animate`, non.** `model_instance_init` (object_models.c:270-291)
  recopie les sommets du modele dans `vertices[0]` *et* `vertices[1]` a la
  creation, et `animationTaskNum` vaut 0. Avec le stub, le personnage devrait
  s'afficher en pose de repos, fige mais visible. Sa valeur de retour n'est lue
  par aucun des trois sites d'appel.
- **L'enroulement des faces, non.** Build avec `GC_NO_CULL=1` : le personnage
  reste absent alors que plus rien n'est coupe.

La vraie cause etait ailleurs, et la piste `mtx_ortho` notee ici d'abord etait
mauvaise : c'etait le depth buffer jamais vide (section ci-dessus). Les ~50 %
de sommets du slot 0 qui ressortent derriere l'oeil sur les frames de menu sont
normaux -- le decor entoure la camera.

### Décompression des assets (fait)

`gzip_inflate_block` etait un stub qui rendait 0, donc `gzip_inflate` bouclait
zero fois et **tout asset compresse ressortait vide** : polices, textures,
modeles, circuits. Le jeu tournait sa boucle complete sur du neant, ce qui
donne exactement l'aspect d'un renderer qui ne marche pas.

`src/gzip.c` rejoint `GAME_EXCLUDE` et `platform/gc/gc_gzip.c` le remplace en
s'appuyant sur zlib, deja present dans les portlibs et deja lie. C'est le
meilleur rapport travail/resultat du portage : l'inflate d'origine est celui de
Mark Adler (le `gzip_huft_build` du depot en est la copie mot pour mot), donc
zlib decode exactement le meme flux, et les 781 lignes de MIPS de
`src/hasm/gzip_asm.s` n'ont pas a etre traduites.

Le conteneur : quatre octets de taille decompressee en little-endian, un octet,
puis du DEFLATE brut -- d'ou `inflateInit2(&strm, -MAX_WBITS)`. Ce n'est pas un
fichier gzip : ni magie `1f 8b`, ni CRC final. Le decalage de 5 octets vient du
commentaire de `src/gzip.c` lui-meme, et les deux sites d'appel le confirment.

Resultat mesure : **zero erreur d'inflate** sur un run complet, et la liste
d'affichage passe de 332 commandes a 1906, profondeur de pile 1 a 3, avec les
formats de texture RGBA16 / RGBA32 / IA16 / IA8 qui apparaissent enfin. L'ecran
n'est plus noir mais bleu -- la couleur que le jeu demande vraiment.

### Audio : le silence plutôt que les bips

*(Historique, garde parce que le raisonnement resservira.)*
`gc_audio_run_cmds` parcourait la liste de commandes et la jetait. Le tampon
remis ensuite a `osAiSetNextBuffer` ne contenait donc pas du silence mais ce que
l'allocateur y avait laisse, rejoue soixante fois par seconde : une tonalite
continue. `audio_mixer.c` exportait `gGcAudioMixerImplemented = 0` et l'etage de
sortie emettait du silence, tout en consommant et cadençant le meme nombre de
frames -- l'horloge du gestionnaire audio est ce qui rythme le jeu.

**Depuis le 2026-09-03 le drapeau vaut 1 et les opcodes sont ecrits.** Voir
« Reste a faire » : le mixeur tourne mais sa sortie est fausse, et la ligne
`aud` du heartbeat est ce qui doit etre lu en premier.

### Géométrie (fait) — l'écran-titre s'affiche

Logo, menu START/OPTIONS et le decor 3D texture derriere. `G_MTX`, `G_VTX`,
`G_TRIN`, le viewport via `G_MOVEMEM` et la selection de matrice par
`G_MOVEWORD` sont implementes. Zero liste abandonnee, zero erreur d'inflate.

Rien n'a ete suppose ; chaque champ vient du depot :

- **La matrice est un MVP complet.** camera.c:1418 fait
  `mtxf_mul(model, viewProj, out)`, et `mtxf_mul` (src/hasm/math_util.c) vaut
  `out[i][j] = somme_k a[i][k]*b[k][j]`. Avec les vecteurs-lignes du N64, cela
  signifie `clip = v * M` : **la colonne j de M produit la composante j**.
- **Le format virgule fixe** vient de `mtxf_to_mtx` : parties entieres des seize
  elements dans les huit premiers mots, parties fractionnaires dans les huit
  derniers, l'element (i,j) a l'index 16 bits `i*4+j` de chaque bloc.
- **`guPerspectiveF`** (libultra/src/gu/perspective.c) place `-1` en mf[2][3],
  donc w vaut `-z` et un sommet de `w <= 0` est derriere la camera.
- **Le viewport se lit, il ne se suppose pas** : `viewport_rsp_set` le reecrit
  par camera et l'ecran partage en depend. Deux bits de fraction (`Vp_t`).
- **Les encodages** viennent des macros : `gSPVertexDKR` met
  `(n-1)<<3 | (adresse&6) | append` dans l'octet de parametre, donc
  `n = (p>>3)+1` et l'append est le bit 0 ; `gSPPolygon` met
  `(numTris-1)<<4 | texEnabled` en bits 16..23 ; `gSPMatrixDKR` decale
  l'emplacement de six dans ce meme octet, donc bits 22..23.
- **Le seul point que le depot ne dit pas** est le signe de l'axe y du
  viewport. Il est negatif ici, et cela a ete tranche en regardant l'image, pas
  affirme.

**Bug trouve en lisant : l'index de `G_MOVEWORD` est en bits 0..7, pas 16..23.**
`gMoveWd` est `gImmp21(pkt, G_MOVEWORD, offset, index, data)`, et gImmp21 place
p0 au bit 8 et p1 au bit 0. L'ancien code lisait l'index la ou un *Dma1p* range
son octet de parametre : `G_MW_SEGMENT` ne correspondait jamais et la table de
segments restait vide. Invisible jusqu'ici parce que `rsp_segment` de DKR passe
`base + K0BASE`, donc toutes les adresses de la liste sont absolues.

**Profondeur, decoupe au plan proche et faces arriere sont maintenant en
place.** Les sommets restent en espace de clip et la division perspective a
lieu a l'emission, parce que la decoupe doit passer avant : diviser par un w
negatif retourne un triangle qui traverse le plan proche au lieu de le
raccourcir. Sutherland-Hodgman contre le seul plan `w >= 1.0` ; les cotes sont
laisses au scissor de GX. Trois sommets en donnent au plus quatre, emis en
eventail. La 3D a ses propres formats de vertex (POS_XYZ) et
`GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE)` ; les rectangles d'interface gardent
le chemin plat sans profondeur, ce qui correspond aux cycles fill et copy du
RDP plutot que d'etre seulement moins cher.

Le near/far de la projection valent -1 et 0 volontairement : cela rend la ligne
z `clip.z = -z_in`, donc une profondeur fournie en 0..1 ressort en 0..-1, la
plage que ce meme `gfx_ortho` produit pour une boite ordinaire. Quel que soit
le bout que GX considere comme proche, les valeurs tombent dans la plage. Ne
pas supposer quelles entrees de la projection GX lit, ni le sens de son z
normalise : l'en-tete ne documente ni l'un ni l'autre.

Deux conventions tranchees en regardant l'image, pas affirmees : le signe de y
du viewport (verifie, le logo est a l'endroit) et le sens d'enroulement des
faces avant (**pas encore verifie**, aucun modele ne s'affiche encore).

Restent ouverts : `G_MW_BILLBOARD` est un stub, et **le modele du personnage ne
s'affiche pas** sur l'ecran de selection alors que son nom et le ciel texture
le font. Piste principale, avec preuve et non supposition : `objects.c:3454`
appelle `obj_animate(obj)` pour les modeles `MODELTYPE_ANIMATED` juste apres
avoir pose `obj->curVertData`, et `obj_animate` est un des stubs. L'autre piste
est l'enroulement non verifie ci-dessus ; se teste en desactivant la coupe.

### Rectangles textures (fait)

`G_TEXRECT` dessine. L'ecran de selection de personnage affiche son texte dans
la police du jeu, degrade, contour et ombre portee compris, sur le fond bleu.

**TMEM n'est pas emule.** Le RDP chargeait les texels dans 4 Ko de TMEM puis les
relisait a travers un descripteur de tuile ; reproduire ça fidelement voudrait
dire reproduire TMEM. Rien ici n'en a besoin : chaque texture que DKR dessine
est chargee juste avant d'etre utilisee, par la sequence fixe `G_SETTIMG`,
`G_SETTILE`, `G_LOADBLOCK`, `G_SETTILE`, `G_SETTILESIZE`, `G_TEXRECT`. Donc
l'adresse du dernier `G_SETTIMG` qu'un *chargement* a effectivement consomme
est la texture, le descripteur de tuile dit comment la lire, et
`G_SETTILESIZE` dit sa taille. C'est le raccourci que prend tout renderer N64
de haut niveau ; il ne tombe que sur l'idiome « charger une fois, dessiner
plusieurs fois depuis des coins differents de TMEM », qui est un idiome 3D et
non un idiome de rectangle.

Detail qui compte : l'adresse est retenue au **chargement**, pas au
`G_SETTIMG`. La liste pose l'image, decrit une tuile de chargement, charge,
*puis* decrit la tuile a travers laquelle elle va reellement dessiner -- et les
deux descripteurs ne s'accordent pas, volontairement.

**Tout est converti en `GX_TF_RGBA8`.** Les formats en presence (RGBA5551,
RGBA8888, IA16, IA8) auraient chacun un format GX plus etroit, et l'auront un
jour, mais une cible unique en 32 bits ne laisse qu'un seul pavage a ne pas se
tromper. GX range le RGBA8 en blocs de 4x4 texels sur 64 octets : seize paires
AR, puis seize paires GB -- ce n'est donc jamais un memcpy, quelle que soit la
concordance de boutisme.

Le cache de textures est indexe sur l'adresse **et sur une empreinte des
texels**, parce que le jeu decompresse ses textures dans un tas qu'il reutilise :
une adresse seule n'identifie pas une texture.

`G_TEXRECT` est la seule commande de trois mots de la liste. Ses coordonnees de
texture vivent dans les deux `G_RDPHALF` qui la suivent, consommes par le
walker lui-meme : les laisser au `switch` dessinerait le rectangle sans texture
puis executerait deux non-ops.

### Le socle de rendu

`GC_DEBUG=1` fait afficher, une fois par seconde, le recensement complet de la
derniere display list : nombre de commandes, sous-listes `G_DMADL`, profondeur
de pile, histogramme par opcode, rectangles remplis avec leur couleur et leurs
coins, et les mots bruts du dernier `G_SETTILE` / `G_SETTILESIZE` / `G_TEXRECT`.
C'est ce qui a redirige le travail vers la 2D au lieu de la geometrie.

### Corrigé récemment

Quatre bugs se presentaient tous de la meme façon : un ecran noir immobile.

- **Le scheduler diffusait le retrace a tous ses clients.** libultra fait ça,
  mais Rare a ajoute un champ `id` a `OSScClient` (le commentaire de
  `include/PR/sched.h` le dit) et route dessus, parce que DKR enregistre un
  client dont la file est le bouton reset : `is_reset_pressed()` considere
  *n'importe quel* message arrivant sur `gNMIMesgQueue` comme un appui. Le jeu
  se croyait donc reinitialise des la premiere frame et partait dans son
  `while (1) ;`. Le retrace va desormais a tout le monde sauf au client
  PRE_NMI, et PRE_NMI seulement a lui.
- **`segmented_to_virtual` jetait les pointeurs absolus.** Masquer l'octet de
  poids fort en numero de segment transforme `0x80d36030` en segment 0, offset
  `0x00d36030` : le walker sautait en memoire basse, y lisait des zeros et les
  decodait en `G_NOOP` indefiniment -- dans le thread du scheduler, ce qui
  emporte toute la machine. Un octet de poids fort superieur au nombre de
  segments signifie maintenant « adresse deja absolue », et une adresse
  physique (le RSP adressait la RDRAM physiquement) est ramenee dans MEM1.
- **`G_DMADL` etait traite comme un `G_DL`.** Ce n'est pas un appel : d'apres
  `gDkrDmaDisplayList` dans `include/f3ddkr.h`, les bits 16..23 portent un
  *nombre de commandes* (et les bits 0..15 le meme nombre en octets), et le bloc
  vise n'a pas de `G_ENDDL` -- `dDialogueBoxDrawModes` dans `src/font.c` est un
  `Gfx[2]` nu. Le RSP en televersait exactement ce nombre en DMEM, les
  executait, puis reprenait la suite. Lire ces bits comme le drapeau « push » de
  `G_DL` faisait passer pour un saut tout `G_DMADL` de compte non nul : le
  walker ne revenait jamais dans la liste appelante et finissait par lire des
  donnees. Le walker porte desormais une borne de fin par niveau de pile, et
  verifie le compte contre la longueur en octets.
- **Le walker de display list n'avait aucune borne.** Il en a une maintenant,
  plus une validation de la cible de chaque saut : une liste qu'il ne comprend
  pas coute une frame et une ligne de diagnostic, au lieu de figer la console.
- **Le thread de jeu tournait sur 64 Ko de pile.** `GC_GAME_STACK_SIZE` (256 Ko)
  etait declare mais jamais utilise : `osCreateThread` allouait la taille
  generique a tout le monde. Chaque pile porte desormais une sentinelle.

Deux autres, sans rapport avec l'ecran noir :

- `osSetIntMask` reactivait les interruptions au lieu de restaurer l'etat
  precedent, ce qui casse les sections critiques imbriquees d'`audiosfx.c`.
  Il rend et reprend maintenant le niveau d'IRQ de libogc.
- `gc_assets.c` passait `NULL` a `ARQ_PostRequest`, que libogc dereference
  immediatement, et appelait `AR_Alloc` apres un `AR_Init(NULL, 0)` qui ne lui
  laisse nulle part ou noter quoi que ce soit. Le port gere lui-meme sa region
  ARAM, ce qui est le mode documente quand on initialise ainsi.
- Le suivi des dependances d'en-tetes ne marchait pas : `-MF $(BUILD)/$*.d`
  vivait dans un `CFLAGS` a affectation simple (`:=`), donc `$*` etait evalue
  sans radical et toutes les unites ecrivaient dans `build/gc/.d`. Les objets
  ne se reconstruisaient jamais sur changement d'en-tete.

### Terminé et vérifié

La couche de portage, fichier par fichier. Tous compilent sans erreur, et la
colonne de droite dit ce que chacun remplace.

| Domaine | Fichier | Correspondance |
|---|---|---|
| Threads | `ultra/os_thread.c` | LWP, avec sémaphore de démarrage pour la sémantique « créé mais arrêté », piles allouées ici (celles du jeu sont dimensionnées pour MIPS) |
| Messages | `ultra/os_message.c` | `MQ_*` de libogc, handle rangé dans `mtqueue`, `validCount` maintenu car `save_data.c` le lit |
| Scheduler | `ultra/os_sched.c` | exécution synchrone des tâches, messagerie préservée |
| Temps | `ultra/os_time.c` | time base Gekko remis à l'échelle du compteur N64 (rapport exact 125/108) |
| Cache | `ultra/os_cache.c` | `osWritebackDCacheAll` est un no-op assumé : plus aucun coprocesseur ne lit la mémoire hors cache |
| VI | `ultra/os_vi.c` + `gc_video.c` | `VIDEO_*`, retrace en callback, cadencement `fb_update` reproduit tel quel |
| DMA assets | `ultra/os_pi.c` + `gc_assets.c` | ARQ vers l'ARAM, avec bounce buffer pour les transferts non alignés sur 32 octets |
| Manettes | `ultra/os_cont.c` | `PAD_*` ; N64 Z → gâchette L, N64 L → bouton Z, boutons C → stick C |
| Sauvegarde | `ultra/os_eeprom.c` | EEPROM 4 kbit → un blob de 512 octets via `gc_storage.c` |
| Audio (sortie) | `ultra/os_ai.c` | AI 48 kHz + rééchantillonnage linéaire depuis le taux du jeu |
| Système | `ultra/os_system.c` | globales de boot, `osVirtualToPhysical` en identité (le mixeur tourne sur le CPU et doit pouvoir déréférencer) |
| Audio (mixage) | `audio/audio_mixer.c` | les quinze opcodes de l'ABI, `A_POLEF` compris |
| Controller Pak | `ultra/os_pfs.c` | l'API du pak émulée sur une image de 32 Ko, 123 pages de 256 octets |
| Stockage | `gc_storage.c` | un blob nommé : carte mémoire (deux slots) puis carte SD. EEPROM et pak y passent tous les deux |
| Plantages | `gc_crash.c` | vecteurs PowerPC via `-Wl,--wrap`, rapport sur la carte, rejeu à l'écran au boot suivant |
| Journal | `gc_logfile.c` | `sd:/dkr/dkr.log`, fichier de 256 Ko preattribue au boot : les vidanges ecrasent des octets et ne touchent jamais la FAT |
| Registres N64 | `gc_n64io.c` + `include/PR/rcp.h` | `IO_READ`/`IO_WRITE` repondus au lieu d'etre dereferences ; l'inconnu est compte, pas faute |
| Assertions | `gc_assert.c` | `__assert` avec l'ordre du decompile, journalise une fois par site et retourne |

### Reste à faire

Mis à jour le **2026-09-04**, après sept sessions sur console. L'ordre est celui
que la mesure dicte ; chaque poste porte le chiffre qui le justifie.

**Fait le 2026-09-04.** Sections dédiées plus haut pour chacune :

- **`a_interleave`**, la cause racine de la saturation audio : la moitié de
  chaque trame était du bruit non atténué. Mesuré après : `clipped 0..64/18000`
  par tâche, contre un pic collé à 32767 avant. **Pas encore jugé à l'oreille.**
- **`A_POLEF`** (quinzième et dernier opcode audio), donc **réverbération
  rallumée** (`GC_AUDIO_FX ?= 1`).
- **`G_PERSPNORMALIZE`** et **`G_SETBLENDCOLOR`** : `ignored:` et `aud-ign:`
  sont **tous deux vides**.
- **Le journal sur carte SD**, préalloué pour ne jamais toucher la FAT.
- **Le gestionnaire de plantage**, porté sur les vecteurs PowerPC — et rendu
  incapable de fauter lui-même (`MSR_FP`, aucun printf).
- **Le Controller Pak** et **la carte mémoire**, EEPROM comprise.
- **Le message de fin de tâche du scheduler** — première cause du plantage
  matériel : le portage renvoyait `task->msg`, NULL pour toute tâche graphique
  de DKR, et le jeu le déréférençait.
- **Les registres matériels N64** (`gc_n64io.c`) — `cam_init` lisait
  `PI_STATUS_REG` en dur, non mappé ici. Seconde cause.
- **`__assert`** (`gc_assert.c`) — même nom que celui de newlib, trois arguments
  chacun, ordre inverse ; le troisième était déréférencé comme une chaîne.
- **Le coût de la console framebuffer** : 686 Ko de `memcpy` par ligne défilée,
  dans le framebuffer que GX possède. Mesuré à 1659 ms pour 60 retraces avant,
  1200 après — la valeur exacte d'une console 50 Hz.
- **La corruption de la carte SD** : le journal s'allongeait à chaque vidange et
  réécrivait la FAT une fois par seconde sur une machine qui mourait en cours de
  route. Le fichier est préattribué maintenant.

`platform/gc/stubs.c` ne contient plus aucun `PORT-TODO`.

---

**0. Le run du 2026-09-04 (build `0a8d733e`) : ce qu'il a dit.** La console
démarre, dessine et tient la cadence — `clock: 1200 ms`, `180 retr`, `129`
images, `ignored:` et `aud-ign:` vides, `asserts 0`, `n64 io: 3 reads, 0
unknown`. Le bloc `CRASH` de la carte était le faux positif d'allocation nulle,
pas un plantage. Le seul signal réel est le `gzip: inflate -3`, une fois, au
bout de ~3,5 s, dans les menus. Build suivant : `cba85be2`.

**1. VÉRIFIER QUE LA CONSOLE DÉMARRE.** Deux causes racines corrigées coup sur
coup : le message de fin de tâche (le jeu déréférençait NULL à chaque image) et
la lecture d'un registre matériel N64 dans `cam_init`. Le second run a montré le
jeu arrivant jusqu'aux menus, donc le premier correctif tient. C'est le prochain
run qui dit si le second suffit.

Si ça plante encore, le journal doit contenir un bloc `CRASH` complet — le
gestionnaire ne peut plus se saborder et plus rien ne tourne après lui pour
écraser l'écran — et `n64 io: ... N unknown (last %08x)` dans le heartbeat nomme
tout registre N64 que le portage ne modélise pas encore.

**2. L'asset qui ne se décompresse pas.** Le run avec `src` et `from` a donné
`object_model_init +408`, et une mesure hors ligne contre la ROM a éliminé
trois hypothèses d'un coup : le format du conteneur est juste (les 390 modèles
se décompressent hors ligne), la table des modèles est saine, et les octets vus
(`dd 4a dd 4a dd 0a`) **n'existent nulle part dans la ROM**. Le tampon n'a donc
jamais reçu d'octets d'asset. Trois possibilités restent et l'instrument du
prochain run les sépare : l'anneau des lectures dit si la lecture a eu lieu, la
relecture ARAM dit si l'image est bonne à cet offset, et `gc_assets_verify()`
dit au démarrage si l'image entière est intacte. Voir « Un asset ne se
décompresse pas », sous-section du 2026-09-04.

*(Le `mempool_alloc_safe(0)` qui accompagnait ce défaut **n'en est pas un** :
trois sections d'assets de la ROM sont vides par construction. Le rapport de
plantage ne le signale plus comme une panne — voir la section dédiée. Les trois
assertions de `env.c` ne se déclenchent pas non plus : `asserts 0`.)*

**3. Écouter l'audio.** Corrigé par la mesure, pas encore par l'oreille. Le
chiffre à surveiller dans `dkr.log` est `clipped N/18000` : quelques dizaines
est normal pour un jeu fort, quelques centaines serait une distorsion qui reste
à expliquer. Si le son est mauvais, l'A/B en une compilation est
`GC_AUDIO_FX=0`.

**4. Le cadencement des images.** Écart de fidélité relevé en lisant
`libultra/src/sc/sched.c` et **volontairement non corrigé** dans le même commit
qu'un plantage : l'original diffère la réponse d'une tâche `OS_SC_LAST_TASK`
jusqu'à ce que deux retraces soient passés (`sc->unkTask`, `frameCount`), ce qui
est le plafond à 30 images/seconde du jeu. Le portage répond immédiatement. À
porter proprement, avec `frameCount` incrémenté au retrace comme dans
l'original.

**5. Le sol en aplat clair.** Visible sur les captures, présent avec **et** sans
`GC_DYNLIT2`. C'est ce qui empêche les ombres de se lire. Les compteurs de
texture sont propres (`tex magenta: unhandled 0, no tlut 0`), donc ce n'est pas
une conversion ratée. **Mesurer avant de corriger** : `cover` nomme déjà la
primitive la plus large avec sa texture et son combineur.

**6. La latence audio, ~112 ms.** Conséquence assumée de `RING_FRAMES 8192`.
La récupérer demande une politique d'acceptation plus fine, **pas** un anneau
plus petit — 2048 et 4096 ont été mesurés et tronquent les tampons.

**7. Le carton « 1 » derrière chaque personnage** de l'écran de sélection. Voir
« Symptômes ouverts » ci-dessous.

**8. Vérifier les fantômes de bout en bout.** Le Controller Pak est émulé et
compile, mais le chemin complet — enregistrer, éteindre, rallumer, relire — n'a
pas été parcouru.

### Trois pièges de méthode, appris le 2026-09-03

**L'instrument lui-même peut mentir.** Le heartbeat bat tous les 60 VSyncs et
j'ai longtemps divisé par « une seconde ». C'en fait **1101** sous Dolphin et
**1200** sur la console PAL de l'utilisateur. Le heartbeat imprime désormais
`clock: N ms since last beat` ; **tout taux tiré de ce log doit être mis à
l'échelle par `1000/elapsedMs`.**

**Ne pas « corriger » la référence par raisonnement.** `ref-sm64gc` est éprouvé
contre du vrai matériel. Deux fois ce jour-là j'ai écrit une conclusion élégante
sans mesure — la ligne `step_diff` de l'envmixer, et l'affirmation que `A_POLEF`
ne filtrait que la prise de sortie. Les deux étaient fausses.

**Un maximum ne dit rien sur une distribution.** `peak` a correctement désigné
la saturation audio, puis a continué de crier après la correction. Le compte
d'échantillons écrêtés à côté du maximum a coûté trois lignes et a tranché en un
run. Quand un compteur sature, ajouter celui qui compte plutôt que celui qui
maximise.

### Et quatre appris le 2026-09-04, tous chers

**Relire l'opcode ennuyeux.** Le défaut audio n'était ni dans l'envmixer, ni
dans le décodeur ADPCM, ni dans le rééchantillonneur — les trois fonctions dont
l'arithmétique méritait vérification, et toutes les trois vérifiées deux fois.
Il était dans `a_interleave`, quatorze lignes qui ne font que recopier des
échantillons, jamais comparées à la référence parce qu'il n'y avait « rien à se
tromper ». La bonne heuristique n'est pas « où l'arithmétique est-elle
difficile » mais **« quelles lignes n'ont jamais été comparées à la
référence »**.

**Le canal de debug fait partie du système testé.** `CON_EnableGecko(EXI_CHANNEL_1,
FALSE)` envoyait du protocole USB Gecko dans le SD Gecko de l'utilisateur, sur
le même canal EXI, depuis le premier jour. Inoffensif sous l'émulateur, dont le
slot B *est* un Gecko. Demander ce que l'instrument **occupe**, pas seulement ce
qu'il imprime.

**Un changement de l'endroit où l'instrument écrit est un changement de
l'instrument.** Le tamponnage des balises pour supprimer une vraie concurrence
carte a fait perdre exactement les balises que le plantage devait laisser. Ça
mérite le même scepticisme qu'une modification du code mesuré.

**Dolphin est indulgent là où le portage diffère le plus.** Il n'émule pas la
MMU pour du homebrew : lire l'adresse 4 ou 0xDEADBEE0 y réussit tranquillement.
Les caches, l'alignement, l'EXI sont dans le même cas. **Les trois défauts
matériels de la journée sont tous dans cette catégorie**, et aucun n'était
reproductible sous l'émulateur. Quand un symptôme n'existe que sur console, ne
pas insister avec l'émulateur : instrumenter la console, et lire l'écran de
plantage — une photographie de la télévision a donné en une fois ce que quatre
runs instrumentés n'avaient pas donné.


### Symptômes ouverts, non encore mesurés

- **Le carton « 1 » s'affiche derrière chaque personnage** de l'écran de
  sélection. `G_SETCIMG` n'est plus un candidat : la mesure a montré que les
  deux seules cibles sont le framebuffer et le Z-buffer, sans cible hors écran.
  Restent le cache de textures rendant la même tuile à huit appelants, ou le
  chemin des sprites. L'instrument existe : `cover` nomme la primitive la plus
  large avec sa texture et son combineur ; restreint aux texrects de cet écran,
  il tranche en une exécution. **Ne pas corriger avant d'avoir mesuré.**
- **Sprites du repli CPU hors champ à gauche** : `cpu box (-398,38)-(-196,63)
  slot2 bb1`. La boîte écran est négative, donc l'ancre du billboard ou la
  projection du repli place le quad à gauche de l'écran.
- **Un gel après plusieurs appuis sur START**, menant assez loin dans les
  menus. Le heartbeat s'arrête complètement, donc c'est le thread de boot
  lui-même qui ne tourne plus. Non reproductible sur quatre appuis. À instruire
  avec `gc_stack_overflowed`, le `blocked at` du thread de jeu et les paires
  entrée/sortie de `gc_gfx_run_dl`.
