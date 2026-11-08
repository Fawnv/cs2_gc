"""The intro movie (the COUNTER-STRIKE 2 logo at the game start) from your own video, as a CSForge mod.

    python tools\\make_intro_mod.py --cs2 "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive" --data release\\cs2_gc --test
    python tools\\make_intro_mod.py --cs2 "..." --data release\\cs2_gc --video my_intro.webm
    python tools\\make_intro_mod.py --cs2 "..." --data release\\cs2_gc --video my_intro.mp4 --video720 my_intro_small.webm
    python tools\\make_intro_mod.py --data release\\cs2_gc --remove

Reads the game files only. The game's script intromovie plays file://{resources}/videos/intro.webm
(panorama\\videos\\intro.webm in the game folder: VP9 3840x2160 with an Opus track; intro720p.webm next to it
is VP8 1280x720), and the logo sound comes on its own (sound event UIPanorama.IntroLogo; make_sound_mod.py
replaces it). The movies are loose files in game\\csgo, not in the game's VPK; a file in a VPK wins over a
loose one, so the mod's copy in csforge_dir.vpk should be the one played.

--test puts in a movie of the game itself (the Operation Broken Fang menu background, op9_main), to see in
one start whether the swap works. A video that isn't WebM is converted with ffmpeg (VP9 1080p, VP8 720p).
Enter, Space or Escape skip the intro as before.
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_music_mod import cs2_running, kv_string  # noqa: E402

VIDEOS = ("panorama", "videos")
TEST = {"intro.webm": "op9_main.webm", "intro720p.webm": "op9_main720p.webm"}


def is_webm(path):
    with open(path, "rb") as f:
        head = f.read(64)
    return head[:4] == b"\x1a\x45\xdf\xa3" and b"webm" in head


def convert(src, out, small):
    """Any video to WebM with ffmpeg: VP9 1080p for intro.webm, VP8 720p for intro720p.webm."""
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        sys.exit(f"{src} isn't WebM and there is no ffmpeg in PATH: install ffmpeg, or save the video as WebM "
                 "(VP9 or VP8) yourself, for example with HandBrake")
    video = ["-c:v", "libvpx", "-b:v", "3M", "-vf", "scale=1280:-2"] if small else \
        ["-c:v", "libvpx-vp9", "-b:v", "6M", "-row-mt", "1", "-vf", "scale=1920:-2"]
    cmd = [ffmpeg, "-y", "-loglevel", "error", "-i", src, *video, "-c:a", "libopus", "-b:a", "128k", out]
    print("ffmpeg:", " ".join(cmd[1:]))
    if subprocess.run(cmd).returncode != 0 or not os.path.exists(out):
        sys.exit(f"ffmpeg couldn't convert {src}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cs2", help="CS2 folder (for --test)")
    ap.add_argument("--data", required=True, help="cs2_gc data folder (release\\cs2_gc)")
    ap.add_argument("--video", help="your intro (WebM as is, anything else through ffmpeg)")
    ap.add_argument("--video720", help="the 720p one (default: made from --video)")
    ap.add_argument("--test", action="store_true", help="the game's own op9_main movie instead of yours")
    ap.add_argument("--mod", default="intro", help="mod folder in <data>\\mods (default intro)")
    ap.add_argument("--priority", type=int, default=0)
    ap.add_argument("--remove", action="store_true", help="delete the mod: the game's intro is back")
    args = ap.parse_args()

    mod_dir = os.path.join(args.data, "mods", args.mod)
    if args.remove:
        if os.path.isdir(mod_dir):
            shutil.rmtree(mod_dir)
            print(f"removed {mod_dir}: the game's own intro plays from the next start")
        else:
            print(f"no {mod_dir}")
        return
    if bool(args.test) == bool(args.video):
        sys.exit("give --video <file> or --test")

    out_dir = os.path.join(mod_dir, "files", *VIDEOS)
    os.makedirs(out_dir, exist_ok=True)
    if args.test:
        if not args.cs2:
            sys.exit("--test needs --cs2 (the movie comes from the game folder)")
        game_videos = os.path.join(args.cs2, "game", "csgo", *VIDEOS)
        for name, source in TEST.items():
            src = os.path.join(game_videos, source)
            if not os.path.exists(src):
                sys.exit(f"no {src}")
            shutil.copyfile(src, os.path.join(out_dir, name))
        title, description = "Intro: test (op9_main)", "the intro swapped for the game's op9_main movie"
    else:
        for name, src, small in (("intro.webm", args.video, False), ("intro720p.webm", args.video720 or args.video, True)):
            if not os.path.exists(src):
                sys.exit(f"no {src}")
            out = os.path.join(out_dir, name)
            if is_webm(src):
                shutil.copyfile(src, out)
            else:
                convert(src, out, small)
        title, description = "Custom intro", f"the intro from {os.path.basename(args.video)}"

    with open(os.path.join(mod_dir, "mod.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("// made by tools\\make_intro_mod.py\n\"mod\"\n{\n")
        f.write(f'\t"name"\t\t"{kv_string(title)}"\n')
        f.write(f'\t"description"\t"{kv_string(description)}"\n')
        f.write(f'\t"enabled"\t"1"\n\t"priority"\t"{args.priority}"\n}}\n')

    for name in TEST:
        print(f"{os.path.join(out_dir, name)}: {os.path.getsize(os.path.join(out_dir, name))} bytes")
    print('CSForge needs "mods" "1" in config.txt; it applies at the next game start (without -novid)')
    if cs2_running():
        print("CS2 is running: restart it to apply")


if __name__ == "__main__":
    main()
