# Музыкальные наборы CS2

Все наборы из `music_definitions` в `items_game.txt` (102 штуки, прочитано из `pak01_dir.vpk` 2026-11-07, сборка игры на эту дату). Название — из русской локализации игры. Значение из колонки `--kit` передаётся в `tools\make_music_mod.py`.

- **MVP, с / Меню, с:** длина этих отрезков у оригинала. Твой трек режется под эти длины (меню получает песню целиком, если она длиннее).
- **Можно заменить:** все. «Да» — у набора есть треки старого формата `.vsnd_c` (тип 4), их заголовок скрипт берёт за образец. «Да (формат 5)» — новые наборы (CS:GO от Valve, Half-Life: Alyx и почти все с id 78+). У них описание звука лежит в блоке `CTRL` (KV3), а не в `DATA`. Скрипт кладёт на их место файлы типа 4 с заголовком от трека другого набора (`theverkkars_01`): игра читает оба типа, в `isoxo_01` и `knock2_02` они перемешаны. Замена формата 5 в игре ещё не проверялась.
- id 77 и 97 в игре нет. У id 2 нет названия в локализации, в инвентаре его обычно не бывает.

| id | Набор (как в игре) | `--kit` | Отрезков | MVP, с | Меню, с | Можно заменить |
|---:|---|---|---:|---:|---:|---|
| 1 | Valve — Counter-Strike 2 | `valve_cs2_01` | 14 | 10.9 | 207.2 | да |
| 2 | (без названия) | `valve_02` | 13 | 12.6 | 51.2 | да (формат 5) |
| 3 | Daniel Sadowski — Crimson Assault | `danielsadowski_01` | 13 | 9.1 | 152.9 | да |
| 4 | Noisia — Sharpened | `noisia_01` | 16 | 8.4 | 129.8 | да |
| 5 | Robert Allaire — Insurgency | `robertallaire_01` | 16 | 11.2 | 139.9 | да |
| 6 | Sean Murray — A*D*8 | `seanmurray_01` | 15 | 14.0 | 159.8 | да |
| 7 | Feed Me — High Noon | `feedme_01` | 14 | 9.9 | 130.0 | да |
| 8 | Dren — Death's Head Demolition | `dren_01` | 17 | 11.4 | 215.7 | да |
| 9 | Austin Wintory — Desert Fire | `austinwintory_01` | 15 | 10.7 | 163.5 | да |
| 10 | Sasha — LNOE | `sasha_01` | 13 | 9.1 | 140.6 | да |
| 11 | Skog — Metal | `skog_01` | 15 | 11.8 | 91.8 | да |
| 12 | Midnight Riders — All I Want for Christmas | `midnightriders_01` | 15 | 6.8 | 211.3 | да |
| 13 | Matt Lange — IsoRhythm | `mattlange_01` | 15 | 7.3 | 199.8 | да |
| 14 | Mateo Messina — For No Mankind | `mateomessina_01` | 22 | 7.4 | 158.1 | да |
| 15 | Разные исполнители — Hotline Miami | `hotlinemiami_01` | 18 | 7.2 | 289.8 | да |
| 16 | Daniel Sadowski — Total Domination | `danielsadowski_02` | 13 | 6.2 | 151.4 | да |
| 17 | Damjan Mravunac — The Talos Principle | `damjanmravunac_01` | 15 | 7.9 | 214.9 | да |
| 18 | Proxy — Battlepack | `proxy_01` | 15 | 9.8 | 141.2 | да |
| 19 | Ki:Theory — MOLOTOV | `kitheory_01` | 13 | 8.0 | 157.1 | да |
| 20 | Troels Folmann — Uber Blasto Phone | `troelsfolmann_01` | 15 | 9.9 | 245.1 | да |
| 21 | Kelly Bailey — Hazardous Environments | `kellybailey_01` | 15 | 26.3 | 167.0 | да |
| 22 | Skog — II-Headshot | `skog_02` | 13 | 7.7 | 238.5 | да |
| 23 | Daniel Sadowski — The 8-Bit Kit | `danielsadowski_03` | 13 | 8.1 | 138.2 | да |
| 24 | AWOLNATION — I Am | `awolnation_01` | 17 | 8.9 | 148.1 | да |
| 25 | Mord Fustang — Diamonds | `mordfustang_01` | 15 | 11.4 | 194.5 | да |
| 26 | Michael Bross — Invasion! | `michaelbross_01` | 13 | 10.4 | 191.8 | да |
| 27 | Ian Hultquist — Lion's Mouth | `ianhultquist_01` | 17 | 11.1 | 169.7 | да |
| 28 | New Beat Fund —Sponge Fingerz | `newbeatfund_01` | 15 | 13.4 | 206.2 | да |
| 29 | Beartooth — Disgusting | `beartooth_01` | 15 | 8.0 | 198.0 | да |
| 30 | Lennie Moore — Java Havana Funkaloo | `lenniemoore_01` | 17 | 8.5 | 242.0 | да |
| 31 | Darude — Moments CSGO | `darude_01` | 17 | 7.6 | 170.7 | да |
| 32 | Beartooth — Aggressive | `beartooth_02` | 15 | 9.3 | 166.5 | да |
| 33 | Blitz Kids — The Good Youth | `blitzkids_01` | 15 | 8.1 | 196.1 | да |
| 34 | Hundredth — FREE | `hundredth_01` | 17 | 5.3 | 146.9 | да |
| 35 | Neck Deep — Life's Not Out To Get You | `neckdeep_01` | 17 | 10.9 | 165.1 | да |
| 36 | Roam — Backbone | `roam_01` | 17 | 7.8 | 184.6 | да |
| 37 | Twin Atlantic — GLA | `twinatlantic_01` | 17 | 8.3 | 224.1 | да |
| 38 | Skog — III-Arena | `skog_03` | 13 | 7.6 | 293.9 | да |
| 39 | The Verkkars — EZ4ENCE | `theverkkars_01` | 15 | 6.3 | 154.1 | да |
| 40 | Halo — The Master Chief Collection | `halo_01` | 15 | 15.2 | 142.1 | да |
| 41 | Scarlxrd — King, Scar | `scarlxrd_01` | 13 | 9.6 | 220.7 | да |
| 42 | Half-Life: Alyx — Anti-Citizen | `hlalyx_01` | 18 | 16.7 | 187.8 | да (формат 5) |
| 43 | Austin Wintory — Bachram | `austinwintory_02` | 15 | 12.5 | 181.3 | да |
| 44 | Dren — Gunman Taco Truck | `dren_02` | 18 | 11.8 | 178.1 | да |
| 45 | Daniel Sadowski — Eye of the Dragon | `danielsadowski_04` | 13 | 7.0 | 161.9 | да |
| 46 | Tree Adams and Ben Bromfield — M.U.D.D. FORCE | `treeadams_benbromfield_01` | 15 | 9.6 | 195.7 | да |
| 47 | Tim Huling — Neo Noir | `timhuling_01` | 17 | 9.0 | 145.1 | да |
| 48 | Sam Marshall — Bodacious | `sammarshall_01` | 15 | 7.9 | 198.1 | да |
| 49 | Matt Levine — Drifter | `mattlevine_01` | 17 | 10.4 | 149.0 | да |
| 50 | Amon Tobin — All for Dust | `amontobin_01` | 17 | 10.2 | 154.0 | да |
| 51 | Darren Korb — Hades | `hades_01` | 17 | 8.2 | 143.3 | да |
| 52 | Neck Deep — The Lowlife Pack | `neckdeep_02` | 17 | 12.3 | 186.7 | да |
| 53 | Scarlxrd — CHAIN$AW.LXADXUT. | `scarlxrd_02` | 15 | 6.9 | 212.2 | да |
| 54 | Austin Wintory — Mocha Petal | `austinwintory_03` | 16 | 9.9 | 207.2 | да |
| 55 | Chipzel — ~Yellow Magic~ | `chipzel_01` | 13 | 9.0 | 156.9 | да |
| 56 | Freaky DNA — Vici | `freakydna_01` | 17 | 8.1 | 260.1 | да |
| 57 | Jesse Harlin — Astro Bellum | `jesseharlin_01` | 14 | 9.6 | 144.1 | да |
| 58 | Laura Shigihara — Work Hard, Play Hard | `laurashigihara_01` | 17 | 7.7 | 143.0 | да |
| 59 | Sarah Schachner — KOLIBRI | `sarahschachner_01` | 16 | 10.7 | 178.1 | да |
| 60 | bbno$ — u mad! | `bbnos_01` | 14 | 7.4 | 151.5 | да |
| 61 | The Verkkars & n0thing — Flashbang Dance | `theverkkars_02` | 15 | 9.4 | 157.9 | да |
| 62 | 3kliksphilip — Heading for the Source | `3kliksphilip_01` | 17 | 10.6 | 319.4 | да |
| 63 | Humanity's Last Breath — Void | `hlb_01` | 18 | 10.9 | 135.2 | да |
| 64 | Juelz — Shooters | `juelz_01` | 15 | 9.0 | 260.0 | да |
| 65 | Knock2 — dashstar* | `knock2_01` | 14 | 10.4 | 200.2 | да |
| 66 | Meechy Darko — Gothic Luxury | `meechydarko_01` | 17 | 8.6 | 240.2 | да |
| 67 | Sullivan King — Lock Me Up | `sullivanking_01` | 15 | 6.7 | 160.3 | да |
| 68 | Perfect World, 中脸 Hua Lian — Painted Face | `perfectworld_01` | 13 | 8.0 | 206.7 | да |
| 69 | Denzel Curry — ULTIMATE | `denzelcurry_01` | 17 | 9.1 | 192.1 | да |
| 70 | Valve — CS:GO | `valve_01` | 15 | 33.5 | 128.8 | да (формат 5) |
| 71 | DRYDEN — Feel The Power | `dryden_01` | 15 | 16.0 | 192.5 | да |
| 72 | ISOxo — inhuman | `isoxo_01` | 15 | 12.0 | 198.1 | да |
| 73 | KILL SCRIPT — All Night | `killscript_01` | 15 | 9.0 | 201.0 | да |
| 74 | Knock2 — Make U SWEAT! | `knock2_02` | 15 | 10.3 | 234.3 | да |
| 75 | Rad Cat — Reason | `radcat_01` | 15 | 10.5 | 165.7 | да |
| 76 | TWERL и Ekko & Sidetrack — Under Bright Lights | `twerl_01` | 15 | 11.1 | 228.2 | да |
| 78 | Austin Wintory — The Devil Went Clubbing in Georgia | `austinwintory_04` | 15 | 13.9 | 174.2 | да (формат 5) |
| 79 | Ben Bromfield — Rabbit Hole | `benbromfield_01` | 15 | 9.6 | 147.7 | да (формат 5) |
| 80 | Daniel Sadowski — Dead Shot | `danielsadowski_05` | 13 | 8.5 | 128.1 | да (формат 5) |
| 81 | Dren McDonald — Coffee! Kofe! Kahveh! | `dren_03` | 17 | 11.1 | 169.0 | да (формат 5) |
| 82 | Matt Levine — Agency | `mattlevine_02` | 17 | 10.7 | 241.0 | да (формат 5) |
| 83 | Sam Marshall — Clutch | `sammarshall_02` | 17 | 11.8 | 169.6 | да (формат 5) |
| 84 | Tim Huling — Devil's Paintbrush | `timhuling_02` | 17 | 11.2 | 129.1 | да (формат 5) |
| 85 | Tree Adams — Seventh Moon | `treeadams_01` | 14 | 9.0 | 256.6 | да (формат 5) |
| 86 | Perfect World — Ay Hey | `perfectworld_02` | 14 | 9.2 | 205.8 | да (формат 5) |
| 87 | Adam Beyer — Red Room | `adambeyer_01` | 16 | 8.3 | 393.0 | да (формат 5) |
| 88 | Ghost — Skeletá | `ghost_01` | 17 | 9.1 | 256.1 | да (формат 5) |
| 89 | HEALTH — RAT WARS | `health_01` | 15 | 8.8 | 146.1 | да (формат 5) |
| 90 | James and the Cold Gun — Chewing Glass | `jamesandthecoldgun_01` | 15 | 8.1 | 221.4 | да (формат 5) |
| 91 | Jonathan Young — Starship Velociraptor | `jonathanyoung_01` | 13 | 8.8 | 228.1 | да (формат 5) |
| 92 | Juelz — Floorspace | `juelz_02` | 17 | 8.7 | 212.3 | да (формат 5) |
| 93 | Killer Mike — MICHAEL | `killermike_01` | 15 | 9.1 | 114.1 | да (формат 5) |
| 94 | PVRIS — Evergreen | `pvris_01` | 17 | 6.6 | 152.2 | да (формат 5) |
| 95 | Selective Response — No Love Only Pleasure | `selectiveresponse_01` | 15 | 10.1 | 361.7 | да (формат 5) |
| 96 | Tigercub — The Perfume of Decay | `tigercub_01` | 13 | 6.2 | 220.5 | да (формат 5) |
| 98 | ALRT — DOPAMINE HIT | `alrt_01` | 18 | 8.0 | 194.7 | да (формат 5) |
| 99 | Altare — Change My Mind | `altare_01` | 16 | 10.3 | 195.5 | да (формат 5) |
| 100 | borne — Give It To Me | `borne_01` | 18 | 13.0 | 137.6 | да (формат 5) |
| 101 | Pirapus — EVERYNITE | `pirapus_01` | 16 | 8.3 | 200.6 | да (формат 5) |
| 102 | Repiet & Julia Kleijn — On And On | `repiet_01` | 17 | 10.3 | 243.8 | да (формат 5) |
| 103 | ShockOne — Voices | `shockone_01` | 16 | 13.8 | 206.1 | да (формат 5) |
| 104 | Starjunk 95 — Industrial Sunset Memories | `starjunk95_01` | 18 | 8.7 | 162.5 | да (формат 5) |
