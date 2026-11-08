# CS2 music kits

Every kit in `music_definitions` of `items_game.txt` (102, read from `pak01_dir.vpk` of the game build of 2026-11-07). Pass the `--kit` value to `tools/make_music_mod.py`.

- **MVP, s / Menu, s:** how long these slots are in the original kit. Your track is cut to these lengths (the menu gets the whole song if it's longer).
- **Replaceable:** all of them. *yes*: the kit has tracks in the older `.vsnd_c` format (type 4), whose header the script takes as a template. *yes (format 5)*: newer kits (Valve's CS:GO, Half-Life: Alyx and nearly all from id 78 on) keep the sound description in a `CTRL` block (KV3) instead of `DATA`. The script puts type 4 files in their place with a header from another kit's track (`theverkkars_01`): the game reads both types, `isoxo_01` and `knock2_02` mix them. Replacing format 5 kits hasn't been checked in game yet.
- Ids 77 and 97 don't exist in the game. Id 2 has no name in the localization and usually isn't in inventories.

| id | Kit (as in game) | `--kit` | Slots | MVP, s | Menu, s | Replaceable |
|---:|---|---|---:|---:|---:|---|
| 1 | Valve — Counter-Strike 2 | `valve_cs2_01` | 14 | 10.9 | 207.2 | yes |
| 2 | (no name) | `valve_02` | 13 | 12.6 | 51.2 | yes (format 5) |
| 3 | Daniel Sadowski — Crimson Assault | `danielsadowski_01` | 13 | 9.1 | 152.9 | yes |
| 4 | Noisia — Sharpened | `noisia_01` | 16 | 8.4 | 129.8 | yes |
| 5 | Robert Allaire — Insurgency | `robertallaire_01` | 16 | 11.2 | 139.9 | yes |
| 6 | Sean Murray — A*D*8 | `seanmurray_01` | 15 | 14.0 | 159.8 | yes |
| 7 | Feed Me — High Noon | `feedme_01` | 14 | 9.9 | 130.0 | yes |
| 8 | Dren — Death's Head Demolition | `dren_01` | 17 | 11.4 | 215.7 | yes |
| 9 | Austin Wintory — Desert Fire | `austinwintory_01` | 15 | 10.7 | 163.5 | yes |
| 10 | Sasha — LNOE | `sasha_01` | 13 | 9.1 | 140.6 | yes |
| 11 | Skog — Metal | `skog_01` | 15 | 11.8 | 91.8 | yes |
| 12 | Midnight Riders — All I Want for Christmas | `midnightriders_01` | 15 | 6.8 | 211.3 | yes |
| 13 | Matt Lange — IsoRhythm | `mattlange_01` | 15 | 7.3 | 199.8 | yes |
| 14 | Mateo Messina — For No Mankind | `mateomessina_01` | 22 | 7.4 | 158.1 | yes |
| 15 | Various Artists — Hotline Miami | `hotlinemiami_01` | 18 | 7.2 | 289.8 | yes |
| 16 | Daniel Sadowski — Total Domination | `danielsadowski_02` | 13 | 6.2 | 151.4 | yes |
| 17 | Damjan Mravunac — The Talos Principle | `damjanmravunac_01` | 15 | 7.9 | 214.9 | yes |
| 18 | Proxy — Battlepack | `proxy_01` | 15 | 9.8 | 141.2 | yes |
| 19 | Ki:Theory — MOLOTOV | `kitheory_01` | 13 | 8.0 | 157.1 | yes |
| 20 | Troels Folmann — Uber Blasto Phone | `troelsfolmann_01` | 15 | 9.9 | 245.1 | yes |
| 21 | Kelly Bailey — Hazardous Environments | `kellybailey_01` | 15 | 26.3 | 167.0 | yes |
| 22 | Skog — II-Headshot | `skog_02` | 13 | 7.7 | 238.5 | yes |
| 23 | Daniel Sadowski — The 8-Bit Kit | `danielsadowski_03` | 13 | 8.1 | 138.2 | yes |
| 24 | AWOLNATION — I Am | `awolnation_01` | 17 | 8.9 | 148.1 | yes |
| 25 | Mord Fustang — Diamonds | `mordfustang_01` | 15 | 11.4 | 194.5 | yes |
| 26 | Michael Bross — Invasion! | `michaelbross_01` | 13 | 10.4 | 191.8 | yes |
| 27 | Ian Hultquist — Lion's Mouth | `ianhultquist_01` | 17 | 11.1 | 169.7 | yes |
| 28 | New Beat Fund —Sponge Fingerz | `newbeatfund_01` | 15 | 13.4 | 206.2 | yes |
| 29 | Beartooth — Disgusting | `beartooth_01` | 15 | 8.0 | 198.0 | yes |
| 30 | Lennie Moore — Java Havana Funkaloo | `lenniemoore_01` | 17 | 8.5 | 242.0 | yes |
| 31 | Darude — Moments CSGO | `darude_01` | 17 | 7.6 | 170.7 | yes |
| 32 | Beartooth — Aggressive | `beartooth_02` | 15 | 9.3 | 166.5 | yes |
| 33 | Blitz Kids — The Good Youth | `blitzkids_01` | 15 | 8.1 | 196.1 | yes |
| 34 | Hundredth — FREE | `hundredth_01` | 17 | 5.3 | 146.9 | yes |
| 35 | Neck Deep — Life's Not Out To Get You | `neckdeep_01` | 17 | 10.9 | 165.1 | yes |
| 36 | Roam — Backbone | `roam_01` | 17 | 7.8 | 184.6 | yes |
| 37 | Twin Atlantic — GLA | `twinatlantic_01` | 17 | 8.3 | 224.1 | yes |
| 38 | Skog — III-Arena | `skog_03` | 13 | 7.6 | 293.9 | yes |
| 39 | The Verkkars — EZ4ENCE | `theverkkars_01` | 15 | 6.3 | 154.1 | yes |
| 40 | Halo — The Master Chief Collection | `halo_01` | 15 | 15.2 | 142.1 | yes |
| 41 | Scarlxrd — King, Scar | `scarlxrd_01` | 13 | 9.6 | 220.7 | yes |
| 42 | Half-Life: Alyx — Anti-Citizen | `hlalyx_01` | 18 | 16.7 | 187.8 | yes (format 5) |
| 43 | Austin Wintory — Bachram | `austinwintory_02` | 15 | 12.5 | 181.3 | yes |
| 44 | Dren — Gunman Taco Truck | `dren_02` | 18 | 11.8 | 178.1 | yes |
| 45 | Daniel Sadowski — Eye of the Dragon | `danielsadowski_04` | 13 | 7.0 | 161.9 | yes |
| 46 | Tree Adams and Ben Bromfield — M.U.D.D. FORCE | `treeadams_benbromfield_01` | 15 | 9.6 | 195.7 | yes |
| 47 | Tim Huling — Neo Noir | `timhuling_01` | 17 | 9.0 | 145.1 | yes |
| 48 | Sam Marshall — Bodacious | `sammarshall_01` | 15 | 7.9 | 198.1 | yes |
| 49 | Matt Levine — Drifter | `mattlevine_01` | 17 | 10.4 | 149.0 | yes |
| 50 | Amon Tobin — All for Dust | `amontobin_01` | 17 | 10.2 | 154.0 | yes |
| 51 | Darren Korb — Hades | `hades_01` | 17 | 8.2 | 143.3 | yes |
| 52 | Neck Deep — The Lowlife Pack | `neckdeep_02` | 17 | 12.3 | 186.7 | yes |
| 53 | Scarlxrd — CHAIN$AW.LXADXUT. | `scarlxrd_02` | 15 | 6.9 | 212.2 | yes |
| 54 | Austin Wintory — Mocha Petal | `austinwintory_03` | 16 | 9.9 | 207.2 | yes |
| 55 | Chipzel — ~Yellow Magic~ | `chipzel_01` | 13 | 9.0 | 156.9 | yes |
| 56 | Freaky DNA — Vici | `freakydna_01` | 17 | 8.1 | 260.1 | yes |
| 57 | Jesse Harlin — Astro Bellum | `jesseharlin_01` | 14 | 9.6 | 144.1 | yes |
| 58 | Laura Shigihara — Work Hard, Play Hard | `laurashigihara_01` | 17 | 7.7 | 143.0 | yes |
| 59 | Sarah Schachner — KOLIBRI | `sarahschachner_01` | 16 | 10.7 | 178.1 | yes |
| 60 | bbno$ — u mad! | `bbnos_01` | 14 | 7.4 | 151.5 | yes |
| 61 | The Verkkars & n0thing — Flashbang Dance | `theverkkars_02` | 15 | 9.4 | 157.9 | yes |
| 62 | 3kliksphilip — Heading for the Source | `3kliksphilip_01` | 17 | 10.6 | 319.4 | yes |
| 63 | Humanity's Last Breath — Void | `hlb_01` | 18 | 10.9 | 135.2 | yes |
| 64 | Juelz — Shooters | `juelz_01` | 15 | 9.0 | 260.0 | yes |
| 65 | Knock2 — dashstar* | `knock2_01` | 14 | 10.4 | 200.2 | yes |
| 66 | Meechy Darko — Gothic Luxury | `meechydarko_01` | 17 | 8.6 | 240.2 | yes |
| 67 | Sullivan King — Lock Me Up | `sullivanking_01` | 15 | 6.7 | 160.3 | yes |
| 68 | Perfect World, 中脸 Hua Lian — Painted Face | `perfectworld_01` | 13 | 8.0 | 206.7 | yes |
| 69 | Denzel Curry — ULTIMATE | `denzelcurry_01` | 17 | 9.1 | 192.1 | yes |
| 70 | Valve — CS:GO | `valve_01` | 15 | 33.5 | 128.8 | yes (format 5) |
| 71 | DRYDEN — Feel The Power | `dryden_01` | 15 | 16.0 | 192.5 | yes |
| 72 | ISOxo — inhuman | `isoxo_01` | 15 | 12.0 | 198.1 | yes |
| 73 | KILL SCRIPT — All Night | `killscript_01` | 15 | 9.0 | 201.0 | yes |
| 74 | Knock2 — Make U SWEAT! | `knock2_02` | 15 | 10.3 | 234.3 | yes |
| 75 | Rad Cat — Reason | `radcat_01` | 15 | 10.5 | 165.7 | yes |
| 76 | TWERL and Ekko & Sidetrack — Under Bright Lights | `twerl_01` | 15 | 11.1 | 228.2 | yes |
| 78 | Austin Wintory — The Devil Went Clubbing in Georgia | `austinwintory_04` | 15 | 13.9 | 174.2 | yes (format 5) |
| 79 | Ben Bromfield — Rabbit Hole | `benbromfield_01` | 15 | 9.6 | 147.7 | yes (format 5) |
| 80 | Daniel Sadowski — Dead Shot | `danielsadowski_05` | 13 | 8.5 | 128.1 | yes (format 5) |
| 81 | Dren McDonald — Coffee! Kofe! Kahveh! | `dren_03` | 17 | 11.1 | 169.0 | yes (format 5) |
| 82 | Matt Levine — Agency | `mattlevine_02` | 17 | 10.7 | 241.0 | yes (format 5) |
| 83 | Sam Marshall — Clutch | `sammarshall_02` | 17 | 11.8 | 169.6 | yes (format 5) |
| 84 | Tim Huling — Devil's Paintbrush | `timhuling_02` | 17 | 11.2 | 129.1 | yes (format 5) |
| 85 | Tree Adams — Seventh Moon | `treeadams_01` | 14 | 9.0 | 256.6 | yes (format 5) |
| 86 | Perfect World — Ay Hey | `perfectworld_02` | 14 | 9.2 | 205.8 | yes (format 5) |
| 87 | Adam Beyer — Red Room | `adambeyer_01` | 16 | 8.3 | 393.0 | yes (format 5) |
| 88 | Ghost — Skeletá | `ghost_01` | 17 | 9.1 | 256.1 | yes (format 5) |
| 89 | HEALTH — RAT WARS | `health_01` | 15 | 8.8 | 146.1 | yes (format 5) |
| 90 | James and the Cold Gun — Chewing Glass | `jamesandthecoldgun_01` | 15 | 8.1 | 221.4 | yes (format 5) |
| 91 | Jonathan Young — Starship Velociraptor | `jonathanyoung_01` | 13 | 8.8 | 228.1 | yes (format 5) |
| 92 | Juelz — Floorspace | `juelz_02` | 17 | 8.7 | 212.3 | yes (format 5) |
| 93 | Killer Mike — MICHAEL | `killermike_01` | 15 | 9.1 | 114.1 | yes (format 5) |
| 94 | PVRIS — Evergreen | `pvris_01` | 17 | 6.6 | 152.2 | yes (format 5) |
| 95 | Selective Response — No Love Only Pleasure | `selectiveresponse_01` | 15 | 10.1 | 361.7 | yes (format 5) |
| 96 | Tigercub — The Perfume of Decay | `tigercub_01` | 13 | 6.2 | 220.5 | yes (format 5) |
| 98 | ALRT — DOPAMINE HIT | `alrt_01` | 18 | 8.0 | 194.7 | yes (format 5) |
| 99 | Altare — Change My Mind | `altare_01` | 16 | 10.3 | 195.5 | yes (format 5) |
| 100 | borne — Give It To Me | `borne_01` | 18 | 13.0 | 137.6 | yes (format 5) |
| 101 | Pirapus — EVERYNITE | `pirapus_01` | 16 | 8.3 | 200.6 | yes (format 5) |
| 102 | Repiet & Julia Kleijn — On And On | `repiet_01` | 17 | 10.3 | 243.8 | yes (format 5) |
| 103 | ShockOne — Voices | `shockone_01` | 16 | 13.8 | 206.1 | yes (format 5) |
| 104 | Starjunk 95 — Industrial Sunset Memories | `starjunk95_01` | 18 | 8.7 | 162.5 | yes (format 5) |
