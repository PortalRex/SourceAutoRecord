# Dumps the records of a .sarp file (reference reader for the format).
#   python sarpdump.py file.sarp [--types 10,12,20] [--limit N]
import struct, sys, zlib

NAMES = {0x01: "FILE_INFO", 0x02: "DATATABLES", 0x03: "CUSTOMDATA_TABLE", 0x10: "SEGMENT_BEGIN", 0x11: "SEGMENT_END",
         0x12: "SYNC", 0x20: "PACKET", 0x21: "ENTITIES", 0x22: "STRINGTABLE", 0x23: "USERCMD", 0x24: "CONSOLECMD",
         0x25: "CUSTOMDATA", 0x27: "EVENT", 0x7F: "END"}


def var(b, p):
    v = s = 0
    while True:
        c = b[p]; p += 1
        v |= (c & 0x7F) << s; s += 7
        if not c & 0x80:
            return v, p


def string(b, p):
    n, p = var(b, p)
    return b[p:p + n].decode("utf-8", "replace"), p + n


def records(path):
    d = open(path, "rb").read()
    assert d[:8] == b"SARPROOF", "not a proof"
    hsize = struct.unpack_from("<I", d, 12)[0]
    p = hsize
    raw = bytearray()
    while p + 4 <= len(d):
        magic = struct.unpack_from("<I", d, p)[0]
        if magic == 0x444E4553:
            break
        idx, rawsz, stsz, flags, crc = struct.unpack_from("<IIIII", d, p + 4)
        st = d[p + 24:p + 24 + stsz]
        raw += zlib.decompress(st) if flags & 1 else st
        p += 24 + stsz
    q = 0
    while q < len(raw):
        t = raw[q]; q += 1
        n, q = var(raw, q)
        yield t, bytes(raw[q:q + n])
        q += n


def describe(t, b):
    if t == 0x20:
        flags, clock = b[0], struct.unpack_from("<i", b, 1)[0]
        nbits, _ = var(b, 1 + 4 + 152 + 8)
        return "flags=%d clock=%d bits=%d" % (flags, clock, nbits)
    if t in (0x12,):
        return "clock=%d" % struct.unpack_from("<i", b, 0)[0]
    if t == 0x21:
        tick, flags = struct.unpack_from("<iB", b, 0)
        maxe, p = var(b, 5); upd, p = var(b, p); bits, p = var(b, p)
        return "tick=%d delta=%d max=%d updated=%d bits=%d" % (tick, flags & 1, maxe, upd, bits)
    if t == 0x22:
        tick = struct.unpack_from("<i", b, 0)[0]
        name, p = string(b, 4); n, p = var(b, p)
        return "tick=%d table=%s entries=%d" % (tick, name, n)
    if t == 0x10:
        idx, p = var(b, 0); flags = b[p]; p += 1
        m, p = string(b, p)
        return "segment=%d flags=%d map=%s" % (idx, flags, m)
    if t == 0x11:
        clock = struct.unpack_from("<i", b, 0)[0]
        why, _ = string(b, 4)
        return "clock=%d why=%s" % (clock, why)
    if t in (0x23,):
        clock, num = struct.unpack_from("<ii", b, 0)
        return "clock=%d cmd=%d slot=%d" % (clock, num, b[8])
    if t == 0x24:
        clock = struct.unpack_from("<i", b, 0)[0]
        s, _ = string(b, 5)
        return "clock=%d flags=%d %r" % (clock, b[4], s[:80])
    if t == 0x25:
        clock, cb = struct.unpack_from("<ii", b, 0)
        return "clock=%d callback=%d len=%d" % (clock, cb, len(b) - 8)
    if t == 0x27:
        kind, clock = struct.unpack_from("<Bi", b, 0)
        s, _ = string(b, 5)
        return "kind=%d clock=%d %s" % (kind, clock, s)
    return "%d bytes" % len(b)


def main():
    path = sys.argv[1]
    types = None
    limit = 10 ** 9
    if "--types" in sys.argv:
        types = {int(x, 16) for x in sys.argv[sys.argv.index("--types") + 1].split(",")}
    if "--limit" in sys.argv:
        limit = int(sys.argv[sys.argv.index("--limit") + 1])
    n = 0
    for t, b in records(path):
        if types and t not in types:
            continue
        print("%-16s %s" % (NAMES.get(t, hex(t)), describe(t, b)))
        n += 1
        if n >= limit:
            break


if __name__ == "__main__":
    main()
