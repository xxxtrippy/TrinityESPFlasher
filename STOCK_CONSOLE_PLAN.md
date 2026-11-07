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

## SMC handling — OPEN, needs a decision + a second console to validate

A stock console needs the **glitch SMC program** plus **its own config**. From
one console's data we established (see analysis in session):

- The SMC (Nuvoton cipher in `smc.c`) decrypts cleanly; the stock↔glitch diff
  is **442 bytes** — the generic RGH3 glitch patch set (config-free, since both
  are the same console).
- Prebuilt decrypted glitch SMC templates exist (`TRINITY_SMC.bin`,
  `TRINITY_CLEAN.bin`, `TRINITY_CR4.bin`) matching a glitch SMC in ~11.9 K/12 K
  bytes.
- The SMC header's first 4 bytes are a **checksum** over the body (rest of the
  header is identical stock vs glitch) whose algorithm is **not** a standard
  CRC32/Adler/sum — still unidentified.

Two candidate recipes:
- **(A) target stock SMC + 442-byte glitch patch + recomputed checksum** —
  preserves each console's exact config & version. Blocked on the checksum algo.
- **(B) write a prebuilt glitch SMC template** — valid checksum already, but
  uses the template's default config (video region/fan). Low brick risk; video
  region is largely dashboard/AV-cable driven, so usually fine.

**Current builder** uses the base's SMC (which, for the first console, is its
own correct glitch SMC). For true stock support, pick (A) once the checksum is
cracked, else (B) as the safe default. Validating either properly needs a
second console's stock+glitch SMC pair.

## Status

- DONE: console-independent build model, splice builder, ESP read fix, full
  LCD flow (FND→DMP→XEL→KEY→BLD→FLSH), CPU-key capture over WiFi.
- OPEN: universal SMC recipe (checksum or template choice) + validation on a
  second console; optional auto-capture after XEL.
