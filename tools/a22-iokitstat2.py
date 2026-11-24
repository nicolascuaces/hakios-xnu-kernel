#!/usr/bin/env python3
"""
a22-iokitstat2.py -- adds the Mali (panfrost) buffers to the emmc_stat report. Needs a22-iokitstat.py applied.

  python3 tools/a22-iokitstat2.py ~/dev/hakios            # review only
  python3 tools/a22-iokitstat2.py ~/dev/hakios apply      # apply (backups *.iokitstat2.orig)

Prints, with every emmc_stat report:  emmc mali: own buffers N (X MB)
(own = created by the programs through create_bo: Mesa textures, vertex buffers, tiler heaps...; the
surfaces of the screen are not counted here, they are in 'emmc iokit: surfaces').
"""
import sys, shutil, os
root = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else ".")
do = len(sys.argv) > 2 and sys.argv[2] == "apply"
x = os.path.join(root, "xnu-src", "xnu-8796.121.2")
mali = os.path.join(x, "iokit/Kernel/RedwoodMali.cpp")
emmc = os.path.join(x, "bsd/dev/redwood_emmc.c")
M = "a22-iokitstat2"
m = open(mali, encoding="utf-8").read(); e = open(emmc, encoding="utf-8").read()
if M in m and M in e:
    print("already applied"); sys.exit(0)
def rep(s, old, new):
    if s.count(old) != 1:
        print("anchor not found exactly once:", old[:50]); sys.exit(1)
    return s.replace(old, new, 1)
a1 = "uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed, gMaliShown;\n"
m = rep(m, a1, a1 + """
/* %s: buffers the programs asked for (not screen surfaces), for emmc_stat */
static uint64_t gMaliOwnBytes;
static uint32_t gMaliOwnCount;
extern "C" void
RedwoodMaliUsage(uint32_t * count, uint64_t * bytes)
{
	*count = gMaliOwnCount;
	*bytes = gMaliOwnBytes;
}
""" % M)
m = rep(m, "\tb->memory = memory;\n\trequest->offset = b->address;\n",
        "\tb->memory = memory;\n\tgMaliOwnBytes += b->size;\n\tgMaliOwnCount++;\n\trequest->offset = b->address;\n")
m = rep(m, "\tredwood_mali_unmap(b->address, b->size);\n\tb->memory->release();\n",
        "\tredwood_mali_unmap(b->address, b->size);\n\tif (b->surface == NULL) {\n\t\tgMaliOwnBytes -= b->size;\n\t\tgMaliOwnCount--;\n\t}\n\tb->memory->release();\n")
e = rep(e, "\t\t    gl >> 20, gk >> 20);\n\t}\n", """\t\t    gl >> 20, gk >> 20);
\t}
\t/* %s */
\t{
\t\textern void RedwoodMaliUsage(uint32_t *count, uint64_t *bytes);
\t\tuint32_t mc;
\t\tuint64_t mb;

\t\tRedwoodMaliUsage(&mc, &mb);
\t\tprintf("emmc mali: own buffers %%u (%%llu MB)\\n", mc, mb >> 20);
\t}
""" % M)
print("ok")
if do:
    for p, s in ((mali, m), (emmc, e)):
        shutil.copy(p, p + ".iokitstat2.orig"); open(p, "w", encoding="utf-8").write(s)
    print("applied")
else:
    print("review only (add 'apply')")
