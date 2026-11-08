# cs2_gc

> [!WARNING]
> cs2_gc always runs the game with `-insecure`: VAC is off and official servers won't let you in. It is not affiliated with or endorsed by Valve. Use it at your own risk, ideally on a secondary Steam account.

In Valve games, the Game Coordinator (GC) is the backend service behind inventories, loadouts, cases, stickers and ranks. cs2_gc is a port of [csgo_gc](https://github.com/mikkokko/csgo_gc) to Counter-Strike 2. It redirects the game's GC traffic to an implementation that runs inside the game process, so everything works offline and locally. Your inventory lives in `cs2_gc\inventory.txt`.

## Features

- **Inventory:** equipping (CS2 equip slots), case opening including the newest cases, 7-day case rentals, stickers (with rotation, scale and offset), charms (applying and removing), name tags, storage units, music kits, StatTrak and StatTrak swaps, souvenirs and the in-game store
- **Trade up contracts**, including five coverts for a knife or gloves
- **Armory:** pass activation, stars and rewards
- **Progression:** level and XP kept in the inventory file, XP for matches with bots and on community servers, the weekly care package and the Service Medal at level 40
- **Terminals** (Genesis, Dead Hand), Sticker Slabs, tournament items (viewer passes, souvenir tokens) and inspect links for your own items
- **Ranks:** Competitive, Wingman and Premier CS Rating, set in the config
- **Web inventory editor** served by the GC: add anything from a catalogue of the whole game, edit items you have (rarity, quality, wear, pattern, StatTrak counter, name tag), changes show up in game right away
- **CSForge mod loader:** custom skins, music kits, sounds, intro video and texts, or any game file replaced, without touching the game folder
- **Upgrader:** an inventory tab where skins go in for a chance at a pricier one, with chances from real market prices
- **Dedicated server** on Windows, no Steam client needed (experimental)

> [!NOTE]
> Most of this is checked in game. A few newer parts (terminals, StatTrak swaps, Sticker Slabs, tournament items, XP for matches, sound and intro mods, the upgrader's market prices) have so far only been checked by the offline self-test. Weekly missions and matchmaking are not implemented. Windows only.

## Safety

- The game always starts with `-insecure`. The launcher restarts itself with it if it's missing and refuses `-trusted`. A local server can't become secure either (`BSecureAllowed` returns false, the secure flag is stripped from `InitGameServer`).
- Game files are never modified. cs2_gc only reads the game's DLLs and `pak01_dir.vpk`. Everything it writes (config, inventory, log, mods) stays in its own `cs2_gc\` data folder.
- CS2 only starts when the launcher sits next to `cs2.exe`, so `run_cs2_gc.cmd` copies `cs2_gc.exe` into `game\bin\win64\` only while you play and removes it, along with any crash dumps, when the game exits. `cleanup_cs2_folder.cmd` does the same cleanup on demand and checks that nothing named `cs2_gc*` is left.
- Launching CS2 from Steam as usual gives you the normal game with VAC. cs2_gc doesn't hook into it.

## Installation

1. Requirements: Windows 10 or 11 (x64), Steam running, CS2 installed.
2. Download the latest zip from the [releases page](../../releases/latest) and extract it anywhere outside the game folder.
3. If CS2 isn't in `C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive`, set `CS2_ROOT` at the top of `run_cs2_gc.cmd` and `cleanup_cs2_folder.cmd`.
4. Run `run_cs2_gc.cmd` and keep its window open while you play. Launch options are passed on to the game, e.g. `run_cs2_gc.cmd -novid -console`.
5. If the window was closed early or the game crashed, run `cleanup_cs2_folder.cmd`.

The log is in `release\cs2_gc\gc_log.txt` and in the game console (`[GC]` lines).

## Inventory editing

While the game is running, open http://127.0.0.1:13001/ (or `open_editor.cmd`) and press *Connect*.

- **Catalogue:** every item of the current game (weapons and their finishes, knives, gloves, stickers, charms, music kits, graffiti, patches), searchable by name, internal name or id. *Add to inventory* puts it straight into the game. `tools/gen_invedit_catalog.py --lang <language>` makes it with the names of another game language.
- **Edit item:** for an item you have, change its rarity, quality (including the ★ of knives), wear and pattern seed, the StatTrak counter and the name tag.
- **Mods:** turn CSForge mods on and off and change their load order. Takes effect at the next game start.

Changes are saved to `inventory.txt`. `"inventory_editor_port"` sets the port. To connect another csgo_gc editor, set `"inventory_editor_origin"` to the `Origin` it provides.

`inventory.txt` has the same format as in csgo_gc, so its offline editors work too. Edit it only while the game is closed, or the GC will overwrite it.

## Configuration

See [examples/config.txt](examples/config.txt) (it becomes `release\cs2_gc\config.txt`). The main options:

| Option | What it does |
|---|---|
| `ranks` | Competitive, Wingman and Premier rank and wins |
| `player_level`, `player_cur_xp` | level and XP of a new inventory; after that they're kept in `inventory.txt` |
| `mods` | `1` turns on CSForge and the built-in interface fixes (the upgrader among them) |
| `panorama_fixes` | the built-in interface fixes, on by default when mods are on |
| `match_xp` | XP for matches with bots and on community servers |
| `upgrader_prices`, `upgrader_prices_hours` | the upgrader's price list currency (`RUB`, `USD`, `EUR`, or `off`) and how often it's refreshed |
| `inventory_editor_port` | the editor's port, 13001 by default |
| `log_output` | 0 nothing, 1 the game console, 2 the console and `gc_log.txt` |

## Mods (CSForge)

CSForge builds every enabled mod into one VPK at game start (`cs2_gc\mods\_build\csforge_dir.vpk`) and mounts it in front of the game's own files from inside the process. The game folder isn't touched. Turn it on with `"mods" "1"`. If the game crashes with mods, set it back to `0`.

A mod is a folder in `cs2_gc\mods\`, every file in it is optional:

```
cs2_gc\mods\<mod>\
  mod.txt                  "mod" { "name" "..." "author" "..." "version" "..." "enabled" "1" "priority" "0" }
  items_game.txt           "items_game" { "paint_kits" { ... } "music_definitions" { ... } ... }
  localization.txt         "lang" { "Tokens" { "PaintKit_my_skin_Tag" "My skin" } }
  localization_<lang>.txt  the same for one language
  files\...                compiled game files (.vtex_c, .vsnd_c, .vmdl_c...) at their game paths
```

- `items_game.txt` sections are appended to the game's schema. Ids the game or an earlier mod already has are skipped (duplicates crash the client), existing entries can't be changed.
- `localization.txt` tokens are added to every language in `"mod_languages"` (English and Russian by default) and replace existing ones, so anything in the game can be renamed.
- For the same path in `files\`, the mod with the higher `priority` wins. Mods load by priority, then by folder name.
- `CSForge:` lines in `gc_log.txt` list the mods, conflicts, skipped ids and the built VPK.

Compile your assets with the free CS2 Workshop Tools (`resourcecompiler`). Use ids above 10100 for paint kits and above 110 for music kits.

### Mod tools

All of them work without the Workshop Tools. They only read the game folder and write mods into the data folder (`--data release\cs2_gc`).

| Tool | What it makes |
|---|---|
| `tools/make_demo_mod.py` | a demo mod: a custom AK-47 finish and the old CS:GO theme in the menu |
| `tools/make_music_mod.py` | a music kit from one mp3: replaces the tracks of an existing kit and renames it. `--cutter` opens a page to pick each slot's start on the waveform. [music_kits.md](music_kits.md) lists the kits |
| `tools/make_sound_mod.py` | replaces any game sound (shots, footsteps, the bomb, UI) with your own file. `--list ak47` shows the game's sounds |
| `tools/make_intro_mod.py` | replaces the COUNTER-STRIKE 2 intro video |
| `tools/make_bot_mod.py` | sets the bot difficulty per game mode (the game has no menu for it) |

The tools convert other audio formats to MP3 with ffmpeg, or with the codecs built into Windows when ffmpeg isn't installed.

```bat
python tools\make_music_mod.py --cs2 "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive" --data release\cs2_gc --mp3 track.mp3 --name "My kit" --kit theverkkars_01 --add-item
python tools\make_sound_mod.py --cs2 "..." --data release\cs2_gc --audio shot.mp3 --sound "sounds/weapons/ak47/ak47_0*" --length 0.6
python tools\make_bot_mod.py --cs2 "..." --data release\cs2_gc --difficulty expert
```

### Built-in interface fixes

With mods on, CSForge also patches the game's own Panorama scripts at every start (`"panorama_fixes" "1"`). The current scripts are read from the game, so a game update can't bring back an old copy, and a fix whose anchor is gone is skipped with a line in the log.

- A case without a key used to open a window with no buttons at all during a match, not even Close. Now it opens for viewing only, with Close.
- The upgrader, see below.

## Upgrader

An *Upgrader* tab in the inventory, and *Upgrade* in a skin's right-click menu. Put up to 10 skins in (knives and gloves too) and pick a target. If the wheel stops in the chance arc you get the target, otherwise the skins are gone.

- **Chance:** what the bet is worth divided by the target's price. A skin worth 50 for one worth 100 is 50%. The target has to cost more than the bet, so cheaper targets aren't offered.
- **Targets:** a skin in a given wear (FN, MW, FT, WW, BS), cheapest first, each with its price and your chance. Filters: rarity, type, same weapon, StatTrak™, and x1.5-x10 multipliers that only show targets at least that many times the bet. The skin you win gets a float within the wear you picked.
- **Prices:** [market.csgo.com](https://market.csgo.com)'s public price list (lowest offers, no key needed). cs2_gc downloads it at game start when the saved one (`cs2_gc\prices_RUB.json`) is older than 12 hours and keeps using the old one when there's no internet. A price depends on the wear, StatTrak™ and souvenir status, like on the market. A skin that isn't on sale in its wear can't go in. Doppler phases and gems share one market name and so one price.
- **No price list:** with `"upgrader_prices" "off"`, or without internet on the first start, the chances go by rarity, 1:10 per rarity step.

The roll happens in the GC, with the same prices the window shows. The won skin stays hidden until the wheel stops.

## Building

Requires Visual Studio 2022 with the C++ workload, which includes CMake and vcpkg.

```bat
build.cmd
```

The first build takes 20-30 minutes while vcpkg builds protobuf, mbedtls and libwebsockets. The result is in `release\`, and rebuilding never overwrites your `config.txt` and `inventory.txt`. `tools/make_release_zip.py` packs a release zip.

### Offline self-test

`cs2_gc_selftest.exe` runs without the game or Steam and checks reading the VPK and the item schema, opening every case, trade ups, rentals, the store, the editor protocol, CSForge builds, the upgrader and more. It only reads the game folder and writes only to the data folder you give it. Put `items_extra.txt` and `price_sheet.txt` from `examples\` into that folder first.

```bat
build\tests\cs2_gc_selftest.exe "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive" "%TEMP%\cs2_gc_test"
```

`--serve <seconds>` runs only the editor's server on top of the data folder's `inventory.txt`, to try the page without the game. `--prices <currency>` downloads the upgrader's price list.

### After a game update

- `tools/dump_protos.py` and `tools/gen_message_names.py` refresh the GC message definitions from `client.dll`.
- `tools/gen_unusual_lists.py` prints the knife and glove lists of new cases for `examples/items_extra.txt` (the public `items_game.txt` doesn't have them).
- `tools/gen_invedit_catalog.py` rebuilds the editor's catalogue.

## How it works

```
cs2_gc.exe (the launcher)
 ├─ restarts itself with -insecure if needed
 ├─ finds CS2 (libraryfolders.vdf / -cs2path / its own folder)
 ├─ loads game\bin\win64\tier0.dll, then engine2 through tier0!Plat_LoadModule
 ├─ installs the GC:
 │   ├─ finds steamclient64.dll through the registry (HKCU\Software\Valve\Steam\ActiveProcess)
 │   ├─ hooks steamclient64!CreateInterface with an ISteamClient023 proxy
 │   │     ├─ SteamGameCoordinator001 → our GC (ClientGC / ServerGC)
 │   │     └─ SteamUser023 / SteamGameServer015 / SteamUtils010 / SteamMatchMakingServers002 / SteamFriends017
 │   └─ hooks SteamAPI_RegisterCallback / RunCallbacks (for GCMessageAvailable_t)
 └─ Source2Main(hInst, 0, cmdline, nShow, "<game\bin\win64>", "csgo"), the way cs2.exe does it
```

The CS2 GC protobufs are dumped from the descriptors in `client.dll`, the Steam interface layouts from the flat API in `steam_api64.dll`, and `items_game.txt` is read from `pak01_dir.vpk` at runtime.

## Project layout

```
cs2_gc/        the GC: client and server logic, inventory, item schema, Steam hooks, VPK, CSForge
launcher/      cs2_gc.exe
protobufs/     CS2 GC protobufs (dumped from client.dll); csgo_original/ has csgo_gc's for comparison
steamworks/    Steamworks headers and the generated interface proxies
tests/         the offline self-test
tools/         protobuf and interface dumpers, the editor catalogue generator, mod tools, packaging
editor/        the web inventory editor (a fork of csgo_invedit) with the CS2 catalogue
examples/      config.txt, inventory.txt, items_extra.txt, price_sheet.txt, a demo mod
reference/     the original csgo_gc README
```

## Bug reports

Open an issue with what you did, what happened and `gc_log.txt`. If it doesn't look GC related, check first that it doesn't happen in the normal game launched from Steam.

## License

[2-Clause BSD](LICENSE), like csgo_gc.

## Credits

- **Mikko Kokko** - author of [csgo_gc](https://github.com/mikkokko/csgo_gc) and [csgo_invedit](https://github.com/mikkokko/csgo_invedit)
- **Theeto** - code reused in csgo_gc from its predecessor project, unusual loot lists
- **wr4th** - the CS2 port, CSForge, the upgrader
- [ByMykel/CSGO-API](https://github.com/ByMykel/CSGO-API) - item image links and case data
- [market.csgo.com](https://market.csgo.com) - the upgrader's prices

Counter-Strike and Counter-Strike 2 are trademarks of Valve Corporation.
