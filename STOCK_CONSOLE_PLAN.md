# Stock-console support — PC-free / J-Runner-free RGH3 via the ESP only

Goal: the ESP does everything for a **stock** (retail, un-glitched, unknown
CPU-key) Trinity — dump, pull the CPU key, build, flash — with no PC and no
J-Runner. The tool already works for a console whose RGH3 image you have
(see RGH3_BUILD_MODEL.md). This file tracks what stock consoles additionally
need.

## What a stock console changes vs. a known console

1. **Unknown CPU key.** Not stored in NAND (it's in eFuses). You don't need it
   to *build* (the RGH3 chain is console-independent and the KeyVault is copied
   verbatim), but it's wanted for records / DVD-drive work / verification.
2. **Per-console SMC calibration** (video region, fan curve, thermals) that the
   universal base can't carry.

## CPU-key capture (DONE — `JOB_GETKEY` / `xellnet`)

Flow on a stock console:
1. **DMP** — dump the stock NAND (needed for its KeyVault).
2. **XEL** — write an RGH3 *Write-XeLL* image (generic `TRINITY_RGH3.ecc`
   template + this console's KeyVault from the dump; no CPU key needed) and
   flash it. The console glitches and cold-boots into XeLL.
3. **KEY** — `xellnet_fetch()` scans the ESP's WiFi /24 for XeLL's HTTP page,
   scrapes `CPU Key:`, validates it with the CPU-key ECC, and writes
   `/sd/cpukey.txt`. (Requires the 360's Ethernet and the ESP's WiFi on the
   same router.) No screen-reading, no PC — solves the CRT problem.
4. **BLD / FLSH** — build the full RGH3 image and flash it.

`JOB_GETKEY` is exempt from the SMC-link gate because the console is booted into
XeLL (not SMC-stopped) during capture.

## SMC handling (settled by the build model)

Checked against real images (see RGH3_BUILD_MODEL.md): the SMC region at 0x1000
is the RGH3 glitch SMC program, identical for every Trinity built with the same
options, so the base's copy is used as is. The per-console part, the SMC config
(fan and thermal calibration), is a separate block at 0xF7C000 and is taken from
the target console's dump. No SMC patching or checksum is needed.

Still unproven on hardware: booting a re-targeted image on a second console.

## Status

- DONE: console-independent build model, splice builder, ESP read fix, full
  LCD flow (FND→DMP→XEL→KEY→BLD→FLSH), CPU-key capture over WiFi.
- OPEN: the console link on the built hardware (README, Known issues);
  validation on a second console; optional auto-capture after XEL.
