"""Derive interface vtable slot indices from steam_api64.dll flat-API thunks (read-only).

Each SteamAPI_ISteamX_Method export is a tiny thunk that loads the vtable from
the first argument and jumps/calls through a fixed slot. We decode a few common
x64 encodings to recover that slot.
"""
import re
import struct
import sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
from peinfo import load, sections, rva2off, cstr  # noqa: E402


def export_map(data):
    secs, dirs, _ = sections(data)
    rva, size = dirs[0]
    o = rva2off(secs, rva)
    n_funcs, n_names, funcs_rva, names_rva, ords_rva = struct.unpack_from("<IIIII", data, o + 20)
    fo = rva2off(secs, funcs_rva)
    no = rva2off(secs, names_rva)
    oo = rva2off(secs, ords_rva)
    out = {}
    for i in range(n_names):
        name = cstr(data, rva2off(secs, struct.unpack_from("<I", data, no + 4 * i)[0]))
        ordinal = struct.unpack_from("<H", data, oo + 2 * i)[0]
        frva = struct.unpack_from("<I", data, fo + 4 * ordinal)[0]
        out[name] = rva2off(secs, frva)
    return out


def decode_slot(code):
    """Return vtable byte offset used by the thunk, or None."""
    # Follow a leading jmp rel32 (incremental-link style thunks)
    i = 0
    # find "mov rax,[rcx]" = 48 8B 01, possibly after some prologue
    m = code.find(b"\x48\x8b\x01", 0, 48)
    if m < 0:
        return None
    i = m + 3
    # look for jmp/call [rax+disp]
    for j in range(i, min(i + 40, len(code) - 6)):
        b = code[j:j + 7]
        pfx = 1 if b[0] == 0x48 else 0
        op, modrm = b[pfx], b[pfx + 1]
        if op != 0xFF:
            continue
        reg = (modrm >> 3) & 7
        if reg not in (2, 4):  # call / jmp
            continue
        mod, rm = modrm >> 6, modrm & 7
        if rm != 0:  # [rax...]
            continue
        if mod == 0:
            return 0
        if mod == 1:
            return struct.unpack_from("<b", b, pfx + 2)[0]
        if mod == 2:
            return struct.unpack_from("<i", b, pfx + 2)[0]
    return None


if __name__ == "__main__":
    path, iface = sys.argv[1], sys.argv[2]
    data = load(path)
    exps = export_map(data)
    prefix = f"SteamAPI_{iface}_"
    rows = []
    for name, off in exps.items():
        if not name.startswith(prefix):
            continue
        slot = decode_slot(data[off:off + 96])
        rows.append((slot // 8 if slot is not None else 999, name[len(prefix):]))
    for idx, name in sorted(rows):
        print(f"{idx:3d} {name}")
