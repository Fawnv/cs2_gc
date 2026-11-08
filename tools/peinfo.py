"""Read-only PE inspector: exports, imports, and string grep. Never writes to the input file."""
import re
import struct
import sys


def load(path):
    with open(path, "rb") as f:
        return f.read()


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    ddir = opt + (112 if magic == 0x20B else 96)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        name = data[so:so + 8].rstrip(b"\0").decode(errors="replace")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, so + 8)
        secs.append((name, va, vsize, raw, rsize))
        so += 40
    dirs = [struct.unpack_from("<II", data, ddir + 8 * i) for i in range(16)]
    return secs, dirs, magic == 0x20B


def rva2off(secs, rva):
    for name, va, vsize, raw, rsize in secs:
        if va <= rva < va + max(vsize, rsize):
            return rva - va + raw
    return None


def cstr(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode(errors="replace")


def exports(data):
    secs, dirs, _ = sections(data)
    rva, size = dirs[0]
    if not rva:
        return []
    o = rva2off(secs, rva)
    n_names = struct.unpack_from("<I", data, o + 24)[0]
    names_rva = struct.unpack_from("<I", data, o + 32)[0]
    no = rva2off(secs, names_rva)
    out = []
    for i in range(n_names):
        r = struct.unpack_from("<I", data, no + 4 * i)[0]
        out.append(cstr(data, rva2off(secs, r)))
    return out


def imports(data):
    secs, dirs, is64 = sections(data)
    rva, size = dirs[1]
    out = {}
    if not rva:
        return out
    o = rva2off(secs, rva)
    while True:
        oft, ts, fc, name_rva, ft = struct.unpack_from("<IIIII", data, o)
        if not name_rva:
            break
        dll = cstr(data, rva2off(secs, name_rva))
        thunk = rva2off(secs, oft or ft)
        names = []
        while thunk is not None:
            if is64:
                v = struct.unpack_from("<Q", data, thunk)[0]
                ordflag = 1 << 63
                thunk += 8
            else:
                v = struct.unpack_from("<I", data, thunk)[0]
                ordflag = 1 << 31
                thunk += 4
            if not v:
                break
            if v & ordflag:
                names.append(f"#{v & 0xFFFF}")
            else:
                names.append(cstr(data, rva2off(secs, v & 0x7FFFFFFF) + 2))
        out[dll] = names
        o += 20
    return out


def grep(data, pattern, minlen=6):
    rx = re.compile(rb"[\x20-\x7e]{%d,}" % minlen)
    pat = re.compile(pattern, re.I)
    seen = {}
    for m in rx.finditer(data):
        s = m.group().decode()
        if pat.search(s):
            seen[s] = seen.get(s, 0) + 1
    return seen


if __name__ == "__main__":
    cmd, path = sys.argv[1], sys.argv[2]
    data = load(path)
    if cmd == "exports":
        for e in exports(data):
            print(e)
    elif cmd == "imports":
        for dll, names in imports(data).items():
            flt = sys.argv[3] if len(sys.argv) > 3 else None
            if flt and not re.search(flt, dll, re.I):
                continue
            print(f"[{dll}] {len(names)}")
            for n in names:
                print("   ", n)
    elif cmd == "grep":
        for s, c in sorted(grep(data, sys.argv[3]).items()):
            print(c, s)
