First public release of cs2_gc, a port of [csgo_gc](https://github.com/mikkokko/csgo_gc) to Counter-Strike 2. It answers the game's Game Coordinator traffic from inside the game process, so inventories, cases and ranks work locally, without Valve's servers.

> [!WARNING]
> Under cs2_gc the game always runs with `-insecure`. VAC is off and official servers won't let you in. Use a secondary Steam account, at least at first. This project is not affiliated with Valve.

## What's in it

- Inventory: equipping, cases including the newest ones, 7-day case rentals, stickers, charms, name tags, storage units, music kits, StatTrak swaps, souvenirs and the in-game store
- Trade up contracts, including five coverts for a knife or gloves
- The Armory pass, stars and rewards
- Level and XP saved in the inventory, XP for matches with bots and on community servers, the weekly care package and the Service Medal
- Competitive, Wingman and Premier ranks from the config
- A web inventory editor at http://127.0.0.1:13001/ for adding items and editing the ones you have
- CSForge, a mod loader for custom skins, music kits, sounds and the intro video that leaves the game folder untouched
- An Upgrader tab in the inventory, with chances from market.csgo.com prices

CSForge and the Upgrader are off by default. `"mods" "1"` in `release\cs2_gc\config.txt` turns them on.

Terminals, StatTrak swaps, Sticker Slabs, tournament items, XP for matches, sound and intro mods and the upgrader's market prices have only been checked by the offline self-test so far. Weekly missions and matchmaking aren't implemented. Windows only.

## Install

1. You need Windows 10 or 11 (x64), Steam running and CS2 installed.
2. Download the zip below and extract it anywhere outside the game folder.
3. If CS2 isn't in the default Steam library (`C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive`), set `CS2_ROOT` at the top of `run_cs2_gc.cmd` and `cleanup_cs2_folder.cmd`.
4. Run `run_cs2_gc.cmd` and keep its window open while you play.

CS2 only starts when the launcher sits next to `cs2.exe`, so `run_cs2_gc.cmd` copies `cs2_gc.exe` there for the session and removes it when the game closes. If the window was closed early or the game crashed, run `cleanup_cs2_folder.cmd`. Launching CS2 from Steam still gives you the normal game with VAC.

`README.txt` in the archive repeats these steps. The repository's README covers the config, mods and building from source.

If something breaks, open an issue with what you did, what happened and `release\cs2_gc\gc_log.txt`.
