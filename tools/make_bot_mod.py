"""Bot difficulty for matches with bots, as a CSForge mod (CS2 has no difficulty choice in the menu).

    python tools\\make_bot_mod.py --cs2 "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive" --data release\\cs2_gc
    python tools\\make_bot_mod.py --cs2 "..." --data release\\cs2_gc --difficulty hard --modes casual deathmatch
    python tools\\make_bot_mod.py --cs2 "..." --data release\\cs2_gc --remove

Reads the game files only. Every game mode runs its own config (cfg\\gamemode_<mode>.cfg, which sets
bot_difficulty 1 or 2) and then gamemode_<mode>_server.cfg, a file left for server operators that the game
doesn't ship (see "exec" in gamemodes.txt). The mod adds those files with the difficulty you pick, so nothing
of the game's is replaced; CSForge packs them at the next game start. Bots joining after that use the new
difficulty: from the next match on. The game console shows "CSForge bots: ..." when the file runs.

Two convars, because server.dll picks a joining bot's skill from bot_difficulty (0-3) only when game_online
is set; offline (a match with bots from the menu) it maps custom_bot_difficulty instead: 3 normal, 4 hard,
5 expert, below that easy. The menu's own difficulty dropdown that sets it (GameTypesAPI.SetCustomBotDifficulty,
player_botdifflast_s) is still in mainmenu_play, hidden.
"""
import argparse
import os
import re
import shutil
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_music_mod import cs2_running, kv_string  # noqa: E402
from vpk_extract import read_file, read_tree  # noqa: E402

# bot_difficulty: 0 easy, 1 normal, 2 hard, 3 expert; custom_bot_difficulty is the same + 2 (0 none, 1 dumb)
LEVELS = {"easy": 0, "normal": 1, "hard": 2, "expert": 3}
LEVEL_NAMES = {0: "easy", 1: "normal", 2: "hard", 3: "expert"}


def server_configs(cs2):
    """gamemode_<mode>_server.cfg names the game runs, and the ones that already exist on disk."""
    vpk = os.path.join(cs2, "game", "csgo", "pak01_dir.vpk")
    if not os.path.exists(vpk):
        sys.exit(f"no {vpk}")
    dir_data, data_start, entries = read_tree(vpk)
    if "gamemodes.txt" not in entries:
        sys.exit("no gamemodes.txt in the game files")
    text = read_file(vpk, entries["gamemodes.txt"], dir_data, data_start).decode("utf-8", "replace")
    names = sorted(set(re.findall(r'"exec"\s+"(gamemode_[a-z0-9_]+_server\.cfg)"', text)))
    cfg_dir = os.path.join(cs2, "game", "csgo", "cfg")
    on_disk = {name for name in names if os.path.exists(os.path.join(cfg_dir, name))}
    return names, on_disk


def mode_of(cfg_name):
    return cfg_name[len("gamemode_"):-len("_server.cfg")]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cs2", required=True)
    ap.add_argument("--data", required=True, help="cs2_gc data folder (release\\cs2_gc)")
    ap.add_argument("--difficulty", default="expert",
                    help="easy, normal, hard, expert or 0-3 (default expert)")
    ap.add_argument("--modes", nargs="*", metavar="MODE",
                    help="only these modes (casual, competitive, competitive2v2, deathmatch, armsrace...); default all")
    ap.add_argument("--mod", default="bots", help="mod folder in <data>\\mods (default bots)")
    ap.add_argument("--priority", type=int, default=0)
    ap.add_argument("--remove", action="store_true", help="delete the mod")
    args = ap.parse_args()

    mod_dir = os.path.join(args.data, "mods", args.mod)
    if args.remove:
        if os.path.isdir(mod_dir):
            shutil.rmtree(mod_dir)
            print(f"removed {mod_dir}")
        else:
            print(f"no {mod_dir}")
        return

    level = LEVELS.get(args.difficulty.lower())
    if level is None:
        if not args.difficulty.isdigit() or int(args.difficulty) not in LEVEL_NAMES:
            sys.exit(f"unknown difficulty {args.difficulty}: easy, normal, hard, expert or 0-3")
        level = int(args.difficulty)

    names, on_disk = server_configs(args.cs2)
    if args.modes:
        wanted = {m.lower() for m in args.modes}
        unknown = wanted - {mode_of(n) for n in names}
        if unknown:
            sys.exit(f"no such mode {', '.join(sorted(unknown))}; the game has {', '.join(mode_of(n) for n in names)}")
        names = [n for n in names if mode_of(n) in wanted]

    cfg_dir = os.path.join(mod_dir, "files", "cfg")
    if os.path.isdir(cfg_dir):
        for old in os.listdir(cfg_dir):
            if old.startswith("gamemode_") and old.endswith("_server.cfg"):
                os.remove(os.path.join(cfg_dir, old))
    os.makedirs(cfg_dir, exist_ok=True)

    for name in names:
        if name in on_disk:
            print(f"note: the game folder has its own cfg\\{name}, the mod's one is used instead")
        with open(os.path.join(cfg_dir, name), "w", encoding="ascii", newline="\n") as f:
            f.write("// made by tools\\make_bot_mod.py: CS2 runs this after the mode's own config\n")
            f.write(f"bot_difficulty {level}\n")
            f.write(f"custom_bot_difficulty {level + 2}\n")
            f.write("bot_dont_shoot 0\n")
            f.write(f'echo "CSForge bots: bot_difficulty {level}, custom_bot_difficulty {level + 2} ({mode_of(name)})"\n')

    title = f"Bots: {LEVEL_NAMES[level]}"
    with open(os.path.join(mod_dir, "mod.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("// made by tools\\make_bot_mod.py\n\"mod\"\n{\n")
        f.write(f'\t"name"\t\t"{kv_string(title)}"\n')
        f.write(f'\t"description"\t"bot_difficulty {level} in {", ".join(mode_of(n) for n in names)}"\n')
        f.write(f'\t"enabled"\t"1"\n\t"priority"\t"{args.priority}"\n}}\n')

    print(f"{mod_dir}: bot_difficulty {level} ({LEVEL_NAMES[level]}) for {len(names)} modes: "
          f"{', '.join(mode_of(n) for n in names)}")
    print('CSForge needs "mods" "1" in config.txt; it applies at the next game start')
    if cs2_running():
        print("CS2 is running: restart it to apply")


if __name__ == "__main__":
    main()
