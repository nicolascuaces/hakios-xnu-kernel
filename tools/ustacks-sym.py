#!/usr/bin/env python3
"""
Names the frames of the "ustacks:" lines of a HakiOS log (boot-arg ustacks=PROCESS ustacks_at=N), using the
iOS 16.5 dyld shared cache in ios165/ (the one tools/dsc/dsc.py reads).

  python3 tools/ustacks-sym.py logs/ios-lsd.log            -> every sampled stack, counted
  python3 tools/ustacks-sym.py logs/ios-lsd.log lsd        -> only the process lsd

A frame inside the shared cache is shown as  Image+offset  and, when the cache has a symbol at or before it,
as  symbol+offset . A frame outside it (the program's own code) is left as it is. The slide of the cache
comes from each "ustacks: NAME[PID] ... shared cache slide 0x..." line.
"""
import bisect
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "dsc"))
import dsc  # noqa: E402

HEAD = re.compile(r"ustacks: (\S+?)\[(\d+)\] .*shared cache slide (0x[0-9a-f]+)")
THREAD = re.compile(r"ustacks:\s+thread (0x[0-9a-f]+): ((?:0x[0-9a-f]+ ?)+)")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    want = sys.argv[2] if len(sys.argv) > 2 else None
    cache = dsc.Cache()
    bases = sorted((a, n) for n, a in cache.images.items())
    addrs = [a for a, _ in bases]
    tables = {}

    def table(name):
        if name not in tables:
            t = {}
            try:
                im = dsc.Image(cache, name)
                for fn in (lambda: {a: s for s, a in im.export_list().items()}, im.local_symbols):
                    try:
                        t.update(fn())
                    except Exception:
                        pass
            except Exception:
                pass
            tables[name] = (t, sorted(t))
        return tables[name]

    def name_of(addr, slide):
        u = addr - slide
        i = bisect.bisect_right(addrs, u) - 1
        if i < 0 or u - addrs[i] > 0x8000000:
            return "%#x" % addr
        base, image = bases[i]
        t, keys = table(image)
        j = bisect.bisect_right(keys, u) - 1
        short = os.path.basename(image)
        if j >= 0 and u - keys[j] < 0x20000:
            return "%s`%s+%#x" % (short, t[keys[j]], u - keys[j])
        return "%s+%#x" % (short, u - base)

    counts, order = {}, []
    proc = None
    slide = 0
    for line in open(sys.argv[1], errors="replace"):
        m = HEAD.search(line)
        if m:
            proc, slide = m.group(1), int(m.group(3), 16)
            continue
        m = THREAD.search(line)
        if m and proc and (want is None or proc == want):
            frames = [int(x, 16) for x in m.group(2).split() if int(x, 16) != 0]
            key = (proc, tuple(frames), slide)
            if key not in counts:
                order.append(key)
            counts[key] = counts.get(key, 0) + 1
    if not counts:
        raise SystemExit("no ustacks lines%s in %s" % (" for " + want if want else "", sys.argv[1]))
    for key in sorted(order, key=lambda k: -counts[k]):
        proc, frames, slide = key
        print("%d x %s" % (counts[key], proc))
        for f in frames:
            print("    ", name_of(f, slide))
        print()


main()
