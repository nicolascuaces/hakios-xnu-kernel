#!/usr/bin/env python3
"""
a22-iokitstat.py -- shows how much of the wired IOKit memory belongs to IOSurface and to the GPU buffers.

  python3 tools/a22-iokitstat.py ~/dev/hakios            # review only
  python3 tools/a22-iokitstat.py ~/dev/hakios apply      # apply (backups *.iokitstat.orig)

Needs a22-emmcstat.py (v3 or v4) already applied. Adds, to every emmc_stat report, the line
  emmc iokit: surfaces N (X MB), gpu buffers live Y MB, gpu kept Z MB
(live surfaces already count the kept pool, if MiniIOSurface keeps them in gSurfaceBytes; the number says it).
"""
import re, sys, shutil, os

root = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else ".")
do = len(sys.argv) > 2 and sys.argv[2] == "apply"
x = os.path.join(root, "xnu-src", "xnu-8796.121.2")
gpu = os.path.join(x, "iokit/Kernel/RedwoodGPU.cpp")
emmc = os.path.join(x, "bsd/dev/redwood_emmc.c")
MARK = "a22-iokitstat"

def rd(p):
    return open(p, encoding="utf-8").read()

g = rd(gpu)
e = rd(emmc)
if MARK in g and MARK in e:
    print("already applied"); sys.exit(0)

pat = re.compile(r"^(\s*)(fBufferBytes|fKeptBytes) ([+-])= (.+);$", re.M)
def mirror(m):
    var = "gGPULive" if m.group(2) == "fBufferBytes" else "gGPUKept"
    return "%s%s %s= %s;\n%s%s %s= %s;" % (m.group(1), m.group(2), m.group(3), m.group(4),
                                            m.group(1), var, m.group(3), m.group(4))
g2, n = pat.subn(mirror, g)
if n != 6:
    print("expected 6 byte counters in RedwoodGPU.cpp, found", n); sys.exit(1)
anchor = "} gGPUStat;\n"
if anchor not in g2:
    print("anchor gGPUStat not found"); sys.exit(1)
g2 = g2.replace(anchor, anchor + """
/* %s: totals over all clients, for the kernel's memory report (emmc_stat) */
static uint64_t gGPULive, gGPUKept;
extern "C" void
RedwoodGPUUsage(uint64_t * live, uint64_t * kept)
{
	*live = gGPULive;
	*kept = gGPUKept;
}
""" % MARK, 1)

old = "\tzone_top_report();\n\tvm_tag_report();\n}"
if old not in e:
    print("es_wired shape not found (apply a22-emmcstat.py first)"); sys.exit(1)
e2 = e.replace(old, """\tzone_top_report();
\tvm_tag_report();
\t/* %s */
\t{
\t\textern void MiniIOSurfaceUsage(uint32_t *count, uint64_t *bytes);
\t\textern void RedwoodGPUUsage(uint64_t *live, uint64_t *kept);
\t\tuint32_t sc;
\t\tuint64_t sb, gl, gk;

\t\tMiniIOSurfaceUsage(&sc, &sb);
\t\tRedwoodGPUUsage(&gl, &gk);
\t\tprintf("emmc iokit: surfaces %%u (%%llu MB), gpu buffers live %%llu MB, gpu kept %%llu MB\\n", sc, sb >> 20,
\t\t    gl >> 20, gk >> 20);
\t}
}""" % MARK, 1)

print("RedwoodGPU.cpp: %d counters mirrored; redwood_emmc.c: line added" % n)
if do:
    for p, s in ((gpu, g2), (emmc, e2)):
        shutil.copy(p, p + ".iokitstat.orig")
        open(p, "w", encoding="utf-8").write(s)
    print("applied")
else:
    print("review only (add 'apply')")
