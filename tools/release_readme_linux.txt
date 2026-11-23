cs2_gc: a local Game Coordinator for Counter-Strike 2 (Linux x64)
================================================================

A port of csgo_gc (Mikko Kokko) from CS:GO to CS2. Inventory, loadouts, cases, trade ups,
stickers, charms, ranks, the Armory, a web inventory editor, the CSForge mod loader and the
upgrader all work locally, without Valve's servers. README.md has the details.

IMPORTANT
- With cs2_gc the game always runs with -insecure: VAC is off and official servers won't let
  you in. Launching CS2 from Steam as usual gives you the normal game, without cs2_gc.
- cs2_gc never modifies game files. Use a secondary Steam account, at least at first.
- Not affiliated with or endorsed by Valve.

REQUIREMENTS
- Linux x86_64 (glibc 2.35+, Ubuntu 22.04+, Debian 12+, Fedora, Arch, SteamOS), Steam running, native CS2 installed.

HOW TO RUN
1. Extract this archive anywhere, just not into the game folder.
2. If CS2 is not in the default Steam folder (~/.steam/steam/steamapps/common/...),
   set the CS2_ROOT environment variable or edit run_cs2_gc.sh and cleanup_cs2_folder.sh.
3. Run ./run_cs2_gc.sh and keep the terminal open while you play. Launch options are passed on, e.g.:
   ./run_cs2_gc.sh -novid -console
4. While the game is running, the inventory editor is at http://127.0.0.1:13001/

WHERE THINGS ARE
  release_linux/cs2_gc_launcher       the launcher
  release_linux/cs2_gc/config.txt     settings: ranks, level, mods, the upgrader's prices
  release_linux/cs2_gc/inventory.txt  your inventory (the game changes it)
  release_linux/cs2_gc/inventory.txt.bak the inventory as the last start read it
  release_linux/cs2_gc/mods/          CSForge mods; install a mod's .zip in the editor's Mods tab
  release_linux/cs2_gc/gc_log.txt     the log, after the first start
  release_linux/cs2_gc/editor/        the inventory editor page

MODS AND THE UPGRADER
The CSForge mod loader and the built-in interface fixes (the Upgrader tab in the inventory,
a Close button for the case window during a match) are turned on in config.txt with "mods" "1".
Mods go into release_linux/cs2_gc/mods/, README.md explains how to make them.
The upgrader's chances come from market.csgo.com prices. cs2_gc downloads the list at game start
and keeps it in release_linux/cs2_gc/prices_RUB.json.

IF SOMETHING WENT WRONG
If the run_cs2_gc.sh terminal was closed early or the game crashed, run ./cleanup_cs2_folder.sh:
it removes cs2_gc from the game folder and checks that nothing is left there.

License: 2-Clause BSD, see LICENSE.
