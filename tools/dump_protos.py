"""Dump .proto files embedded in a game binary (e.g. CS2 client.dll).

Read-only: the binary is only opened for reading.

Generated protobuf C++ code keeps each .proto file as a serialized
FileDescriptorProto. We find those blobs, decode them with a small built-in
protobuf reader (no dependencies) and print them back as .proto source.

Valve's custom options (valveextensions.proto) are dropped and
`option optimize_for = LITE_RUNTIME;` is added, so the output can be compiled
with protobuf-lite like csgo_gc does.

Usage:
    python tools/dump_protos.py <client.dll> <out_dir> [name.proto ...]
"""
import os
import re
import sys

# ---------------------------------------------------------------- wire reader


class ParseError(Exception):
    pass


def read_varint(buf, pos):
    result = 0
    shift = 0
    while True:
        if pos >= len(buf):
            raise ParseError("eof in varint")
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not b & 0x80:
            return result, pos
        shift += 7
        if shift > 63:
            raise ParseError("varint too long")


def iter_fields(buf, start=0, end=None):
    """Yield (field_number, wire_type, value) for a serialized message."""
    pos = start
    end = len(buf) if end is None else end
    while pos < end:
        key, pos = read_varint(buf, pos)
        num, wt = key >> 3, key & 7
        if num == 0:
            raise ParseError("field 0")
        if wt == 0:
            val, pos = read_varint(buf, pos)
        elif wt == 1:
            val = buf[pos:pos + 8]
            pos += 8
        elif wt == 2:
            ln, pos = read_varint(buf, pos)
            if pos + ln > end:
                raise ParseError("length out of range")
            val = buf[pos:pos + ln]
            pos += ln
        elif wt == 5:
            val = buf[pos:pos + 4]
            pos += 4
        else:
            raise ParseError(f"bad wire type {wt}")
        if pos > end:
            raise ParseError("overrun")
        yield num, wt, val


def s(b):
    return b.decode("utf-8")


# ------------------------------------------------------ descriptor decoding

FILE_FIELDS = {1: 2, 2: 2, 3: 2, 4: 2, 5: 2, 6: 2, 7: 2, 8: 2, 9: 2, 10: 0, 11: 0, 12: 2, 14: 0}


def blob_length(buf, start):
    """Length of the FileDescriptorProto starting at `start`, or 0."""
    pos = start
    seen_name = False
    last_ok = start
    while pos < len(buf):
        try:
            key, p = read_varint(buf, pos)
        except ParseError:
            break
        num, wt = key >> 3, key & 7
        if num not in FILE_FIELDS or FILE_FIELDS[num] != wt:
            break
        if wt == 0:
            try:
                _, p = read_varint(buf, p)
            except ParseError:
                break
        else:
            try:
                ln, p = read_varint(buf, p)
            except ParseError:
                break
            if p + ln > len(buf):
                break
            p += ln
        if num == 1:
            if seen_name:
                break  # a second name means the next file started
            seen_name = True
        pos = p
        last_ok = pos
    return last_ok - start


def parse_enum(buf):
    e = {"name": "", "values": []}
    for num, wt, val in iter_fields(buf):
        if num == 1:
            e["name"] = s(val)
        elif num == 2:
            v = {"name": "", "number": 0}
            for n2, _, v2 in iter_fields(val):
                if n2 == 1:
                    v["name"] = s(v2)
                elif n2 == 2:
                    v["number"] = v2 - (1 << 64) if v2 >= (1 << 63) else v2
                    if v["number"] >= (1 << 31):
                        v["number"] -= 1 << 32
            e["values"].append(v)
        elif num == 3:
            for n2, _, v2 in iter_fields(val):
                if n2 == 2:
                    e["allow_alias"] = bool(v2)
    return e


def parse_field(buf):
    f = {"name": "", "number": 0, "label": 1, "type": 0, "type_name": None,
         "default": None, "packed": None, "deprecated": False, "oneof": None,
         "extendee": None}
    for num, wt, val in iter_fields(buf):
        if num == 1:
            f["name"] = s(val)
        elif num == 2:
            f["extendee"] = s(val)
        elif num == 3:
            f["number"] = val
        elif num == 4:
            f["label"] = val
        elif num == 5:
            f["type"] = val
        elif num == 6:
            f["type_name"] = s(val)
        elif num == 7:
            f["default"] = s(val)
        elif num == 8:
            for n2, _, v2 in iter_fields(val):
                if n2 == 2:
                    f["packed"] = bool(v2)
                elif n2 == 3:
                    f["deprecated"] = bool(v2)
        elif num == 9:
            f["oneof"] = val
    return f


def parse_message(buf):
    m = {"name": "", "fields": [], "nested": [], "enums": [], "oneofs": [],
         "extensions": [], "ext_ranges": [], "map_entry": False}
    for num, wt, val in iter_fields(buf):
        if num == 1:
            m["name"] = s(val)
        elif num == 2:
            m["fields"].append(parse_field(val))
        elif num == 3:
            m["nested"].append(parse_message(val))
        elif num == 4:
            m["enums"].append(parse_enum(val))
        elif num == 5:
            r = [0, 0]
            for n2, _, v2 in iter_fields(val):
                if n2 in (1, 2):
                    r[n2 - 1] = v2
            m["ext_ranges"].append(r)
        elif num == 6:
            m["extensions"].append(parse_field(val))
        elif num == 7:
            for n2, _, v2 in iter_fields(val):
                if n2 == 7:
                    m["map_entry"] = bool(v2)
        elif num == 8:
            name = ""
            for n2, _, v2 in iter_fields(val):
                if n2 == 1:
                    name = s(v2)
            m["oneofs"].append(name)
    return m


def parse_file(buf):
    fd = {"name": "", "package": None, "deps": [], "messages": [], "enums": [],
          "extensions": [], "syntax": None}
    for num, wt, val in iter_fields(buf):
        if num == 1:
            fd["name"] = s(val)
        elif num == 2:
            fd["package"] = s(val)
        elif num == 3:
            fd["deps"].append(s(val))
        elif num == 4:
            fd["messages"].append(parse_message(val))
        elif num == 5:
            fd["enums"].append(parse_enum(val))
        elif num == 7:
            fd["extensions"].append(parse_field(val))
        elif num == 12:
            fd["syntax"] = s(val)
    return fd


# ----------------------------------------------------------------- printing

TYPES = {1: "double", 2: "float", 3: "int64", 4: "uint64", 5: "int32",
         6: "fixed64", 7: "fixed32", 8: "bool", 9: "string", 10: "group",
         11: None, 12: "bytes", 13: "uint32", 14: None, 15: "sfixed32",
         16: "sfixed64", 17: "sint32", 18: "sint64"}
LABELS = {1: "optional", 2: "required", 3: "repeated"}
DROP_DEPS = {"google/protobuf/descriptor.proto", "valveextensions.proto"}


def fmt_default(f):
    d = f["default"]
    t = f["type"]
    if t == 9:
        return '"' + d.replace("\\", "\\\\").replace('"', '\\"') + '"'
    if t == 12:
        return '"' + d + '"'  # already C-escaped by protoc
    return d


def print_enum(e, ind, out):
    out.append(f"{ind}enum {e['name']} {{")
    if e.get("allow_alias"):
        out.append(f"{ind}\toption allow_alias = true;")
    for v in e["values"]:
        out.append(f"{ind}\t{v['name']} = {v['number']};")
    out.append(f"{ind}}}")


def field_line(f, in_oneof=False):
    t = TYPES.get(f["type"]) or f["type_name"]
    opts = []
    if f["default"] is not None:
        opts.append(f"default = {fmt_default(f)}")
    if f["packed"]:
        opts.append("packed = true")
    if f["deprecated"]:
        opts.append("deprecated = true")
    o = f" [{', '.join(opts)}]" if opts else ""
    label = "" if in_oneof else LABELS[f["label"]] + " "
    return f"{label}{t} {f['name']} = {f['number']}{o};"


def print_message(m, ind, out):
    out.append(f"{ind}message {m['name']} {{")
    for e in m["enums"]:
        print_enum(e, ind + "\t", out)
    for n in m["nested"]:
        print_message(n, ind + "\t", out)
    emitted_oneofs = set()
    for f in m["fields"]:
        if f["oneof"] is not None:
            idx = f["oneof"]
            if idx in emitted_oneofs:
                continue
            emitted_oneofs.add(idx)
            out.append(f"{ind}\toneof {m['oneofs'][idx]} {{")
            for g in m["fields"]:
                if g["oneof"] == idx:
                    out.append(f"{ind}\t\t{field_line(g, True)}")
            out.append(f"{ind}\t}}")
            continue
        out.append(f"{ind}\t{field_line(f)}")
    for lo, hi in m["ext_ranges"]:
        out.append(f"{ind}\textensions {lo} to {hi - 1};")
    out.append(f"{ind}}}")


def render(fd):
    out = []
    if fd["syntax"]:
        out.append(f'syntax = "{fd["syntax"]}";')
    if fd["package"]:
        out.append(f"package {fd['package']};")
    for d in fd["deps"]:
        if d not in DROP_DEPS:
            out.append(f'import "{d}";')
    out.append("")
    out.append("option optimize_for = LITE_RUNTIME;")
    out.append("")
    for e in fd["enums"]:
        print_enum(e, "", out)
        out.append("")
    for m in fd["messages"]:
        print_message(m, "", out)
        out.append("")
    # non-descriptor extensions
    exts = [x for x in fd["extensions"] if not (x["extendee"] or "").startswith(".google.protobuf.")]
    for x in exts:
        out.append(f"extend {x['extendee']} {{")
        out.append(f"\t{field_line(x)}")
        out.append("}")
        out.append("")
    return "\n".join(out)


# --------------------------------------------------------------------- main

def find_files(data):
    found = {}
    for m in re.finditer(rb"\x0a([\x01-\x7f])([A-Za-z0-9_./]{1,127}?\.proto)", data):
        if m.group(1)[0] != len(m.group(2)):
            continue
        start = m.start()
        ln = blob_length(data, start)
        if ln < 16:
            continue
        blob = data[start:start + ln]
        try:
            fd = parse_file(blob)
        except (ParseError, UnicodeDecodeError):
            continue
        name = fd["name"]
        # keep the biggest copy if a name appears more than once
        if name not in found or len(blob) > len(found[name][0]):
            found[name] = (blob, fd)
    return found


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    src, out_dir = sys.argv[1], sys.argv[2]
    wanted = set(sys.argv[3:])
    with open(src, "rb") as f:
        data = f.read()
    files = find_files(data)
    os.makedirs(out_dir, exist_ok=True)
    for name, (blob, fd) in sorted(files.items()):
        if wanted and name not in wanted:
            continue
        if name in DROP_DEPS:
            continue
        path = os.path.join(out_dir, name)
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(render(fd))
        nmsg = len(fd["messages"])
        print(f"{name}: {nmsg} messages, {len(fd['enums'])} enums, deps={fd['deps']}")


if __name__ == "__main__":
    main()
