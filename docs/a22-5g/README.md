# HakiOS on the Samsung Galaxy A22 5G (SM-A226BR, MediaTek MT6833)

Work-in-progress port of HakiOS (iOS 16.5 / XNU 8796.121.2) to the Samsung Galaxy A22 5G.
It is built on the `mtk` branch of `oosexgang/hakios-xnu-kernel`, which was written for the
Infinix Hot 20 (MT6768). This repository only contains **the A22 5G changes** (loader, drivers,
patch scripts and build helpers), not a full tree.

> **Language note.** The files were translated from Spanish to English, but some parts may
> still be in Spanish: variable and function names (e.g. `viejo`, `nuevo`, `redes`), some
> commit messages and issue text, a few log lines, and the project guides. Command-line
> options accept both English and the original Spanish spellings (`apply`/`aplicar`,
> `list`/`lista`, ...). If something does not match what you read here, check the Spanish name.

## What works

- Boot chain: `mtkloader` runs in front of the Android kernel in the boot partition and starts XNU.
- Memory: 992 MB at `0x100000000` (a single clean DRAM bank).
- CPU: 7 cores, the A76 cluster at about 2 GHz.
- Storage: eMMC with DMA. Touch: Himax HX83112F. Buttons: volume and power.
- GPU: Mali-G57 MC2 through Mesa Panfrost (GL), about 43-52 fps, peaks near 58-59.
- Wi-Fi (several saved networks), audio and charging bring-up drivers are included; see `tools/`.

Not everything is finished. Expect rough edges, and do not rely on it as a daily phone.

## What is NOT in this repository (and must not be)

- Samsung's kernel source, firmware, device tree, partition images or backups of your phone.
- Touch-controller firmware (`himaxfw.h`) and Wi-Fi/connectivity firmware.
- Your serial number and your Wi-Fi network names or passwords.
- Anything that comes from Apple (iOS images, dyld cache, frameworks).

You have to bring those yourself, from official sources, as described below.

## 1. Get Samsung's kernel source (required)

The A22 constants and several drivers were derived from Samsung's open-source release for this
phone. Download it yourself; it is not redistributed here.

1. Open Samsung's open-source release centre: <https://opensource.samsung.com>.
2. Search for the model **SM-A226BR** (use the model that matches your phone; the A226B family
   has several regional variants).
3. Pick the release whose firmware version matches the one installed on your phone
   (Settings > About phone > Software information > Baseband / Build number).
4. Download the **Kernel** archive (named like `SM-A226BR_..._Kernel.tar.gz`).
   You may need to accept the licence and sign in with a Samsung account.
5. Extract it into its own empty folder and keep it **outside** this repository
   (for example `~/a22-backup/fuente/`). Do not commit it.

## 2. Before you touch the phone

- Make a full backup of the phone, including the partition table, from a custom recovery.
- **Never modify** `preloader`, `lk`, `seccfg`, `efs`, `sec_efs`, `nvdata`, or the eMMC
  `PARTITION_CONFIG`. A wrong `PARTITION_CONFIG` (boot disabled) bricks the device, and recovering it
  needs a special procedure. This port does not need to touch any of them.
- Only run steps you understand. You are responsible for your device.

## 3. Apply the A22 changes

1. Start from a checkout of the `mtk` branch of `oosexgang/hakios-xnu-kernel`.
2. Copy this repository's `mtkloader/`, `tools/` and `userland/` over it, and copy
   `tools/redwood_a22*.c`, `tools/redwood_emmc.c`, `tools/redwood_himax.c` and
   `tools/redwood_mali_a22.h` into `xnu-src/xnu-8796.121.2/bsd/dev/`.
3. Run the patch scripts in `tools/` in review mode first (no arguments after the tree path),
   then with `apply`:

   ```
   python3 tools/a22-constantes.py ~/dev/hakios            # review only
   python3 tools/a22-constantes.py ~/dev/hakios apply      # apply
   ```

   `a22-constantes.py` assumes a fresh tree from the original branch. On a tree that already has
   these changes it may report "MISSING" or "NOT FOUND"; that is a review warning and changes
   nothing.
4. Build the userland image (needs your own iOS 16.5 files, which are not provided):

   ```
   DROP_JOBS="com.apple.chronod" WIFID=1 DRIVERS=softpipe,panfrost GL=1 RC_JOBS=rc.jobs.unlock ./userland/build.sh
   ```

   Compile Mesa with `ninja -j4` and the kernel with `-j2`; with more jobs a laptop can freeze.
5. Test cycles use `tools/xnu-cycle-a22.sh`. Read the script header first: it writes the boot
   partition from Android (root) and checks a hash. The phone serial must come from the
   `HAKIOS_SERIAL` environment variable and is never stored in files.

## Standard boot arguments

```
cpus=7 touchscreen=3 buttons=3 mali=2 mali_start=1 mali_fastmap=2 mali_mhz=955 fb_threads=6 fb_filter=0
```

## Known limits

- RAM is limited to one 992 MB bank; more would need a kernel change to use a second bank.
- Frame rate is limited by CPU work in the framebuffer path, not by the GPU.
- Settings can look "frozen" after the screen auto-dims: set Auto-Lock to Never.

## Credits

- `oosexgang/hakios-xnu-kernel` (`mtk` branch): the base this work builds on.
- `gitlab.com/pessdres/a22x-mainline`: clock and voltage reference for the A76 cluster.
- Mesa / Panfrost for the GPU driver.

This is an unofficial hobby project, not affiliated with or endorsed by Samsung, MediaTek or Apple.
