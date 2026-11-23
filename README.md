# HakiOS · MediaTek Dimensity 700 (Samsung Galaxy A22 5G)

**Apple's open-source Darwin kernel (XNU, xnu-8796.121.2 — the iOS 16.5 kernel) on a MediaTek phone**, the way Android is Linux on
phones: a boot loader of its own, drivers of its own and a version of its own. This is a fork of
[oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel); the branch `a22-5g` adds the
**Samsung Galaxy A22 5G (SM-A226BR) — MediaTek MT6833 (Dimensity 700), Mali-G57 MC2 GPU**. On top of it runs an unmodified iOS 16.5
user space: launchd → backboardd → SpringBoard → apps.

The phone loses nothing. `mtkloader` installs into the boot partition ahead of the Android kernel; XNU starts only when asked, and
the phone returns to Android by itself. The iOS system image lives in its own `xnusys` partition and is read from eMMC on demand.
A research bench, not a ROM, and not something to use as a daily phone.

> **Language.** The A22 files were translated from Spanish to English, but some parts may still be in Spanish: variable and function
> names, a few log lines and some older notes. Command-line options accept both spellings (`apply`/`aplicar`, `list`/`lista`).

**Documentation:** [docs/a22-5g/README.md](docs/a22-5g/README.md) (what to download, how to apply the changes and build); the base
documentation of the upstream project (Infinix Hot 20, branch `mtk`) is in [docs/](docs/README.md).

## What works

- **Boot** — `mtkloader` on the MT6833 (a Samsung-specific memory map and framebuffer); seven cores, the A76 cluster raised to about
  2 GHz (`tools/a22-bigclk.py`, `mtkloader/mtka22clk.h`).
- **Memory & storage** — **992 MB** of DRAM for iOS in a single bank at `0x100000000` (the part of the 4 GB that is free once Android
  is not running); the system root is on eMMC (`xnusys`), read with DMA (`tools/redwood_emmc.c`).
- **Graphics** — an in-kernel Mali-G57 driver (`tools/redwood_mali_a22.h`) and Mesa with **panfrost** (OpenGL ES through
  `userland/`, `tools/a22-mesa-v9.py`): about 43–52 fps in normal use, with peaks near 58–59. The GPU is mostly idle; the limit is CPU
  work in the framebuffer path.
- **Input** — Himax HX83112F touchscreen with multitouch (`tools/redwood_himax.c`), and the phone's real power and volume buttons
  (`tools/redwood_a22keys.c`).
- **Wi-Fi** — CONNSYS brought up from firmware you take from your own phone; the phone can join one of several saved networks
  (`tools/redwood_a22wifi.c`, `tools/a22-redes.py`).
- **Audio and charging** — bring-up drivers for the audio path and the MT6360 charger are included (`tools/redwood_a22audio.c`,
  `tools/redwood_a22charge.c`); they are experimental.

**Not yet, or not a goal:** cellular, Bluetooth, sensors, camera, vibration, a second DRAM bank (more than 992 MB), 90 Hz, and a
fully writable system volume (`/private/var` is a RAM file system rebuilt at every boot). Apple's online services — activation, Apple
ID, iCloud, the App Store — are **not supported and not a goal**: this hardware has no Secure Enclave, and nothing Apple's servers
verify is forged.

## What you bring, and what is never here

Nothing of Apple's, nothing of Samsung's and no vendor firmware is in this repository. You fetch these yourself, from official
sources:

- the iOS 16.5 files, from an IPSW you download (`ios165/fetch.sh`);
- Samsung's open-source **kernel** for your SM-A226BR, from <https://opensource.samsung.com> (step by step in
  [docs/a22-5g/README.md](docs/a22-5g/README.md));
- the touch and Wi-Fi/connectivity firmware, from your own phone.

No iOS file, no phone firmware (`himaxfw.h` is git-ignored), no serial number and no Wi-Fi name or password is checked in.

## Safety

The port never needs to modify `preloader`, `lk`, `seccfg`, `efs`, `sec_efs`, `nvdata` or the eMMC `PARTITION_CONFIG`; do not touch them.
Back up the phone first. You are responsible for your own device.

## Building

The kernel is built from an external XNU source tree plus `xnu/redwood.patch`; the A22 changes are applied with the scripts in
`tools/` (`a22-*.py`, review mode first, then `apply`); user space is built with `userland/build.sh`; test sessions are run with
`tools/xnu-cycle-a22.sh`. Every command in order: [docs/a22-5g/README.md](docs/a22-5g/README.md). The general method is the one in
[docs/from-a-clone.md](docs/from-a-clone.md) and [docs/mtk-porting.md](docs/mtk-porting.md).

## Credits

- [oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel), branch `mtk` (Infinix Hot 20): the base of this work.
- [a22x-mainline](https://gitlab.com/pessdres/a22x-mainline): clock and voltage reference for the A76 cluster.
- Mesa / panfrost for the GPU driver.

## License and legal

The XNU patch and the files derived from it are under the Apple Public Source License 2.0; everything else is under MIT, as the file
headers say. None of this is legal advice.

HakiOS is an independent research project. It is **not affiliated with or endorsed by Apple, Samsung or MediaTek**; "Darwin",
"iPhone", "XNU", "iOS", "Apple", "Samsung", "Galaxy" and "MediaTek" are named only to describe what the code works with. If you
believe a file here infringes anyone's rights, open an issue and it will be removed.
