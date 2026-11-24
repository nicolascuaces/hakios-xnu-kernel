#!/usr/bin/env python3
"""
a22-ksample.py -- kernel PC sampler (diagnostic only): where does kernel_task spend its time?

  python3 tools/a22-ksample.py ~/dev/hakios            # review only (changes nothing)
  python3 tools/a22-ksample.py ~/dev/hakios apply      # apply (backups *.ksamp.orig)

Boot-args (nothing happens without them):
  ksamp_at=SECONDS   start sampling that many seconds after the kernel starts
  ksamp_dur=SECONDS  sample for this long (default 60), then print the result

Every interrupt (timer, IPI, device) records the PC and LR of the interrupted code. At the end the kernel prints
  ksamp: total N, user U, kernel K
  ksamp: pc  COUNT 0xADDRESS      (top 40 PCs, kernel addresses already unslid)
  ksamp: lr  COUNT 0xADDRESS      (top 40 return addresses)
Symbolize on the Mac with tools/ksamp-sym.sh LOG.
"""
import os
import shutil
import sys

tree = os.path.expanduser(sys.argv[1]) if len(sys.argv) > 1 else sys.exit(__doc__)
do_apply = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
base = os.path.join(tree, "xnu-src", "xnu-8796.121.2", "osfmk", "arm64")
MARK = "ksamp"

SLEH = os.path.join(base, "sleh.c")
UET = os.path.join(base, "user_exception_trace.c")

SLEH_DECL_OLD = "static void sleh_interrupt_handler_prologue(arm_saved_state_t *, unsigned int type);\n"
SLEH_DECL_NEW = SLEH_DECL_OLD + """
/* ksamp: kernel PC sampler (tools/a22-ksample.py), see user_exception_trace.c */
extern volatile int ksamp_on;
extern void ksamp_hit(uint64_t pc, uint64_t lr, int is_user);
"""
SLEH_HOOK_OLD = """	uint64_t pc = is_user ? get_saved_state_pc(state) :
	    VM_KERNEL_UNSLIDE(get_saved_state_pc(state));
"""
SLEH_HOOK_NEW = SLEH_HOOK_OLD + """
	if (ksamp_on) {
		ksamp_hit(pc, is_user ? 0 : VM_KERNEL_UNSLIDE(get_saved_state_lr(state)), is_user);
	}
"""

UET_CODE = r'''

/*
 * ksamp (tools/a22-ksample.py): kernel PC sampler. Boot-args ksamp_at=SECONDS ksamp_dur=SECONDS.
 * Every interrupt records the PC and LR of the interrupted code in a small lock-free table; at the end the
 * top entries are printed (unslid kernel addresses). Diagnostic only: off unless ksamp_at is given.
 */
#define KSAMP_SLOTS 8192
#define KSAMP_TOP   40

volatile int ksamp_on;
static uint64_t ksamp_key[2][KSAMP_SLOTS];
static uint32_t ksamp_cnt[2][KSAMP_SLOTS];
static uint32_t ksamp_total, ksamp_user, ksamp_lost;
static uint32_t ksamp_dur = 60;
static int ksamp_phase;
static thread_call_t ksamp_call;

static void
ksamp_add(int t, uint64_t v)
{
	uint32_t i = (uint32_t)(((v >> 2) * 0x9E3779B97F4A7C15ULL) >> 51);

	for (int n = 0; n < 16; n++, i = (i + 1) & (KSAMP_SLOTS - 1)) {
		uint64_t k = __atomic_load_n(&ksamp_key[t][i], __ATOMIC_RELAXED);
		if (k == 0) {
			uint64_t zero = 0;
			if (__atomic_compare_exchange_n(&ksamp_key[t][i], &zero, v, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
				k = v;
			} else {
				k = zero;
			}
		}
		if (k == v) {
			__atomic_fetch_add(&ksamp_cnt[t][i], 1, __ATOMIC_RELAXED);
			return;
		}
	}
	__atomic_fetch_add(&ksamp_lost, 1, __ATOMIC_RELAXED);
}

void
ksamp_hit(uint64_t pc, uint64_t lr, int is_user)
{
	__atomic_fetch_add(&ksamp_total, 1, __ATOMIC_RELAXED);
	if (is_user) {
		__atomic_fetch_add(&ksamp_user, 1, __ATOMIC_RELAXED);
		return;
	}
	ksamp_add(0, pc);
	if (lr != 0) {
		ksamp_add(1, lr);
	}
}

static void
ksamp_print(void)
{
	static const char *const name[2] = { "pc", "lr" };

	printf("ksamp: total %u, user %u, kernel %u, lost %u\n", ksamp_total, ksamp_user, ksamp_total - ksamp_user, ksamp_lost);
	for (int t = 0; t < 2; t++) {
		for (int r = 0; r < KSAMP_TOP; r++) {
			uint32_t best = 0, bi = 0;
			for (uint32_t i = 0; i < KSAMP_SLOTS; i++) {
				if (ksamp_cnt[t][i] > best) {
					best = ksamp_cnt[t][i];
					bi = i;
				}
			}
			if (best == 0) {
				break;
			}
			printf("ksamp: %s %u 0x%llx\n", name[t], best, (unsigned long long)ksamp_key[t][bi]);
			ksamp_cnt[t][bi] = 0;
		}
	}
	printf("ksamp: end\n");
}

static void
ksamp_fire(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	if (ksamp_phase == 0) {
		ksamp_phase = 1;
		ksamp_on = 1;
		printf("ksamp: sampling for %u s\n", ksamp_dur);
		clock_interval_to_deadline(ksamp_dur, NSEC_PER_SEC, &deadline);
		thread_call_enter_delayed(ksamp_call, deadline);
	} else {
		ksamp_on = 0;
		ksamp_print();
	}
}

static void
ksamp_init(void)
{
	uint32_t at = 0;
	uint64_t deadline;

	if (!PE_parse_boot_argn("ksamp_at", &at, sizeof(at)) || at == 0) {
		return;
	}
	PE_parse_boot_argn("ksamp_dur", &ksamp_dur, sizeof(ksamp_dur));
	ksamp_call = thread_call_allocate(ksamp_fire, NULL);
	clock_interval_to_deadline(at, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(ksamp_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, ksamp_init);
'''


def patch(path, edits, append=None):
    src = open(path).read()
    if MARK + "_on" in src or "ksamp_init" in src:
        print("already applied:", path)
        return
    out = src
    for old, new in edits:
        if out.count(old) != 1:
            print("NOT FOUND (%d matches): %s ..." % (out.count(old), old.strip().splitlines()[0]), path)
            return
        out = out.replace(old, new)
    if append:
        out = out.rstrip("\n") + "\n" + append
    print(("patching: " if do_apply else "would patch: ") + path)
    if do_apply:
        shutil.copy(path, path + ".ksamp.orig")
        open(path, "w").write(out)


patch(SLEH, [(SLEH_DECL_OLD, SLEH_DECL_NEW), (SLEH_HOOK_OLD, SLEH_HOOK_NEW)])
patch(UET, [], UET_CODE)
if not do_apply:
    print("review only; add 'apply' to apply.")
