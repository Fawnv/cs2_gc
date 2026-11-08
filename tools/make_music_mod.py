"""Turn an mp3 into a cs2_gc music kit mod (reads the game files only, no Workshop Tools needed).

    python tools\\make_music_mod.py --cs2 "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive" ^
        --data release\\cs2_gc --mp3 musics\\track.mp3 --name "My kit" --add-item

The mod replaces the tracks of an existing music kit (--kit, default theverkkars_01 = The Verkkars,
EZ4ENCE) and renames it, so equipping that kit plays your music under your name. A whole new kit
needs its own compiled sound events plus an entry in soundevents_manifest.vrman_c, that's not done yet.

How the slots get filled: every slot of the kit (main menu, round start, MVP, bomb planted, the 10
second countdowns...) gets as long as the kit's own track. It is cut out of your mp3 starting at
--at <slot>=<seconds> (default 0), and if the mp3 is shorter it repeats, which is right for a looping
track. Cuts are on mp3 frame boundaries (26 ms), the mp3 itself is copied, not re-encoded.

The mp3 has to be MPEG-1 layer 3, 44100 Hz, stereo (what CS2's own music is); anything else (m4a, wav, ogg, a
mono mp3) gets converted with ffmpeg when it is installed. A .vsnd_c is a small
header (taken from the kit's own file, with the sample count, duration and data size patched) followed
by the mp3 frames. Newer kits only have type 5 files (header in a KV3 CTRL block); the game reads type 4
just as well, so those get type 4 files with another kit's header.
"""
import argparse
import http.server
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import webbrowser

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_invedit_catalog import parse_kv, to_dict  # noqa: E402
from vpk_extract import read_file, read_tree  # noqa: E402

BITRATES_V1 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0]
RATES_V1 = [44100, 48000, 32000]
SAMPLES_PER_FRAME = 1152

# vsnd_c (resource type 4) DATA block, see the header of any sounds/music/*.vsnd_c
VSND_FORMAT_MP3 = 1 << 17  # bit 17 of the packed format word: set for mp3, clear for pcm
OFF_SAMPLES, OFF_DURATION, OFF_STREAM_SIZE = 8, 12, 28


def mp3_frames(data, start=0):
    """(offset, size) of every MPEG-1 layer 3 frame, plus the sample rate and channel mode."""
    i = start
    if data[i:i + 3] == b"ID3":
        i += 10 + ((data[i + 6] & 0x7f) << 21 | (data[i + 7] & 0x7f) << 14 | (data[i + 8] & 0x7f) << 7 | (data[i + 9] & 0x7f))
    frames, rates, modes = [], set(), set()
    while i + 4 <= len(data):
        h = struct.unpack(">I", data[i:i + 4])[0]
        version, layer, bitrate, rate, pad, mode = (h >> 19) & 3, (h >> 17) & 3, (h >> 12) & 15, (h >> 10) & 3, (h >> 9) & 1, (h >> 6) & 3
        if (h >> 21) & 0x7ff != 0x7ff or version != 3 or layer != 1 or bitrate in (0, 15) or rate == 3:
            i += 1
            continue
        size = 144 * BITRATES_V1[bitrate] * 1000 // RATES_V1[rate] + pad
        if i + size > len(data):
            break
        frames.append((i, size))
        rates.add(RATES_V1[rate])
        modes.add(mode)
        i += size
    return frames, rates, modes


def is_info_frame(data, offset, size):
    return b"Xing" in data[offset:offset + 64] or b"Info" in data[offset:offset + 64]


def vsnd_blocks(data):
    header_size, _, type_version, block_offset, block_count = struct.unpack_from("<IHHII", data, 0)
    blocks = {}
    for i in range(block_count):
        o = 8 + block_offset + i * 12
        rel, size = struct.unpack_from("<II", data, o + 4)
        blocks[data[o:o + 4].decode("ascii", "replace")] = (o + 4 + rel, size)
    return header_size, type_version, blocks


def mp3_template(vsnd):
    """header bytes and DATA offset of a type 4 mp3 vsnd_c, or None."""
    header_size, type_version, blocks = vsnd_blocks(vsnd)
    if type_version != 4 or "DATA" not in blocks or blocks["DATA"][1] < 32:
        return None
    data_offset = blocks["DATA"][0]
    packed, = struct.unpack_from("<I", vsnd, data_offset)
    stream, = struct.unpack_from("<I", vsnd, data_offset + OFF_STREAM_SIZE)
    if not packed & VSND_FORMAT_MP3 or stream != len(vsnd) - header_size:
        return None
    return bytearray(vsnd[:header_size]), data_offset


def vsnd_seconds(vsnd):
    """length of a game sound: type 4 has it in DATA, type 5 (KV3 in CTRL) is counted from its mp3 frames."""
    header_size, type_version, blocks = vsnd_blocks(vsnd)
    if blocks.get("DATA", (0, 0))[1] >= 32:
        data_offset = blocks["DATA"][0]
        packed, = struct.unpack_from("<I", vsnd, data_offset)
        samples, = struct.unpack_from("<I", vsnd, data_offset + OFF_SAMPLES)
        rate = packed & 0x1FFFF
        return samples / rate if rate else None
    frames, rates, _ = mp3_frames(vsnd, header_size)
    if frames and len(rates) == 1:
        return len(frames) * SAMPLES_PER_FRAME / rates.pop()
    return None


TEMPLATE_DONORS = ["sounds/music/theverkkars_01/roundmvpanthem_01.vsnd_c", "sounds/music/valve_cs2_01/roundmvpanthem_01.vsnd_c"]


def find_mp3_template(vpk, entries, dir_data, data_start):
    """(header, data offset), path: a type 4 mp3 vsnd_c from the game files to build any sound on."""
    music = sorted(e for e in entries if e.startswith("sounds/music/") and e.endswith(".vsnd_c"))
    for path in TEMPLATE_DONORS + music:
        if path in entries:
            template = mp3_template(read_file(vpk, entries[path], dir_data, data_start))
            if template:
                return template, path
    return None, None


def build_vsnd(template, data_offset, sample_rate, audio, sample_count):
    header = bytearray(template)
    packed, = struct.unpack_from("<I", header, data_offset)
    packed = (packed & ~0x1FFFF) | sample_rate
    struct.pack_into("<I", header, data_offset, packed)
    struct.pack_into("<I", header, data_offset + OFF_SAMPLES, sample_count)
    struct.pack_into("<f", header, data_offset + OFF_DURATION, sample_count / sample_rate)
    struct.pack_into("<I", header, data_offset + OFF_STREAM_SIZE, len(audio))
    return bytes(header) + audio


def cut(mp3, frames, start_seconds, seconds, sample_rate):
    """frames from start_seconds on, repeating the track, until `seconds` are covered."""
    per_frame = SAMPLES_PER_FRAME / sample_rate
    first = int(start_seconds / per_frame) % len(frames)
    count = max(1, round(seconds / per_frame))
    out = bytearray()
    for k in range(count):
        offset, size = frames[(first + k) % len(frames)]
        out += mp3[offset:offset + size]
    return bytes(out), count * SAMPLES_PER_FRAME


def convert_audio(path, out, fmt="mp3", channels=2):
    """44.1 kHz mp3 or 16 bit wav: ffmpeg if it is installed, otherwise Windows' own codecs
    (tools\\convert_audio.ps1, m4a/mp3/wav/wma)."""
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg:
        print(f"converting {path} with ffmpeg -> {out}")
        codec = ["-codec:a", "libmp3lame", "-b:a", "192k"] if fmt == "mp3" else ["-codec:a", "pcm_s16le"]
        subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", path, "-vn", "-ar", "44100", "-ac", str(channels),
                        *codec, out], check=True)
    elif os.name == "nt":
        script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "convert_audio.ps1")
        print(f"converting {path} with Windows' codecs -> {out}")
        result = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script,
                                 os.path.abspath(path), os.path.abspath(out), "-Format", fmt, "-Channels", str(channels)],
                                capture_output=True, text=True)
        if result.returncode != 0 or not os.path.exists(out):
            sys.exit(f"{path}: Windows couldn't convert it ({result.stderr.strip() or result.stdout.strip()}).\n"
                     f"Install ffmpeg (winget install Gyan.FFmpeg) or convert the file to {fmt} yourself.")
    else:
        sys.exit(f"{path} needs converting to 44.1 kHz {fmt}: install ffmpeg or convert it yourself.")
    return open(out, "rb").read()


def convert_to_mp3(path):
    return convert_audio(path, os.path.splitext(path)[0] + ".cs2gc.mp3")


def cs2_running():
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq cs2.exe"], capture_output=True, text=True).stdout
    except OSError:
        return False
    return "cs2.exe" in out.lower()


def add_kit_item(inventory_path, music_id, log=print):
    text = open(inventory_path, encoding="utf-8").read()
    if re.search(r'"def_index"\s+"1314"[^}]*"166"\s+"%d"' % music_id, text, re.S):
        log("inventory already has this music kit")
        return
    ids = [int(x) for x in re.findall(r'^\t"(\d+)"\s*$', text, re.M)]
    new_id = max(ids, default=0) + 1
    block = (
        f'\t"{new_id}"\n\t{{\n'
        f'\t\t"inventory"\t\t"{new_id}"\n\t\t"def_index"\t\t"1314"\n\t\t"level"\t\t"1"\n'
        f'\t\t"quality"\t\t"4"\n\t\t"flags"\t\t"0"\n\t\t"origin"\t\t"8"\n\t\t"in_use"\t\t"0"\n'
        f'\t\t"rarity"\t\t"3"\n\t\t"attributes"\n\t\t{{\n\t\t\t"166"\t\t"{music_id}"\n\t\t}}\n\t}}\n'
    )
    end = text.rstrip().rfind("}")
    shutil.copyfile(inventory_path, inventory_path + ".before_music_mod")
    open(inventory_path, "w", encoding="utf-8", newline="\n").write(text[:end] + block + text[end:])
    log(f"added the music kit (music id {music_id}) as item {new_id}, equip it in game "
          "(backup: inventory.txt.before_music_mod)")


def kv_string(text):
    return text.replace("\\", "\\\\").replace('"', '\\"')


SLOT_LABELS = {
    "mainmenu": "Main menu",
    "chooseteam": "Team select",
    "startofmatch": "Start of match",
    "startround_01": "Round start 1",
    "startround_02": "Round start 2",
    "startround_03": "Round start 3",
    "startaction_01": "After the buy time 1",
    "startaction_02": "After the buy time 2",
    "startaction_03": "After the buy time 3",
    "roundmvpanthem_01": "MVP",
    "wonround": "Round won",
    "lostround": "Round lost",
    "bombplanted": "Bomb planted",
    "bombtenseccount": "10 s to the explosion",
    "roundtenseccount": "10 s left in the round",
    "deathcam": "Death cam",
    "endofmatch": "End of match",
}


class Track:
    def __init__(self, path):
        self.path = path
        mp3 = open(path, "rb").read()
        frames, rates, modes = mp3_frames(mp3)
        usable = frames and len(rates) == 1 and 3 not in modes and len(frames) * 400 > len(mp3) / 10
        if not usable or not path.lower().endswith(".mp3"):
            # .m4a/.wav/.ogg/.flac or a mono/odd mp3: make a 44.1 kHz stereo mp3 out of it
            mp3 = convert_to_mp3(path)
            frames, rates, modes = mp3_frames(mp3)
        if not frames:
            sys.exit(f"{path}: no MPEG-1 layer 3 frames found")
        if is_info_frame(mp3, *frames[0]):
            frames = frames[1:]  # Xing/LAME info frame, not audio
        if len(rates) != 1:
            sys.exit(f"{path}: mixed sample rates {rates}")
        if 3 in modes:
            sys.exit(f"{path}: mono, CS2's music is stereo (convert it first)")
        self.mp3, self.frames, self.sample_rate = mp3, frames, rates.pop()
        self.seconds = len(frames) * SAMPLES_PER_FRAME / self.sample_rate
        # the audio as one clean mp3 (no tags, no info frame) for the cutter page to decode
        self.clean = b"".join(mp3[o:o + s] for o, s in frames)


class Kit:
    def __init__(self, cs2, kit):
        self.kit = kit
        vpk = os.path.join(cs2, "game", "csgo", "pak01_dir.vpk")
        dir_data, data_start, entries = read_tree(vpk)
        self.prefix = f"sounds/music/{kit}/"
        paths = sorted(e for e in entries if e.startswith(self.prefix) and e.endswith(".vsnd_c"))
        if not paths:
            sys.exit(f"no music kit '{kit}' (no {self.prefix}*.vsnd_c in the game files)")

        items_game = to_dict(parse_kv(read_file(vpk, entries["scripts/items/items_game.txt"], dir_data, data_start).decode("utf-8", "replace")))["items_game"]
        music = next(((int(k), v) for k, v in items_game["music_definitions"].items() if isinstance(v, dict) and v.get("name") == kit), None)
        if not music:
            sys.exit(f"'{kit}' isn't in music_definitions")
        self.music_id, self.definition = music

        # an mp3 slot of the kit as the header template (some kits have a pcm track or two)
        self.slots, self.template = [], None
        for path in paths:
            vsnd = read_file(vpk, entries[path], dir_data, data_start)
            self.slots.append((path[len(self.prefix):-len(".vsnd_c")], vsnd_seconds(vsnd) or 10.0))
            self.template = self.template or mp3_template(vsnd)
        if not self.template:
            # newer kits are all type 5 (no DATA fields to patch); the game reads type 4 files too
            # (isoxo_01 mixes both), so another kit's type 4 header does
            self.template, donor = find_mp3_template(vpk, entries, dir_data, data_start)
            if not self.template:
                sys.exit(f"no type 4 mp3 track in the game files to use as a header template")
            print(f"{kit} has only type 5 tracks, header template from {donor}")

    def slot_seconds(self, slot, original, track, starts):
        if slot == "mainmenu" and slot not in starts:
            return max(original, track.seconds)  # a whole song in the menu, not cut to the kit's length
        return original


def build_mod(args, track, kit, starts, log=print):
    mod_dir = os.path.join(args.data, "mods", args.mod or f"music_{kit.kit}")
    out_dir = os.path.join(mod_dir, "files", *kit.prefix.strip("/").split("/"))
    os.makedirs(out_dir, exist_ok=True)

    for slot, original in kit.slots:
        seconds = kit.slot_seconds(slot, original, track, starts)
        start = starts.get(slot, 0.0)
        audio, samples = cut(track.mp3, track.frames, start, seconds, track.sample_rate)
        vsnd = build_vsnd(kit.template[0], kit.template[1], track.sample_rate, audio, samples)
        with open(os.path.join(out_dir, slot + ".vsnd_c"), "wb") as f:
            f.write(vsnd)
        repeat = f", track x{seconds / track.seconds:.1f}" if seconds > track.seconds else ""
        log(f"  {slot:20s} {samples / track.sample_rate:6.2f} s from {start:6.2f} s{repeat}")

    loc = kit.definition.get("loc_name", "").lstrip("#")
    desc = kit.definition.get("loc_description", "").lstrip("#")
    with open(os.path.join(mod_dir, "localization.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("// made by tools\\make_music_mod.py: renames the replaced kit\n\"lang\"\n{\n\t\"Tokens\"\n\t{\n")
        f.write(f'\t\t"{loc}"\t\t"{kv_string(args.name)}"\n')
        if args.description and desc:
            f.write(f'\t\t"{desc}"\t\t"{kv_string(args.description)}"\n')
        f.write("\t}\n}\n")
    with open(os.path.join(mod_dir, "mod.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("// made by tools\\make_music_mod.py\n\"mod\"\n{\n")
        f.write(f'\t"name"\t\t"{kv_string(args.name)}"\n\t"description"\t"{kv_string(f"music kit from {os.path.basename(args.mp3)} replacing {kit.kit}")}"\n')
        f.write(f'\t"enabled"\t"1"\n\t"priority"\t"{args.priority}"\n}}\n')
    log(f"wrote {mod_dir} ({len(kit.slots)} tracks), replaces '{kit.kit}' (music id {kit.music_id})")

    if args.add_item:
        if cs2_running():
            log("cs2.exe is running, not touching inventory.txt (close the game and run again)")
        else:
            add_kit_item(os.path.join(args.data, "inventory.txt"), kit.music_id, log)
    return mod_dir


def slots_path(track_path):
    return os.path.splitext(track_path)[0] + ".slots.json"


def load_starts(args):
    starts = {}
    path = args.slots or slots_path(args.mp3)
    if os.path.exists(path):
        saved = json.load(open(path, encoding="utf-8"))
        starts.update({k: float(v) for k, v in saved.get("starts", saved).items()})
        print(f"slot starts from {path}")
    for spec in args.at:
        slot, _, seconds = spec.partition("=")
        starts[slot.strip()] = float(seconds)
    return starts


def serve_cutter(args, track, kit, starts):
    """the cutter page: waveform, a region per slot, click to place, play, build the mod."""
    page = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "music_cutter.html"), encoding="utf-8").read()

    def config():
        return {
            "track": os.path.basename(args.mp3),
            "seconds": track.seconds,
            "kit": kit.kit,
            "name": args.name,
            "slots": [
                {
                    "slot": slot,
                    "label": SLOT_LABELS.get(slot, slot),
                    "length": kit.slot_seconds(slot, original, track, starts),
                    "start": starts.get(slot, 0.0),
                    "whole": slot == "mainmenu" and slot not in starts,
                }
                for slot, original in kit.slots
            ],
        }

    class Handler(http.server.BaseHTTPRequestHandler):
        def send(self, code, body, content_type):
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/":
                html = page.replace("/*CONFIG*/null", json.dumps(config(), ensure_ascii=False))
                self.send(200, html.encode("utf-8"), "text/html; charset=utf-8")
            elif self.path == "/track.mp3":
                self.send(200, track.clean, "audio/mpeg")
            else:
                self.send(404, b"not found", "text/plain")

        def do_POST(self):
            if self.path != "/build":
                return self.send(404, b"not found", "text/plain")
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            starts.clear()
            for slot, seconds in body.get("starts", {}).items():
                starts[slot] = max(0.0, float(seconds))
            json.dump({"track": os.path.basename(args.mp3), "kit": kit.kit, "starts": starts},
                      open(slots_path(args.mp3), "w", encoding="utf-8"), ensure_ascii=False, indent=1)
            lines = []
            try:
                build_mod(args, track, kit, starts, lines.append)
                lines.append(f"saved the starts to {slots_path(args.mp3)}")
                lines.append("done: start the game through run_cs2_gc.cmd and equip the kit")
            except SystemExit as e:
                lines.append(f"error: {e}")
            print("\n".join(lines))
            self.send(200, "\n".join(lines).encode("utf-8"), "text/plain; charset=utf-8")

        def log_message(self, *a):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    url = f"http://127.0.0.1:{server.server_address[1]}/"
    print(f"cutter: {url} (Ctrl+C to stop)")
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cs2", required=True)
    ap.add_argument("--data", required=True, help="cs2_gc data folder (release\\cs2_gc)")
    ap.add_argument("--mp3", required=True, help="the track: mp3, or m4a/wav/wma/ogg/flac (converted with ffmpeg or Windows' codecs)")
    ap.add_argument("--name", required=True, help="the kit's name in game")
    ap.add_argument("--description", default="", help="the kit's description in game")
    ap.add_argument("--kit", default="theverkkars_01", help="existing kit to replace (music_definitions name)")
    ap.add_argument("--mod", help="mod folder name (default music_<kit>)")
    ap.add_argument("--priority", type=int, default=10)
    ap.add_argument("--at", action="append", default=[], metavar="SLOT=SECONDS",
                    help="where in the track a slot starts, e.g. --at roundmvpanthem_01=62.5 (default 0)")
    ap.add_argument("--slots", help="slot starts saved by the cutter (default <track>.slots.json, used if it exists)")
    ap.add_argument("--cutter", action="store_true", help="open the cutter page to place the slots on the waveform")
    ap.add_argument("--no-browser", action="store_true", help="with --cutter: just print the address")
    ap.add_argument("--add-item", action="store_true", help="put the kit into inventory.txt (game must be closed)")
    args = ap.parse_args()

    track = Track(args.mp3)
    print(f"{args.mp3}: {len(track.frames)} frames, {track.sample_rate} Hz stereo, {track.seconds:.2f} s")
    kit = Kit(args.cs2, args.kit)
    starts = load_starts(args)

    if args.cutter:
        serve_cutter(args, track, kit, starts)
    else:
        build_mod(args, track, kit, starts)


if __name__ == "__main__":
    main()
