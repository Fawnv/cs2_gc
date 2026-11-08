cs2_gc: a local Game Coordinator for Counter-Strike 2
=====================================================

A port of csgo_gc (Mikko Kokko) from CS:GO to CS2. Inventory, loadouts, cases, trade ups,
stickers, charms, ranks, the Armory, a web inventory editor, the CSForge mod loader and the
upgrader all work locally, without Valve's servers. README.md has the details.

IMPORTANT
- With cs2_gc the game always runs with -insecure: VAC is off and official servers won't let
  you in. Launching CS2 from Steam as usual gives you the normal game, without cs2_gc.
- cs2_gc never modifies game files. Use a secondary Steam account, at least at first.
- Not affiliated with or endorsed by Valve.

REQUIREMENTS
- Windows 10 or 11 (x64), Steam running, CS2 installed.

HOW TO RUN
1. Extract this archive anywhere, just not into the game folder.
2. If CS2 isn't in C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive,
   set CS2_ROOT at the top of run_cs2_gc.cmd and cleanup_cs2_folder.cmd.
3. Run run_cs2_gc.cmd and keep its window open while you play. It puts cs2_gc.exe next to
   cs2.exe only while you play (CS2 doesn't start otherwise) and removes it when the game exits.
   Launch options are passed on to the game, e.g.: run_cs2_gc.cmd -novid -console
4. While the game is running, the inventory editor is at http://127.0.0.1:13001/
   (or run open_editor.cmd).

WHERE THINGS ARE
  release\cs2_gc.exe              the launcher
  release\cs2_gc\config.txt       settings: ranks, level, mods, the upgrader's prices
  release\cs2_gc\inventory.txt    your inventory (the game changes it)
  release\cs2_gc\gc_log.txt       the log, after the first start
  release\cs2_gc\editor\          the inventory editor page

MODS AND THE UPGRADER
The CSForge mod loader and the built-in interface fixes (the Upgrader tab in the inventory,
a Close button for the case window during a match) are turned on in config.txt with "mods" "1".
Mods go into release\cs2_gc\mods\, README.md explains how to make them.
The upgrader's chances come from market.csgo.com prices: the list is downloaded at game start
and kept in release\cs2_gc\prices_RUB.json.

IF SOMETHING WENT WRONG
If the run_cs2_gc.cmd window was closed early or the game crashed, run cleanup_cs2_folder.cmd:
it removes cs2_gc from the game folder and checks that nothing is left there.

License: 2-Clause BSD, see LICENSE.
