#!/usr/bin/env python
"""rpatch.py - sparse binary patches for the Snow Leopard files Rosettus modifies.

A patch holds only the bytes this project changed (bridge marks, small code fixes), never the
rest of the Apple file, so the repository can carry patches without carrying Apple's binaries.

Format (RPATCH1):
    b"RPATCH1\\n"
    b"<base sha1> <target sha1> <target size>\\n"
    records: >I offset, >I length, <length bytes>   (big-endian, until end of file)

usage:
    rpatch.py make  BASE TARGET OUT.rpatch     create a patch
    rpatch.py apply BASE PATCH OUT             apply (verifies both sha1s)
    rpatch.py check BASE PATCH                 exit 0 if PATCH applies to BASE
Works with Python 2.6+ and 3.x.
"""
import hashlib, struct, sys

MAGIC = b"RPATCH1\n"
GAP = 16          # merge changed runs closer than this


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def make(base, target):
    recs = []
    n = min(len(base), len(target))
    i = 0
    while i < n:
        if base[i:i + 4096] == target[i:i + 4096]:
            i += 4096
            continue
        if base[i] == target[i]:
            i += 1
            continue
        start = i
        last = i
        while i < n and i - last < GAP:
            if base[i] != target[i]:
                last = i
            i += 1
        end = last + 1
        recs.append((start, target[start:end]))
        i = end
    if len(target) > n:
        recs.append((n, target[n:]))
    out = [MAGIC, ("%s %s %d\n" % (sha1(base), sha1(target), len(target))).encode("ascii")]
    for off, b in recs:
        out.append(struct.pack(">II", off, len(b)))
        out.append(b)
    return b"".join(out), len(recs), sum(len(b) for _, b in recs)


def parse(patch):
    if not patch.startswith(MAGIC):
        raise ValueError("not an RPATCH1 file")
    nl = patch.index(b"\n", len(MAGIC))
    bsha, tsha, tsize = patch[len(MAGIC):nl].decode("ascii").split()
    p = nl + 1
    recs = []
    while p < len(patch):
        off, ln = struct.unpack(">II", patch[p:p + 8])
        recs.append((off, patch[p + 8:p + 8 + ln]))
        p += 8 + ln
    return bsha, tsha, int(tsize), recs


def apply(base, patch):
    bsha, tsha, tsize, recs = parse(patch)
    if sha1(base) != bsha:
        raise ValueError("base file does not match (sha1 %s, patch wants %s)" % (sha1(base), bsha))
    out = bytearray(base[:tsize])
    if len(out) < tsize:
        out.extend(b"\0" * (tsize - len(out)))
    for off, b in recs:
        out[off:off + len(b)] = b
    out = bytes(out)
    if sha1(out) != tsha:
        raise ValueError("patched result does not match the expected sha1")
    return out


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    cmd = argv[1]
    rd = lambda p: open(p, "rb").read()
    if cmd == "make" and len(argv) == 5:
        data, nrec, nbytes = make(rd(argv[2]), rd(argv[3]))
        open(argv[4], "wb").write(data)
        print("%s: %d records, %d bytes" % (argv[4], nrec, nbytes))
    elif cmd == "apply" and len(argv) == 5:
        open(argv[4], "wb").write(apply(rd(argv[2]), rd(argv[3])))
    elif cmd == "check" and len(argv) == 4:
        bsha = parse(rd(argv[3]))[0]
        sys.exit(0 if sha1(rd(argv[2])) == bsha else 1)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
