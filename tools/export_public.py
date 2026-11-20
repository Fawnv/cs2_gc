"""Export the public copy of this repo (for the public GitHub repo) into CS2_GC_Public.

    python tools/export_public.py              what's committed (git archive HEAD)
    python tools/export_public.py --worktree   the files as they are on disk, uncommitted changes too
    python tools/export_public.py --out D:/cs2_gc_public

Leaves out what isn't for a public repo and makes it English: public/<path> replaces <path>
(README.md, music_kits.md, the demo mod's texts; the Russian ones with the development notes stay
here), the tools' Russian texts are translated and the editor's catalogue is made with English
names (needs the CS2 install, --cs2). Local paths become the default Steam one. A .git folder in the output (the public repo itself) is kept, everything else
there is replaced by the new export. At the end the copy is searched for things that must not be
in it (SteamID, email, local paths): if one is found the script says where and fails.
"""
import argparse
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile

MARKER = ".cs2gc_public"  # the output is only ever cleared when it has this (made by this script)

# not ours to publish, or only for this private repo
EXCLUDE = [
    "webe/",  # third-party downloads: csgo_invedit and dricotec archives, dricotec's installer
    "tools/export_public.py",
]

# public/<path> replaces <path> in the public copy: English README.md (the Russian one here has the
# development notes), music_kits.md, the demo mod's texts
OVERLAY = "public/"

# the public copy is in English: the tools' Russian texts, pair by pair (a pair that no longer
# matches is reported, and the Russian check at the end catches what's left)
TRANSLATE = {
    "tools/make_bot_mod.py": [
        ('LEVEL_NAMES = {0: "лёгкие", 1: "обычные", 2: "сложные", 3: "эксперты"}',
         'LEVEL_NAMES = {0: "easy", 1: "normal", 2: "hard", 3: "expert"}'),
        ('title = f"Боты: {LEVEL_NAMES[level]}"', 'title = f"Bots: {LEVEL_NAMES[level]}"'),
    ],
    "tools/make_intro_mod.py": [
        ('"Заставка: тест (op9_main)"', '"Intro: test (op9_main)"'),
        ('"Своя заставка"', '"Custom intro"'),
    ],
    "tools/make_sound_mod.py": [('default="Свои звуки"', 'default="Custom sounds"')],
    "tools/make_music_mod.py": [
        ('"Главное меню"', '"Main menu"'),
        ('"Выбор команды"', '"Team select"'),
        ('"Начало матча"', '"Start of match"'),
        ('"Начало раунда 1"', '"Round start 1"'),
        ('"Начало раунда 2"', '"Round start 2"'),
        ('"Начало раунда 3"', '"Round start 3"'),
        ('"После закупки 1"', '"After the buy time 1"'),
        ('"После закупки 2"', '"After the buy time 2"'),
        ('"После закупки 3"', '"After the buy time 3"'),
        ('"Раунд выигран"', '"Round won"'),
        ('"Раунд проигран"', '"Round lost"'),
        ('"Бомба заложена"', '"Bomb planted"'),
        ('"10 секунд до взрыва"', '"10 s to the explosion"'),
        ('"10 секунд до конца раунда"', '"10 s left in the round"'),
        ('"Камера после смерти"', '"Death cam"'),
        ('"Конец матча"', '"End of match"'),
    ],
    "tools/music_cutter.html": [
        ('<html lang="ru">', '<html lang="en">'),
        ("Нарезка трека", "Track cutter"),
        ('title="Пробел">▶ Трек', 'title="Space">▶ Track'),
        ("Клик по волне ставит начало выбранного отрезка и сразу его играет.",
         "Click the waveform to put the selected slot's start there and hear it."),
        ('<th>Отрезок</th><th>Длина</th><th>Начало, с</th><th class="hide-sm">Где в треке</th>',
         '<th>Slot</th><th>Length</th><th>Start, s</th><th class="hide-sm">Where in the track</th>'),
        ("Загружаю трек…", "Loading the track…"),
        ('id="build">Собрать мод', 'id="build">Build the mod'),
        ("Отрезки длиннее трека повторяют его по кругу. Длина каждого отрезка — как у заменяемого набора.",
         "A slot longer than the track loops it. Each slot is as long as in the kit it replaces."),
        ("· заменяет набор ${CONFIG.kit} · ${slots.length} отрезков", "· replaces the kit ${CONFIG.kit} · ${slots.length} slots"),
        ("toFixed(1)} с</td>", "toFixed(1)} s</td>"),
        ("весь трек", "whole track"),
        (' (по кругу)', ' (looped)'),
        ('"Собираю…"', '"Building…"'),
        ('"Не получилось: окно с make_music_mod.py закрыто? "', '"Failed: is the make_music_mod.py window closed? "'),
        ('"Не удалось загрузить трек: "', '"Couldn\'t load the track: "'),
    ],
}

# Russian on purpose: the game's interface texts for Russian players
ALLOW_RUSSIAN = {"cs2_gc/mod_loader.cpp", "mods/upgrader/localization_russian.txt"}

# the editor's catalogue is made with the Russian names here; the public one gets the English
# names (tools/gen_invedit_catalog.py --lang english), cached in build\public_catalog
CATALOG_FILES = ("editor/catalog.json", "editor/icons.json")
DEFAULT_CS2_HERE = r"B:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive"

DEFAULT_CS2 = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive"
REPLACE = [
    (r"B:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive", DEFAULT_CS2),
    (r"B:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive", DEFAULT_CS2.replace("\\", "\\\\")),
    (r'"B:\\SteamLibrary"', r'"D:\\SteamLibrary"'),  # a libraryfolders.vdf example in the launcher
]

# per file (by its public name): (regex, replacement)
EDITS = {
    ".gitignore": [(r"\n/CS2_GC_Public/", "")],
}

FORBIDDEN = [
    "76561198635263950",  # the SteamID the game ran with here
    "674998222",  # its account id
    "wr4thglavdev",
    "icloud",
    "B:\\",  # the drive the game and the repo are on here
    "B:/",
    r"Users\fight",
    "Users/fight",
]


def committed_files(repo):
    archive = subprocess.check_output(["git", "archive", "--format=tar", "HEAD"], cwd=repo)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        for member in tar.getmembers():
            if member.isfile():
                yield member.name, tar.extractfile(member).read()


def worktree_files(repo):
    # tracked files and new ones git doesn't ignore, as they are on disk
    names = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=repo)
    for name in sorted(set(n for n in names.decode("utf-8").split("\0") if n)):
        path = os.path.join(repo, *name.split("/"))
        if os.path.isfile(path):
            with open(path, "rb") as f:
                yield name, f.read()


def english_catalog(repo, cs2, russian_catalog):
    """editor/catalog.json and icons.json with English names, made again when the catalogue here changed"""
    cache = os.path.join(repo, "build", "public_catalog")
    generator = os.path.join(repo, "tools", "gen_invedit_catalog.py")
    with open(generator, "rb") as f:
        key = hashlib.sha256(russian_catalog + f.read()).hexdigest()
    key_file = os.path.join(cache, "source.sha256")
    fresh = os.path.exists(key_file) and open(key_file).read().strip() == key and all(
        os.path.exists(os.path.join(cache, os.path.basename(n))) for n in CATALOG_FILES)
    if not fresh:
        if not os.path.isfile(os.path.join(cs2, "game", "csgo", "pak01_dir.vpk")):
            sys.exit(f"no CS2 in {cs2} for the English editor catalogue: pass --cs2")
        print("making the English editor catalogue (gen_invedit_catalog.py --lang english)...")
        os.makedirs(cache, exist_ok=True)
        subprocess.check_call([sys.executable, generator, "--cs2", cs2, "--lang", "english", "--out", cache])
        with open(key_file, "w") as f:
            f.write(key + "\n")
    result = {}
    for name in CATALOG_FILES:
        with open(os.path.join(cache, os.path.basename(name)), "rb") as f:
            result[name] = f.read()
    return result


def main():
    repo = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=os.path.join(repo, "CS2_GC_Public"))
    parser.add_argument("--worktree", action="store_true", help="export the files on disk instead of HEAD")
    parser.add_argument("--cs2", default=os.environ.get("CS2_ROOT", DEFAULT_CS2_HERE),
                        help="CS2 install dir, for the English editor catalogue")
    args = parser.parse_args()
    out = os.path.abspath(args.out)

    if os.path.normcase(out) in (os.path.normcase(os.path.abspath(repo)), os.path.normcase(os.path.dirname(os.path.abspath(repo)))):
        sys.exit(f"refusing to export into {out}")
    if os.path.isdir(out) and os.listdir(out) and not os.path.exists(os.path.join(out, MARKER)):
        names = [n for n in os.listdir(out) if n != ".git"]
        if names:
            sys.exit(f"{out} has files this script didn't make ({', '.join(names[:5])}), not clearing it")

    if not args.worktree:
        dirty = subprocess.check_output(["git", "status", "--porcelain"], cwd=repo, text=True)
        if dirty.strip():
            print("note: uncommitted changes are not exported (git archive HEAD, --worktree takes them):")
            print("\n".join("  " + line for line in dirty.splitlines()[:10]))

    files = dict(worktree_files(repo) if args.worktree else committed_files(repo))
    overlay = {name[len(OVERLAY):]: data for name, data in files.items() if name.startswith(OVERLAY)}
    if "README.md" not in overlay:
        sys.exit("public/README.md isn't there (not committed yet? --worktree takes it from disk)")
    files = {name: data for name, data in files.items() if not name.startswith(OVERLAY)}
    files.update(overlay)
    if "editor/catalog.json" in files:
        files.update(english_catalog(repo, args.cs2, files["editor/catalog.json"]))

    # clear the old export, keeping the public repo's .git
    os.makedirs(out, exist_ok=True)
    for name in os.listdir(out):
        if name == ".git":
            continue
        path = os.path.join(out, name)
        if os.path.isdir(path) and not os.path.islink(path):
            shutil.rmtree(path)
        else:
            os.remove(path)

    written = changed = 0
    names = set()
    unmatched = []
    for name, data in sorted(files.items()):
        if any(name == e.rstrip("/") or (e.endswith("/") and name.startswith(e)) for e in EXCLUDE):
            continue
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = None
        if text is not None:
            new = text
            for old, replacement in REPLACE:
                new = new.replace(old, replacement)
            for pattern, replacement in EDITS.get(name, []):
                new = re.sub(pattern, replacement, new)
            for russian, english in TRANSLATE.get(name, []):
                if russian not in new:
                    unmatched.append(f"{name}: {russian}")
                new = new.replace(russian, english)
            if new != text:
                changed += 1
                data = new.encode("utf-8")
        if name.lower().endswith((".cmd", ".bat")):
            # cmd.exe misreads labels and long lines in LF batch files
            data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
        target = os.path.join(out, *name.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as f:
            f.write(data)
        names.add(name)
        written += 1

    # batch files stay CRLF for whoever clones the public repo, whatever their autocrlf
    if ".gitattributes" not in names:
        with open(os.path.join(out, ".gitattributes"), "w", encoding="utf-8", newline="\n") as f:
            f.write("* text=auto\n*.cmd text eol=crlf\n*.bat text eol=crlf\n*.ps1 text eol=crlf\n*.ico binary\n*.exe binary\n")
        written += 1

    with open(os.path.join(out, MARKER), "w", encoding="utf-8") as f:
        f.write("made by tools/export_public.py of the private repo; the next export replaces everything here but .git\n")

    # nothing private left
    found = []
    for root, dirs, walked in os.walk(out):
        dirs[:] = [d for d in dirs if d != ".git"]
        for file in walked:
            path = os.path.join(root, file)
            with open(path, "rb") as f:
                content = f.read()
            relative = os.path.relpath(path, out).replace(os.sep, "/")
            for needle in FORBIDDEN:
                if needle.encode("utf-8") in content:
                    found.append(f"{relative}: {needle}")
            try:
                text = content.decode("utf-8")
            except UnicodeDecodeError:
                continue
            russian = [line.strip() for line in text.splitlines() if re.search("[Ѐ-ӿ]", line)]
            if russian and relative not in ALLOW_RUSSIAN:
                found.append(f"{relative}: Russian text, {len(russian)} lines, e.g. {russian[0][:80]}")

    source = "the files on disk" if args.worktree else "HEAD"
    print(f"exported {written} files from {source} to {out} ({changed} with paths or notes replaced), left out: {', '.join(EXCLUDE)}")
    if unmatched:
        print("translations that didn't match (the tool changed?):")
        print("\n".join("  " + u for u in unmatched))
    if found:
        print("still private or Russian:")
        print("\n".join("  " + f for f in found))
        sys.exit(1)
    print("checked: no SteamID, email, local paths or Russian text")
    if unmatched:
        sys.exit(1)


if __name__ == "__main__":
    main()
