# cs2_gc

> [!WARNING]
> cs2_gc always runs the game with `-insecure`: VAC is off and official servers won't let you in. It is not affiliated with or endorsed by Valve. Use it at your own risk, ideally on a secondary Steam account.
---
> [!NOTE]
> Russian README.MD at the bottom of the file | Русский README.MD внизу файла

In Valve games, the Game Coordinator (GC) is the backend service behind inventories, loadouts, cases, stickers and ranks. cs2_gc is a port of [csgo_gc](https://github.com/mikkokko/csgo_gc) to Counter-Strike 2. It redirects the game's GC traffic to an implementation that runs inside the game process, so everything works offline and locally. Your inventory lives in `cs2_gc\inventory.txt`.

## Features

- **Inventory:** equipping (CS2 equip slots), case opening including the newest cases, 7-day case rentals, stickers (with rotation, scale and offset, scraping to any wear or off), charms (applying and removing), name tags, storage units, music kits, StatTrak and StatTrak swaps, souvenirs and the in-game store
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

The GC saves the inventory to `inventory.txt.tmp` and then renames it over `inventory.txt`, so a crash in the middle of a save leaves the old file intact. At every start the file, once read, is copied to `inventory.txt.bak`. A damaged `inventory.txt` (unreadable, an unclosed block or string, a `}` too many) stops the game with a message instead of being read in part and saved over: fix it, restore `inventory.txt.bak`, or delete it to start a new inventory. The tools in `tools/` leave `inventory.txt` alone while `cs2_gc.exe` is running.

## Configuration

See [examples/config.txt](examples/config.txt) (it becomes `release\cs2_gc\config.txt`). The main options:

| Option | What it does |
|---|---|
| `ranks` | Competitive, Wingman and Premier rank and wins |
| `player_level`, `player_cur_xp` | level and XP of a new inventory; after that they're kept in `inventory.txt` |
| `mods` | `1` turns on CSForge, the built-in interface fixes and the mods, the upgrader among them |
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
  items_game_override.txt  changes to entries the schema already has, merged deep
  items_extra.txt          schema sections only the GC reads (a case's knives)
  panorama.txt             your code put into the game's interface scripts
  files\...                compiled game files (.vtex_c, .vsnd_c, .vmdl_c...) at their game paths
```

- `items_game.txt` sections are appended to the game's schema. Ids the game or an earlier mod already has are skipped (duplicates crash the client), existing entries can't be changed.
- `localization.txt` tokens are added to every language in `"mod_languages"` (English and Russian by default) and replace existing ones, so anything in the game can be renamed.
- For the same path in `files\`, the mod with the higher `priority` wins. Mods load by priority, then by folder name.
- `items_game_override.txt` changes what's there already, at any depth: a text replaces the text at the same place, a block adds to the block there. For example, `"client_loot_lists" { "crate_community_33_ancient" { "[my_kit]weapon_ak47" "1" } }` puts a skin into the Kilowatt Case.
- `items_extra.txt` has schema sections only the GC reads, like a case's knife list (the game keeps its own the same way).
- `panorama.txt` puts code into a compiled script: `"panorama" { "1" { "file" "panorama/scripts/mainmenu.vts_c" "anchor" "anchor.js" "code" "code.js" } }` inserts `code.js` in front of the text of `anchor.js`, which must be in the script exactly once.
- A `*_c` file whose header isn't a compiled Source 2 file's is left out, the game could crash on it. The mods' texts always go into the game's own language too (`-language`, or what Steam set for CS2).
- `CSForge:` lines in `gc_log.txt` list the mods, conflicts, skipped ids and the built VPK. The Mods tab of the inventory editor shows every mod's errors and warnings from the build, installs a mod from a `.zip` (the button, or drop it on the tab) and deletes one (it goes to `cs2_gc\mods\_trash`).

Compile your assets with the free CS2 Workshop Tools (`resourcecompiler`). Use ids above 10100 for paint kits and above 110 for music kits.

### Mod tools

All of them work without the Workshop Tools. They only read the game folder and write mods into the data folder: `--data release\cs2_gc` by default, and before a build (no `release\` yet) `_data` next to the tools, made on first use.

Needs `pip install numpy pillow` for the skin, sticker and case tools and the skin editor.

| Tool | What it makes |
|---|---|
| `tools/make_demo_mod.py` | a demo mod: a custom AK-47 finish and the old CS:GO theme in the menu |
| `tools/make_skin_mod.py` | a weapon skin from any picture, with inventory icons. Needs `pip install numpy pillow` |
| `tools/skin_editor.py` | a skin editor in the browser: paint on the texture layout or on the 3D model, then build the skin |
| `tools/make_music_mod.py` | a music kit from one mp3: replaces the tracks of an existing kit and renames it. `--cutter` opens a page to pick each slot's start on the waveform. [music_kits.md](music_kits.md) lists the kits |
| `tools/make_sound_mod.py` | replaces any game sound (shots, footsteps, the bomb, UI) with your own file. `--list ak47` shows the game's sounds |
| `tools/make_intro_mod.py` | replaces the COUNTER-STRIKE 2 intro video |
| `tools/make_bot_mod.py` | sets the bot difficulty per game mode (the game has no menu for it) |
| `tools/make_case_mod.py` | a keyless case of your skins: loot lists by rarity, a collection, knives as the rare special item. Also in the skin editor's "My skins" |
| `tools/make_sticker_mod.py` | a sticker from any picture: paper, holo, glitter, foil or gold (`--finish`; `--mask` limits holo and glitter to the white of a mask), with its inventory icon. Also the skin editor's "Stickers" window: pick, drop or paste a picture, pick the finish, build. The finishes are switches of the game's sticker shader, so each starts from a game sticker that has it and gets textures made from your picture |
| `tools/pack_mod.py` | packs a mod into a `.zip` the Mods tab installs |
| `tools/renumber_mod.py` | gives a mod free ids where the game or an earlier mod has its ids (`--move-items` moves the inventory's items too) |

The tools convert other audio formats to MP3 with ffmpeg, or with the codecs built into Windows when ffmpeg isn't installed.

`make_skin_mod.py` wraps the picture around both sides of the weapon, over the weapon's own colors. To paint the texture yourself, `--template ak47 ak47.png` saves the weapon's texture layout and `--mode uv` builds a skin from the painted file. `--finish` picks the finish: `custom` (paint, the default), `gunsmith` (paint over metal, `--paint-mask` marks the bare metal) or `patina` (the texture is the metal color). `--colors`, `--roughness` and `--pearlescence` set the finish's values. Every weapon gets its own paint kit, and its inventory icons are drawn from the same texture. `--weapons` lists what it can paint: the 55 guns and knives, the six grenades and the C4. `previews\` in the mod folder shows the result before you start the game.

The game doesn't draw paint kits on grenades and the C4 (tested: even with the maps and the paintable flag the client never starts the paint for them), so a grenade skin replaces the grenade's own texture: the mod carries the painted texture and a copy of the grenade's material pointing at it. Every grenade of that kind looks like that, the bots' too, there is no wear, and the newest skin of each grenade is the one in use (removing it brings back the previous one). `tools/mesh_inputs.py` bakes the maps the editor and the icons need from the model (the meshoptimizer-compressed mesh is read by `source2.py`). On the molotov only the rag, neck and cap take paint (the bottle is a separate glass and liquid material). `--add-item` still puts the item into the inventory, equipped.

`python tools\skin_editor.py` opens the same thing as a page (http://127.0.0.1:13002/, it finds CS2 through Steam; `--data` picks another data folder). The texture layout is on the left and the weapon's 3D model is on the right, and the brush, eraser and fill work on both. A picture can go on the layout as a layer you move and turn, or onto the model as a sticker projected from the current view. "Game light" shades the model with the weapon's own baked normals and ambient occlusion and the matte paint of Valve's custom paint material, close to how the game draws it (lighting on a map still varies). The finish (custom paint, gunsmith or patina) is picked at the top, with its metal colors, roughness and pearlescence on the right; on gunsmith a layer ticked "Metal" becomes bare metal. Grenades and the C4 are a group of their own in the weapon list (their maps take a few seconds to bake the first time); their skins replace the grenade's own texture. "Build skin" writes the skin into `mods\skins` like `--mode uv` and keeps the painted texture in `mods\skins\sources\`, so the skin can be opened and painted again later.

```bat
python tools\make_skin_mod.py --cs2 "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive" --data release\cs2_gc --image pic.png --weapon ak47 --name "My AK" --add-item
python tools\make_music_mod.py --cs2 "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive" --data release\cs2_gc --mp3 track.mp3 --name "My kit" --kit theverkkars_01 --add-item
python tools\make_sound_mod.py --cs2 "..." --data release\cs2_gc --audio shot.mp3 --sound "sounds/weapons/ak47/ak47_0*" --length 0.6
python tools\make_bot_mod.py --cs2 "..." --data release\cs2_gc --difficulty expert
```

### Built-in interface fixes

With mods on, CSForge also patches the game's own Panorama scripts at every start (`"panorama_fixes" "1"`). The current scripts are read from the game, so a game update can't bring back an old copy, and a fix whose anchor is gone is skipped with a line in the log.

- A case without a key used to open a window with no buttons at all during a match, not even Close. Now it opens for viewing only, with Close.
## Upgrader

The mod `mods\upgrader`, which comes with cs2_gc (needs `"mods" "1"`, turn it off in the Mods tab): an *Upgrader* tab in the inventory, and *Upgrade* in a skin's right-click menu. Put up to 10 skins in (knives and gloves too) and pick a target. If the wheel stops in the chance arc you get the target, otherwise the skins are gone.

- **Chance:** what the bet is worth divided by the target's price. A skin worth 50 for one worth 100 is 50%. The target has to cost more than the bet, so cheaper targets aren't offered.
- **Targets:** a skin in a given wear (FN, MW, FT, WW, BS), cheapest first, each with its price and your chance. Filters: rarity, type, same weapon, StatTrak™, and x1.5-x10 multipliers that only show targets at least that many times the bet. The skin you win gets a float within the wear you picked.
- **Prices:** [market.csgo.com](https://market.csgo.com)'s public price list (lowest offers, no key needed). cs2_gc downloads it at game start when the saved one (`cs2_gc\prices_RUB.json`) is older than 12 hours and keeps using the old one when there's no internet. A price depends on the wear, StatTrak™ and souvenir status, like on the market. A skin that isn't on sale in its wear can't go in. Doppler phases and gems share one market name and so one price.
- **No price list:** with `"upgrader_prices" "off"`, or without internet on the first start, the chances go by rarity, 1:10 per rarity step.

The roll happens in the GC, with the same prices the window shows. The won skin stays hidden until the wheel stops.

The window is the mod's JavaScript (`mods\upgrader\panorama\`), put into the inventory scripts through its `panorama.txt`, so it can be changed without building cs2_gc. The GC fills in the catalog (`@csforge:upgrader_catalog@`) and does the bets, so the mod says `"needs" "upgrader 1"`: a copy made for another version of cs2_gc isn't applied at all, and the Mods tab says so. `build.cmd` refreshes the mod in `release\cs2_gc\mods` and keeps your `mod.txt` (on/off, priority).

## Building

Requires Visual Studio 2022 with the C++ workload, which includes CMake and vcpkg.

```bat
build.cmd
```

The first build takes 20-30 minutes while vcpkg builds protobuf, mbedtls and libwebsockets. The result is in `release\`, and rebuilding never overwrites your `config.txt` and `inventory.txt`.

`python tools\make_release_zip.py --exe release\cs2_gc.exe` packs a release zip for GitHub Releases: only `cs2_gc.exe` comes from the build, the rest from this tree (clean example `config.txt` and `inventory.txt`, the scripts, the editor page, the built-in upgrader mod, `README.txt`), in the `release\cs2_gc.exe` + `release\cs2_gc\...` layout the scripts expect. The zip goes to `dist\` as `cs2_gc-<date>-<commit>.zip` (`--out`, `--name` and `--source` change that).

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
mods/upgrader/ the upgrader mod, installed into release\cs2_gc\mods by the build
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

---

# cs2_gc RUS README

> [!WARNING]
> cs2_gc всегда запускает игру с параметром `-insecure`: VAC отключён, на официальные серверы не пустит. Проект никак не связан с Valve и не одобрен ей. Используйте на свой страх и риск, лучше на втором аккаунте Steam.

В играх Valve за инвентарь, снаряжение, кейсы, наклейки и звания отвечает серверная служба Game Coordinator (GC). cs2_gc переносит [csgo_gc](https://github.com/mikkokko/csgo_gc) на Counter-Strike 2. Он перенаправляет трафик GC в локальную реализацию внутри самого процесса игры, так что всё работает офлайн и у вас на ПК. Инвентарь хранится в `cs2_gc\inventory.txt`.

## Возможности

- **Инвентарь:** экипировка (слоты снаряжения CS2), открытие кейсов (включая самые новые), аренда кейсов на 7 дней, наклейки (поворот, масштаб, смещение, потёртость до любой степени или полное снятие), брелоки (нанесение и снятие), именные ярлыки, хранилища, наборы музыки, StatTrak и перенос StatTrak, сувенирные предметы и внутриигровой магазин
- **Контракты обмена**, включая пять тайных скинов на нож или перчатки
- **Арсенал:** активация пропуска, звёзды и награды
- **Прогресс:** уровень и опыт хранятся в файле инвентаря, опыт начисляется за матчи с ботами и на серверах сообщества, работают еженедельный набор и медаль за службу на 40-м уровне
- **Терминалы** (Genesis, Dead Hand), плашки для наклеек (Sticker Slabs), турнирные предметы (пропуски зрителя, сувенирные жетоны) и ссылки на осмотр для собственных предметов
- **Звания:** Соревновательный, Напарники и рейтинг Премьер, задаются в конфиге
- **Веб-редактор инвентаря** встроен прямо в GC: добавляйте любой предмет из каталога всей игры, меняйте уже имеющиеся (редкость, качество, износ, паттерн, счётчик StatTrak, именной ярлык), правки сразу появляются в игре
- **Загрузчик модов CSForge:** кастомные скины, наборы музыки, звуки, вступительное видео, тексты и замена любых файлов игры без вмешательства в папку с CS2
- **Апгрейдер:** вкладка в инвентаре, куда можно закинуть скины ради шанса выбить более дорогой, с шансами по реальным ценам с маркета
- **Выделенный сервер** на Windows без клиента Steam (экспериментально)

> [!NOTE]
> Большая часть функций проверена прямо в игре. Несколько новых вещей (терминалы, перенос StatTrak, плашки для наклеек, турнирные предметы, опыт за матчи, моды на звуки и интро, цены апгрейдера с маркета) пока проверялись только офлайн-самотестом. Еженедельные миссии и подбор матчей не реализованы. Работает только на Windows.

## Безопасность

- Игра всегда запускается с `-insecure`. Лаунчер сам перезапустится с этим флагом, если его нет, и откажется принимать `-trusted`. Локальный сервер тоже не сможет стать защищённым (`BSecureAllowed` возвращает `false`, флаг защиты вырезается из `InitGameServer`).
- Файлы игры не трогаются. cs2_gc только читает DLL игры и `pak01_dir.vpk`. Всё, что он записывает (конфиг, инвентарь, лог, моды), лежит в его собственной папке `cs2_gc\`.
- CS2 запускается только если лаунчер лежит рядом с `cs2.exe`, поэтому `run_cs2_gc.cmd` копирует `cs2_gc.exe` в `game\bin\win64\` лишь на время игры, а после выхода удаляет его вместе с дампами крашей. `cleanup_cs2_folder.cmd` делает ту же очистку вручную и проверяет, чтобы не осталось ничего с именем `cs2_gc*`.
- Обычный запуск CS2 из Steam открывает стандартную игру с VAC. cs2_gc в неё не вмешивается.

## Установка

1. Требования: Windows 10 или 11 (x64), запущенный Steam, установленная CS2.
2. Скачайте последний архив со [страницы релизов](../../releases/latest) и распакуйте куда угодно за пределы папки с игрой.
3. Если CS2 стоит не в `C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive`, пропишите свой путь в `CS2_ROOT` в начале файлов `run_cs2_gc.cmd` и `cleanup_cs2_folder.cmd`.
4. Запустите `run_cs2_gc.cmd` и не закрывайте его окно, пока играете. Параметры запуска передаются прямо в игру, например: `run_cs2_gc.cmd -novid -console`.
5. Если окно закрыли раньше времени или игра вылетела, запустите `cleanup_cs2_folder.cmd`.

Лог пишется в `release\cs2_gc\gc_log.txt` и в консоль игры (строки с `[GC]`).

## Редактирование инвентаря

Пока игра запущена, откройте http://127.0.0.1:13001/ (или запустите `open_editor.cmd`) и нажмите *Connect*.

- **Catalogue (Каталог):** все предметы текущей версии игры (оружие и раскраски, ножи, перчатки, наклейки, брелоки, наборы музыки, граффити, нашивки) с поиском по названию, внутреннему имени или ID. Кнопка *Add to inventory* сразу отправляет предмет в игру. Команда `tools/gen_invedit_catalog.py --lang <language>` соберёт каталог с названиями на другом языке игры.
- **Edit item (Правка предмета):** меняет у выбранного предмета редкость, качество (включая звёздочку ★ у ножей), износ, паттерн, счётчик StatTrak и именной ярлык.
- **Mods (Моды):** включает и выключает моды CSForge и меняет порядок их загрузки. Изменения сработают при следующем запуске игры.

Всё сохраняется в `inventory.txt`. Порт задаётся параметром `"inventory_editor_port"`. Чтобы подключить другой редактор от csgo_gc, укажите его `Origin` в `"inventory_editor_origin"`.

Формат `inventory.txt` такой же, как в csgo_gc, поэтому старые офлайн-редакторы тоже подойдут. Редактируйте файл только при закрытой игре, иначе GC перезапишет его поверх.

GC сначала сохраняет инвентарь в `inventory.txt.tmp`, а затем переименовывает его в `inventory.txt`, так что краш в момент записи не испортит старый файл. При каждом запуске прочитанный файл копируется в `inventory.txt.bak`. Если `inventory.txt` повреждён (не читается, незакрытый блок или строка, лишняя `}`), игра остановится с ошибкой вместо того, чтобы загрузить файл наполовину и перезаписать его: почините файл, восстановите `inventory.txt.bak` или удалите его для чистого инвентаря. Скрипты в `tools/` не трогают `inventory.txt`, пока запущен `cs2_gc.exe`.

## Настройка

Смотрите [examples/config.txt](examples/config.txt) (в сборке он лежит как `release\cs2_gc\config.txt`). Основные параметры:

| Параметр | Что делает |
|---|---|
| `ranks` | звания и победы в Соревновательном, Напарниках и Премьере |
| `player_level`, `player_cur_xp` | стартовый уровень и опыт для нового инвентаря; дальше они хранятся в `inventory.txt` |
| `mods` | `1` включает CSForge, встроенные фиксы интерфейса и моды, включая апгрейдер |
| `panorama_fixes` | встроенные фиксы интерфейса, включены по умолчанию, если активны моды |
| `match_xp` | опыт за матчи с ботами и на серверах сообщества |
| `upgrader_prices`, `upgrader_prices_hours` | валюта прайс-листа апгрейдера (`RUB`, `USD`, `EUR` или `off`) и частота обновления |
| `inventory_editor_port` | порт редактора, по умолчанию 13001 |
| `log_output` | 0 ничего, 1 только консоль игры, 2 консоль и `gc_log.txt` |

## Моды (CSForge)

При запуске игры CSForge собирает все включённые моды в один VPK (`cs2_gc\mods\_build\csforge_dir.vpk`) и монтирует его поверх родных файлов игры прямо из процесса. Папка с игрой остаётся нетронутой. Включается параметром `"mods" "1"`. Если с модами игра вылетает, верните `0`.

Мод лежит в своей папке внутри `cs2_gc\mods\`, каждый файл внутри необязателен:

```
cs2_gc\mods\<mod>\
  mod.txt                  "mod" { "name" "..." "author" "..." "version" "..." "enabled" "1" "priority" "0" }
  items_game.txt           "items_game" { "paint_kits" { ... } "music_definitions" { ... } ... }
  localization.txt         "lang" { "Tokens" { "PaintKit_my_skin_Tag" "My skin" } }
  localization_<lang>.txt  то же самое для одного языка
  items_game_override.txt  правки существующих записей схемы, объединяются рекурсивно
  items_extra.txt          секции схемы, которые читает только GC (например, ножи в кейсе)
  panorama.txt             ваш код для вставки в скрипты интерфейса игры
  files\...                скомпилированные файлы игры (.vtex_c, .vsnd_c, .vmdl_c...) по их игровым путям
```

- Секции `items_game.txt` дописываются к схеме игры. ID, которые уже заняты игрой или более ранним модом, пропускаются (дубликаты крашат клиент), существующие записи менять нельзя.
- Токены из `localization.txt` добавляются во все языки из `"mod_languages"` (по умолчанию английский и русский) и перезаписывают старые, так что в игре можно переименовать что угодно.
- Если в `files\` совпадают пути, побеждает мод с более высоким `priority`. Моды грузятся по приоритету, а затем по имени папки.
- `items_game_override.txt` меняет уже существующие записи на любой глубине: строка заменяет строку на том же месте, блок дополняет существующий блок. Например, `"client_loot_lists" { "crate_community_33_ancient" { "[my_kit]weapon_ak47" "1" } }` добавит ваш скин в кейс «Киловатт».
- `items_extra.txt` хранит секции схемы, которые читает только GC, например список ножей в кейсе (сама игра хранит свои так же).
- `panorama.txt` вставляет код в скомпилированный скрипт: `"panorama" { "1" { "file" "panorama/scripts/mainmenu.vts_c" "anchor" "anchor.js" "code" "code.js" } }` вставит `code.js` перед текстом из `anchor.js`, который должен встречаться в скрипте ровно один раз.
- Файл `*_c` без заголовка скомпилированного файла Source 2 пропускается, иначе игра может вылететь. Тексты модов всегда попадают и в текущий язык игры (`-language` или язык CS2 в Steam).
- Строки `CSForge:` в `gc_log.txt` показывают список модов, конфликты, пропущенные ID и собранный VPK. Вкладка Mods в редакторе инвентаря выводит ошибки и предупреждения сборки по каждому моду, ставит моды из `.zip` (через кнопку или перетаскиванием на вкладку) и удаляет их (переносит в `cs2_gc\mods\_trash`).

Ассеты компилируются бесплатными CS2 Workshop Tools (`resourcecompiler`). Для раскрасок берите ID выше 10100, для наборов музыки выше 110.

### Инструменты для модов

Все скрипты работают без Workshop Tools. Они только читают папку с игрой и пишут моды в папку данных: по умолчанию это `--data release\cs2_gc`, а до сборки (пока папки `release\` нет) рядом со скриптами при первом запуске создаётся `_data`.

Для инструментов скинов, наклеек, кейсов и редактора скинов нужно выполнить `pip install numpy pillow`.

| Инструмент | Что делает |
|---|---|
| `tools/make_demo_mod.py` | демо-мод: кастомный скин на AK-47 и старая тема CS:GO в меню |
| `tools/make_skin_mod.py` | скин на оружие из любой картинки вместе с иконками инвентаря. Нужен `pip install numpy pillow` |
| `tools/skin_editor.py` | редактор скинов в браузере: рисуйте на развёртке текстуры или прямо по 3D-модели, затем собирайте скин |
| `tools/make_music_mod.py` | набор музыки из одного mp3: заменяет треки существующего набора и переименовывает его. `--cutter` открывает страницу, где на волне можно выбрать старт для каждого слота. Список наборов в [music_kits.md](music_kits.md) |
| `tools/make_sound_mod.py` | заменяет любой звук игры (выстрелы, шаги, бомбу, интерфейс) на ваш файл. `--list ak47` покажет звуки игры |
| `tools/make_intro_mod.py` | заменяет вступительное видео COUNTER-STRIKE 2 |
| `tools/make_bot_mod.py` | задаёт сложность ботов для каждого режима (в самой игре такого меню нет) |
| `tools/make_case_mod.py` | кейс без ключа из ваших скинов: списки дропа по редкости, коллекция, ножи как редкий особый предмет. Доступен также в разделе «My skins» редактора скинов |
| `tools/make_sticker_mod.py` | наклейка из любой картинки: бумажная, голографическая, блёстки, металлическая или золотая (`--finish`; `--mask` ограничивает голограмму и блёстки белой областью маски), сразу с иконкой инвентаря. Работает и через окно «Stickers» в редакторе скинов: выберите, перетащите или вставьте картинку, укажите тип и соберите. Типы задаются флагами игрового шейдера наклеек, поэтому каждый берёт за основу игровую наклейку с нужным эффектом и подставляет текстуры из вашей картинки |
| `tools/pack_mod.py` | пакует мод в `.zip`, который ставится через вкладку Mods |
| `tools/renumber_mod.py` | выдаёт моду свободные ID, если старые уже заняты игрой или другим модом (`--move-items` перенесёт и предметы в инвентаре) |

Другие аудиоформаты конвертируются в MP3 через ffmpeg, а если его нет, то встроенными кодеками Windows.

`make_skin_mod.py` оборачивает картинку вокруг обеих сторон оружия поверх его базовых цветов. Если хотите рисовать текстуру сами, `--template ak47 ak47.png` сохранит развёртку оружия, а `--mode uv` соберёт скин из готового файла. Флаг `--finish` задаёт тип покрытия: `custom` (краска, по умолчанию), `gunsmith` (краска по металлу, `--paint-mask` отмечает голый металл) или `patina` (текстура задаёт цвет металла). Параметры `--colors`, `--roughness` и `--pearlescence` настраивают свойства покрытия. Каждое оружие получает собственный paint kit, а иконки инвентаря рендерятся с той же текстуры. `--weapons` покажет всё, что можно покрасить: 55 пушек и ножей, шесть гранат и C4. Папка `previews\` внутри мода покажет результат ещё до запуска игры.

Игра не накладывает paint kit на гранаты и C4 (проверено: даже с нужными картами и флагом окраски клиент их не красит), поэтому скин для гранаты заменяет её родную текстуру: мод несёт покрашенную текстуру и копию материала гранаты со ссылкой на неё. Так будут выглядеть все гранаты этого типа, даже у ботов, износа у них нет, а активным всегда остаётся самый новый скин для каждой гранаты (после его удаления вернётся предыдущий). Скрипт `tools/mesh_inputs.py` запекает из модели карты, нужные редактору и иконкам (сжатые через meshoptimizer меши читает `source2.py`). У коктейля Молотова красятся только тряпка, горлышко и крышка (бутылка использует отдельный материал стекла и жидкости). Флаг `--add-item` всё так же добавляет предмет в инвентарь и экипирует его.

Команда `python tools\skin_editor.py` открывает то же самое в браузере (http://127.0.0.1:13002/, путь к CS2 ищется через Steam; `--data` выбирает другую папку данных). Слева развёртка текстуры, справа 3D-модель оружия, кисть, ластик и заливка работают в обеих областях. Картинку можно кинуть на развёртку отдельным слоем (двигать и крутить) или спроецировать на модель как наклейку с текущего ракурса. Режим «Game light» освещает модель с её родными запечёнными нормалями, ambient occlusion и матовой краской кастомного материала Valve, близко к тому, как это выглядит в игре (освещение на картах всё равно отличается). Тип покрытия (custom paint, gunsmith или patina) выбирается сверху, а цвета металла, шероховатость и перламутр справа; в режиме gunsmith слой с галочкой «Metal» становится голым металлом. Гранаты и C4 вынесены в отдельную группу в списке оружия (при первом открытии их карты запекаются несколько секунд), их скины заменяют родную текстуру гранаты. Кнопка «Build skin» сохраняет скин в `mods\skins` аналогично `--mode uv` и оставляет исходную текстуру в `mods\skins\sources\`, чтобы потом её можно было открыть и дорисовать.

```bat
python tools\make_skin_mod.py --cs2 "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive" --data release\cs2_gc --image pic.png --weapon ak47 --name "My AK" --add-item
python tools\make_music_mod.py --cs2 "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive" --data release\cs2_gc --mp3 track.mp3 --name "My kit" --kit theverkkars_01 --add-item
python tools\make_sound_mod.py --cs2 "..." --data release\cs2_gc --audio shot.mp3 --sound "sounds/weapons/ak47/ak47_0*" --length 0.6
python tools\make_bot_mod.py --cs2 "..." --data release\cs2_gc --difficulty expert
```

### Встроенные фиксы интерфейса

Когда моды включены, CSForge при каждом запуске патчит родные скрипты Panorama (`"panorama_fixes" "1"`). Актуальные скрипты берутся прямо из игры, поэтому после обновления CS2 старые версии не всплывут, а фикс с пропавшим якорем просто пропустится с записью в лог.

- Кейс без ключа раньше открывал во время матча окно вообще без кнопок, даже без «Закрыть». Теперь он открывается только для просмотра и с кнопкой закрытия.

## Апгрейдер

Мод `mods\upgrader`, идущий в комплекте с cs2_gc (нужен `"mods" "1"`, отключается во вкладке Mods): добавляет вкладку *Upgrader* в инвентарь и пункт *Upgrade* в контекстное меню скина по ПКМ. Выберите до 10 скинов (ножи и перчатки тоже подходят) и укажите цель. Если стрелка остановится в секторе шанса, вы получите цель, иначе поставленные скины сгорят.

- **Шанс:** стоимость ставки делится на цену цели. Скин за 50 на скин за 100 даёт 50%. Цель должна стоить дороже ставки, более дешёвые варианты не предлагаются.
- **Цели:** скин в конкретном качестве износа (FN, MW, FT, WW, BS), от дешёвых к дорогим, рядом указаны цена и ваш шанс. Фильтры: редкость, тип, то же оружие, StatTrak™ и множители x1.5-x10, оставляющие только цели минимум во столько раз дороже ставки. Выигранный скин получает float внутри выбранного износа.
- **Цены:** публичный прайс-лист [market.csgo.com](https://market.csgo.com) (минимальные предложения, API-ключ не нужен). cs2_gc скачивает его при запуске игры, если сохранённый файл (`cs2_gc\prices_RUB.json`) старше 12 часов, и использует старый, если нет интернета. Цена зависит от износа, StatTrak™ и статуса сувенирного предмета, как на маркете. Скин, которого нет в продаже в своём износе, поставить нельзя. Фазы и гемы Doppler имеют одно название на маркете, поэтому и цена у них общая.
- **Без прайс-листа:** при `"upgrader_prices" "off"` или без интернета при первом запуске шансы считаются по редкости, 1:10 за каждый шаг редкости.

Розыгрыш проходит внутри GC по тем же ценам, что и в окне. Выигранный скин не появляется, пока колесо не остановится.

Само окно написано на JavaScript (`mods\upgrader\panorama\`) и внедряется в скрипты инвентаря через `panorama.txt`, так что его можно править без пересборки cs2_gc. Каталог (`@csforge:upgrader_catalog@`) заполняет GC, он же рассчитывает ставки, поэтому в моде указано `"needs" "upgrader 1"`: копия от другой версии cs2_gc не применится, о чём сообщит вкладка Mods. Скрипт `build.cmd` обновляет мод в `release\cs2_gc\mods`, но не трогает ваш `mod.txt` (вкл/выкл и приоритет).

## Сборка

Понадобится Visual Studio 2022 с компонентом разработки на C++, куда уже входят CMake и vcpkg.

```bat
build.cmd
```

Первая сборка занимает 20-30 минут, пока vcpkg собирает protobuf, mbedtls и libwebsockets. Готовые файлы попадают в `release\`, а повторная сборка не перезаписывает ваши `config.txt` и `inventory.txt`.

Команда `python tools\make_release_zip.py --exe release\cs2_gc.exe` собирает архив для GitHub Releases: из билда берётся только `cs2_gc.exe`, остальное подтягивается из репозитория (чистые примеры `config.txt` и `inventory.txt`, скрипты, страница редактора, встроенный мод апгрейдера, `README.txt`) в структуре `release\cs2_gc.exe` + `release\cs2_gc\...`, которую ждут скрипты. Архив сохраняется в `dist\` под именем `cs2_gc-<дата>-<коммит>.zip` (меняется флагами `--out`, `--name` и `--source`).

### Офлайн-самотест

`cs2_gc_selftest.exe` запускается без игры и Steam и проверяет чтение VPK и схемы предметов, открытие каждого кейса, контракты, аренду, магазин, протокол редактора, сборку CSForge, апгрейдер и остальное. Папку игры он только читает, а пишет исключительно в указанную папку данных. Предварительно скопируйте туда `items_extra.txt` и `price_sheet.txt` из `examples\`.

```bat
build\tests\cs2_gc_selftest.exe "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive" "%TEMP%\cs2_gc_test"
```

Флаг `--serve <seconds>` поднимает только сервер редактора поверх `inventory.txt` из папки данных, чтобы потестировать страницу без запуска игры. Флаг `--prices <currency>` скачивает прайс-лист для апгрейдера.

### После обновления игры

- `tools/dump_protos.py` и `tools/gen_message_names.py` обновляют описания сообщений GC из `client.dll`.
- `tools/gen_unusual_lists.py` выводит списки ножей и перчаток из новых кейсов для `examples/items_extra.txt` (в публичном `items_game.txt` их нет).
- `tools/gen_invedit_catalog.py` пересобирает каталог редактора.

## Как это работает

```
cs2_gc.exe (лаунчер)
 ├─ при необходимости перезапускает себя с -insecure
 ├─ находит CS2 (libraryfolders.vdf / -cs2path / собственная папка)
 ├─ загружает game\bin\win64\tier0.dll, затем engine2 через tier0!Plat_LoadModule
 ├─ устанавливает GC:
 │   ├─ находит steamclient64.dll через реестр (HKCU\Software\Valve\Steam\ActiveProcess)
 │   ├─ перехватывает steamclient64!CreateInterface с помощью прокси ISteamClient023
 │   │     ├─ SteamGameCoordinator001 → наш GC (ClientGC / ServerGC)
 │   │     └─ SteamUser023 / SteamGameServer015 / SteamUtils010 / SteamMatchMakingServers002 / SteamFriends017
 │   └─ перехватывает SteamAPI_RegisterCallback / RunCallbacks (для GCMessageAvailable_t)
 └─ вызывает Source2Main(hInst, 0, cmdline, nShow, "<game\bin\win64>", "csgo"), как это делает cs2.exe
```

Protobuf-описания GC для CS2 выгружаются из дескрипторов в `client.dll`, структуры интерфейсов Steam берутся из плоского API в `steam_api64.dll`, а `items_game.txt` читается напрямую из `pak01_dir.vpk` во время работы.

## Структура проекта

```
cs2_gc/        сам GC: клиентская и серверная логика, инвентарь, схема предметов, хуки Steam, VPK, CSForge
launcher/      cs2_gc.exe
protobufs/     protobuf-файлы GC для CS2 (выгружены из client.dll); в csgo_original/ лежат файлы csgo_gc для сравнения
steamworks/    заголовки Steamworks и сгенерированные прокси интерфейсов
tests/         офлайн-самотест
tools/         дамперы protobuf и интерфейсов, генератор каталога редактора, инструменты для модов, упаковка релизов
mods/upgrader/ мод апгрейдера, копируется в release\cs2_gc\mods при сборке
editor/        веб-редактор инвентаря (форк csgo_invedit) с каталогом CS2
examples/      config.txt, inventory.txt, items_extra.txt, price_sheet.txt, демо-мод
reference/     оригинальный README от csgo_gc
```

## Сообщения об ошибках

Откройте issue и опишите, что вы делали, что произошло, и приложите `gc_log.txt`. Если проблема не похожа на баг GC, сначала проверьте, не повторяется ли она в обычной игре, запущенной из Steam.

## Лицензия

[2-Clause BSD](LICENSE), как и у csgo_gc.

## Авторы и благодарности

- **Mikko Kokko** - автор [csgo_gc](https://github.com/mikkokko/csgo_gc) и [csgo_invedit](https://github.com/mikkokko/csgo_invedit)
- **Theeto** - код, перенесённый в csgo_gc из предыдущего проекта, списки редкого дропа
- **wr4th** - порт на CS2, CSForge, апгрейдер
- [ByMykel/CSGO-API](https://github.com/ByMykel/CSGO-API) - ссылки на картинки предметов и данные кейсов
- [market.csgo.com](https://market.csgo.com) - цены для апгрейдера

Counter-Strike и Counter-Strike 2 являются товарными знаками Valve Corporation.

## Star History

<a href="https://www.star-history.com/?repos=fawnv%2Fcs2_gc&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=fawnv/cs2_gc&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=fawnv/cs2_gc&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=fawnv/cs2_gc&type=date&legend=top-left" />
 </picture>
</a>