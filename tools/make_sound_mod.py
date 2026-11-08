"""Replace any CS2 sound (shots, footsteps, bomb, ui, voice lines...) with your own audio file, as a CSForge mod.

    python tools\\make_sound_mod.py --cs2 "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive" --list ak47
    python tools\\make_sound_mod.py --cs2 "..." --data release\\cs2_gc --audio shot.mp3 --sound "sounds/weapons/ak47/ak47_0*"
    python tools\\make_sound_mod.py --cs2 "..." --data release\\cs2_gc --remove "sounds/weapons/ak47/*"

Reads the game files only. Every matched sound gets a .vsnd_c in <data>\\mods\\<mod>\\files\\ (default mod
"sounds"), which CSForge packs at the next game start. Running it again adds to the same mod.

The game's own effects are 16 bit PCM .vsnd_c (type 4: a small header, then the samples; newer ones are type 5
with a KV3 header, the game reads both). The new file takes the header of a type 4 PCM sound from the game,
with the sample rate, channel count, sample count, duration and data size patched. Mono by default for sounds
the game plays in 3D (a type 4 original says how many channels it has, a type 5 one is assumed mono),
--stereo / --mono override that. The audio is decoded to 44.1 kHz 16 bit wav with ffmpeg or Windows' codecs.
"""
import argparse
import array
import fnmatch
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_music_mod import OFF_DURATION, OFF_SAMPLES, OFF_STREAM_SIZE, convert_audio, cs2_running, kv_string, vsnd_blocks, vsnd_seconds  # noqa: E402
from vpk_extract import read_file, read_tree  # noqa: E402

RATE = 44100
CHANNELS_SHIFT = 24  # packed format word: bits 0-16 sample rate, 17-23 format (0 pcm16, 1 mp3), 24-25 channels
FORMAT_MASK = 0x7F << 17
PCM_TEMPLATES = ["sounds/player/footsteps/bass_01.vsnd_c", "sounds/weapons/c4/c4_click.vsnd_c"]


class Game:
    def __init__(self, cs2):
        self.vpk = os.path.join(cs2, "game", "csgo", "pak01_dir.vpk")
        if not os.path.exists(self.vpk):
            sys.exit(f"no {self.vpk}")
        self.dir_data, self.data_start, self.entries = read_tree(self.vpk)
        self.sounds = sorted(p for p in self.entries if p.endswith(".vsnd_c"))

    def read(self, path):
        return read_file(self.vpk, self.entries[path], self.dir_data, self.data_start)

    def info(self, path):
        """(type, format, channels or None, seconds or None) of a game sound."""
        vsnd = self.read(path)
        _, type_version, blocks = vsnd_blocks(vsnd)
        if blocks.get("DATA", (0, 0))[1] >= 32:
            packed, = struct.unpack_from("<I", vsnd, blocks["DATA"][0])
            fmt = {0: "pcm16", 1: "mp3"}.get((packed & FORMAT_MASK) >> 17, "other")
            return type_version, fmt, (packed >> CHANNELS_SHIFT) & 3, vsnd_seconds(vsnd)
        ctrl = vsnd[blocks["CTRL"][0]:sum(blocks["CTRL"])] if "CTRL" in blocks else b""
        fmt = "mp3" if b"MP3" in ctrl else "pcm16" if b"PCM16" in ctrl else "other"
        return type_version, fmt, None, vsnd_seconds(vsnd) if fmt == "mp3" else None

    def pcm_template(self):
        """header bytes and DATA offset of a type 4 pcm16 vsnd_c."""
        for path in PCM_TEMPLATES + self.sounds:
            vsnd = self.read(path)
            header_size, type_version, blocks = vsnd_blocks(vsnd)
            if type_version != 4 or blocks.get("DATA", (0, 0))[1] < 32:
                continue
            data_offset = blocks["DATA"][0]
            packed, = struct.unpack_from("<I", vsnd, data_offset)
            stream, = struct.unpack_from("<I", vsnd, data_offset + OFF_STREAM_SIZE)
            if packed & FORMAT_MASK == 0 and stream == len(vsnd) - header_size:
                return bytearray(vsnd[:header_size]), data_offset
        sys.exit("no type 4 pcm sound in the game files to use as a header template")

    def match(self, patterns):
        found = []
        for pattern in patterns:
            pattern = pattern.replace("\\", "/").lower()
            if not pattern.endswith(".vsnd_c"):
                pattern += ".vsnd_c"
            hits = [p for p in self.sounds if fnmatch.fnmatchcase(p, pattern)]
            if not hits:
                sys.exit(f"no game sound matches {pattern} (find them with --list)")
            found += [p for p in hits if p not in found]
        return found


def read_wav(data):
    """(channels, rate, pcm16 samples) of a 16 bit pcm wav."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit("not a wav file")
    pos, fmt, samples = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", body)
            if tag not in (1, 0xFFFE) or bits != 16:
                return None
            fmt = (channels, rate)
        elif cid == b"data":
            samples = array.array("h", body[:len(body) // 2 * 2])
        pos += 8 + size + (size & 1)
    return (*fmt, samples) if fmt and samples is not None else None


def decode(path, workdir):
    """(channels, rate, interleaved pcm16) of any audio file: a 16 bit wav as is, the rest through a wav."""
    wav = read_wav(open(path, "rb").read()) if path.lower().endswith(".wav") else None
    if not wav:
        out = os.path.join(workdir, os.path.splitext(os.path.basename(path))[0] + ".wav")
        wav = read_wav(convert_audio(path, out, "wav", 2))
        if not wav:
            sys.exit(f"{path}: the converted wav isn't 16 bit pcm")
    return wav


def to_pcm(decoded, channels, start, length, gain_db):
    """cut [start, start + length) seconds, mix to `channels`, resample to 44.1 kHz, apply the gain."""
    src_channels, rate, samples = decoded
    first = int(start * rate)
    count = len(samples) // src_channels - first if length is None else int(length * rate)
    count = max(0, min(count, len(samples) // src_channels - first))
    if count == 0:
        sys.exit(f"nothing left after --start {start} --length {length}")
    frames = [samples[(first + i) * src_channels:(first + i + 1) * src_channels] for i in range(count)]
    if channels == 1:
        frames = [(sum(f) // len(f),) for f in frames]
    else:
        frames = [(f[0], f[1] if len(f) > 1 else f[0]) for f in frames]
    if rate != RATE:  # linear interpolation, plenty for effects
        n = max(1, int(count * RATE / rate))
        resampled = []
        for i in range(n):
            pos = i * rate / RATE
            a = frames[min(int(pos), count - 1)]
            b = frames[min(int(pos) + 1, count - 1)]
            t = pos - int(pos)
            resampled.append(tuple(int(x + (y - x) * t) for x, y in zip(a, b)))
        frames = resampled
    factor = 10 ** (gain_db / 20)
    return array.array("h", (max(-32768, min(32767, int(s * factor))) for f in frames for s in f))


def build_pcm_vsnd(template, data_offset, channels, samples):
    header = bytearray(template)
    if sys.byteorder != "little":
        samples = array.array("h", samples)
        samples.byteswap()
    audio = samples.tobytes()
    frames = len(samples) // channels
    packed, = struct.unpack_from("<I", header, data_offset)
    packed = (packed & ~(0x1FFFF | FORMAT_MASK | (3 << CHANNELS_SHIFT))) | RATE | (channels << CHANNELS_SHIFT)
    struct.pack_into("<I", header, data_offset, packed)
    struct.pack_into("<i", header, data_offset + 4, -1)  # no loop
    struct.pack_into("<I", header, data_offset + OFF_SAMPLES, frames)
    struct.pack_into("<f", header, data_offset + OFF_DURATION, frames / RATE)
    struct.pack_into("<I", header, data_offset + OFF_STREAM_SIZE, len(audio))
    return bytes(header) + audio


def write_mod_txt(mod_dir, name, priority):
    path = os.path.join(mod_dir, "mod.txt")
    if os.path.exists(path):
        return
    os.makedirs(mod_dir, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("// made by tools\\make_sound_mod.py\n\"mod\"\n{\n")
        f.write(f'\t"name"\t\t"{kv_string(name)}"\n\t"description"\t"replaced game sounds"\n')
        f.write(f'\t"enabled"\t"1"\n\t"priority"\t"{priority}"\n}}\n')


def list_sounds(game, terms, limit):
    hits = [p for p in game.sounds if all(t.lower() in p for t in terms)]
    for path in hits[:limit]:
        type_version, fmt, channels, seconds = game.info(path)
        ch = {1: "mono", 2: "stereo"}.get(channels, "?")
        length = f"{seconds:7.2f} s" if seconds else "      ? "
        print(f"{path:70s} type {type_version} {fmt:5s} {ch:6s} {length}")
    more = f" (showing {limit}, --limit 0 for all)" if limit and len(hits) > limit else ""
    print(f"{len(hits)} sounds{more}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cs2", required=True)
    ap.add_argument("--data", help="cs2_gc data folder (release\\cs2_gc)")
    ap.add_argument("--list", nargs="*", metavar="WORD", help="print the game sounds whose path has all these words")
    ap.add_argument("--limit", type=int, default=200, help="with --list: how many to show (0 = all)")
    ap.add_argument("--sound", action="append", default=[], metavar="PATH",
                    help="game sound to replace, wildcards ok (sounds/weapons/ak47/ak47_0*), repeatable")
    ap.add_argument("--audio", help="your file: mp3, wav, m4a, wma, ogg, flac...")
    ap.add_argument("--remove", action="append", default=[], metavar="PATH", help="drop these replacements from the mod")
    ap.add_argument("--start", type=float, default=0.0, help="seconds to skip at the start of your file")
    ap.add_argument("--length", type=float, help="seconds to keep (default: to the end)")
    ap.add_argument("--gain", type=float, default=0.0, help="volume change in dB, e.g. -6 or 3")
    channels = ap.add_mutually_exclusive_group()
    channels.add_argument("--mono", action="store_true", help="always mono")
    channels.add_argument("--stereo", action="store_true", help="always stereo")
    ap.add_argument("--mod", default="sounds", help="mod folder in <data>\\mods (default sounds)")
    ap.add_argument("--name", default="Custom sounds", help="mod name in mod.txt (for a new mod)")
    ap.add_argument("--priority", type=int, default=20)
    args = ap.parse_args()

    game = Game(args.cs2)
    if args.list is not None:
        return list_sounds(game, args.list, args.limit)
    if not args.data:
        sys.exit("--data is needed to write the mod")
    mod_dir = os.path.join(args.data, "mods", args.mod)
    files_dir = os.path.join(mod_dir, "files")

    for pattern in args.remove:
        pattern = pattern.replace("\\", "/").lower()
        pattern += "" if pattern.endswith(".vsnd_c") else ".vsnd_c"
        removed = 0
        for root, _, names in os.walk(files_dir):
            for name in names:
                rel = os.path.relpath(os.path.join(root, name), files_dir).replace("\\", "/").lower()
                if fnmatch.fnmatchcase(rel, pattern):
                    os.remove(os.path.join(root, name))
                    removed += 1
        print(f"removed {removed} file(s) matching {pattern} from {mod_dir}")
    if args.remove and not args.sound:
        return

    if not args.sound or not args.audio:
        sys.exit("give --sound (what to replace) and --audio (your file), or --list / --remove")
    targets = game.match(args.sound)
    template, data_offset = game.pcm_template()

    with tempfile.TemporaryDirectory() as workdir:
        decoded = decode(args.audio, workdir)
    audio = {}
    for path in targets:
        _, fmt, original_channels, seconds = game.info(path)
        want = 2 if args.stereo else 1 if args.mono else (original_channels or 1)
        if want not in audio:
            audio[want] = to_pcm(decoded, want, args.start, args.length, args.gain)
        vsnd = build_pcm_vsnd(template, data_offset, want, audio[want])
        out = os.path.join(files_dir, *path.split("/"))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(vsnd)
        was = f"{seconds:.2f} s" if seconds else "? s"
        print(f"  {path}: {len(audio[want]) / want / RATE:.2f} s {'stereo' if want == 2 else 'mono'} (was {was} {fmt})")

    write_mod_txt(mod_dir, args.name, args.priority)
    print(f"wrote {len(targets)} sound(s) to {mod_dir}")
    if cs2_running():
        print("cs2 is running: the mod gets packed at the next game start")
    print("start the game through run_cs2_gc.cmd (config \"mods\" \"1\")")


if __name__ == "__main__":
    main()
