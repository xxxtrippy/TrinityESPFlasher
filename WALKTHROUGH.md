# TrinityWebFlasher — complete walkthrough

From a bare ESP32-S3 to a flashed Trinity NAND, start to finish. Written for
someone new to the command line — every terminal step is spelled out.

> **What this tool is:** the **NAND read/write** half of an RGH install — the
> part the RP2040 + J-Runner normally does. It does **not** install a glitch
> chip and does **not** obtain your CPU key; your existing glitch/xeLL setup
> does that (see Phase 6). This tool dumps the NAND, (eventually) builds the
> glitch image on-chip, and flashes it back — standalone, from a touch screen.

> **Read this first — current limits:** Detect and Dump work today. The image
> **Build** is wired end-to-end but returns `NOT_VERIFIED` and the **Flash**
> button refuses a built image until its ECC/crypto is verified on-bench
> (`BUILD_ECC_VERIFIED` / `BUILD_CRYPTO_VERIFIED` in `main/build_image.c`). So
> today you can use it as a verified NAND **dumper**; building and flashing a
> bootable RGH image is the remaining work. Everything below describes the full
> intended flow and flags where the gate stops you.

---

## Phase 0 — Parts checklist

- **ESP32-S3-DevKitC-1 N16R8** (16 MB flash / 8 MB PSRAM) + USB-C cable
- **2.4" ILI9341 SPI LCD with XPT2046 resistive touch** (the standard 2.4" red
  board)
- **microSD breakout** (blue "V474"-style, SPI) + a **microSD card**
- Dupont wires, soldering iron (for the J2C1 header on the console)
- A Windows PC to build and flash the firmware (one time)

---

## Phase 1 — Wire it up

All 3.3 V. **Every ground must be common.** Pin names are in `main/pins.h`.

**ESP32-S3 → Xbox 360 Trinity J2C1 header** (isolated SPI2 bus):

| ESP32-S3 | J2C1 signal |
|---|---|
| GPIO13 | SPI_MISO |
| GPIO11 | SPI_MOSI |
| GPIO12 | SPI_CLK |
| GPIO10 | SPI_SS_N |
| GPIO14 | SMC_DBG_EN |
| GPIO21 | SMC_RST_XDK_N |
| GND | GND |

**ESP32-S3 → SD + LCD + touch** (shared SPI3 bus):

| ESP32-S3 | Goes to |
|---|---|
| GPIO5 | SD CLK · LCD SCK · touch CLK |
| GPIO6 | SD MOSI · LCD MOSI · touch DIN |
| GPIO7 | SD MISO · LCD MISO · touch DO |
| GPIO4 | SD CS |
| GPIO15 | LCD CS |
| GPIO16 | LCD DC |
| GPIO17 | LCD RST |
| GPIO18 | LCD LED (backlight) |
| GPIO8 | touch T_CS |
| GPIO9 | touch T_IRQ |
| 3V3 | SD 3v3 · LCD VCC · touch VCC |
| GND | all grounds |

Leave GPIO19/20 (USB) and GPIO35/36/37 (PSRAM) alone.

---

## Phase 2 — Install ESP-IDF (one time, on your PC)

1. Download the **ESP-IDF Windows Installer** from
   <https://dl.espressif.com/dl/esp-idf/> and run it. Accept the defaults; pick
   version **5.3.x**. When it finishes it adds a Start-menu shortcut called
   **"ESP-IDF 5.3 PowerShell"**.
2. Open that **ESP-IDF 5.3 PowerShell** shortcut (not the normal PowerShell —
   this one has the build tools loaded). You'll use it for every command below.

---

## Phase 3 — Get the project

In the ESP-IDF PowerShell:

```
cd ~\Documents
git clone https://github.com/xxxtrippy/TrinityWebFlasher.git
cd TrinityWebFlasher
```

If you don't have `git`, download the repo ZIP from the GitHub page
("Code → Download ZIP"), extract it to `Documents`, then
`cd ~\Documents\TrinityWebFlasher-main`.

---

## Phase 4 — Build the firmware

Still in the ESP-IDF PowerShell, inside the project folder:

```
idf.py set-target esp32s3
idf.py build
```

- `set-target` is a one-time step; it configures the build for the S3.
- `build` compiles everything. First time takes a few minutes. It should end
  with:
  ```
  Project build complete. To flash, run: ...
  ```
  If you see red `error:` lines instead, copy them out — something in the
  environment is off.

---

## Phase 5 — Flash the firmware onto the ESP32

1. Plug the ESP32-S3 into the PC with USB-C. Use the port labeled **COM** /
   **UART** on the board if it has two.
2. Find its COM port: open **Device Manager → Ports (COM & LPT)** — you'll see
   something like `USB Serial Device (COM5)`. Note the number.
3. Flash and open the serial monitor (replace `COM5` with yours):

```
idf.py -p COM5 flash monitor
```

   You should see boot logs ending around:
   ```
   standalone flasher ready
   ```
   and the screen should light up with the **Trinity Flasher** UI.
4. Press **Ctrl+]** to exit the monitor. Firmware is now on the board
   permanently; you only redo this to update it.

If flashing fails to start, hold the board's **BOOT** button, tap **RST**,
release **BOOT**, and run the `idf.py -p COM5 flash` line again.

---

## Phase 6 — Prepare the microSD card

Format the card **FAT32**, then put two things on it:

```
/cpukey.txt            <- your console's CPU key, 32 hex characters
/xebuild/data/         <- the xeBuild data package for your dashboard
   cb.bin  cbb.bin  cd.bin  ce.bin  patches.bin  smc.bin  xell-reloaded.bin
```

- **`cpukey.txt`** — just paste your 32-hex-character CPU key into a plain text
  file. The firmware reads the first 32 hex characters, so extra text/notes are
  fine. **Where the CPU key comes from:** your console must already be glitched;
  boot **xeLL-reloaded**, which prints the CPU key on screen — copy it down.
  This tool can't compute it from a dump (that's the 360's security, not a
  limitation here; J-Runner needs it the same way).
- **`xebuild/data/`** — this is the exact `data` folder your working
  **J-Runner-With-Extras** install already has for your target dashboard. It's
  Microsoft-derived, so it isn't shipped with this project — you supply your own
  copy. If your bootloader files are named differently, rename them to match the
  list above, or edit the ingredient names in `main/build_image.c`.

Insert the card into the SD module.

---

## Phase 7 — First boot & touch check

1. Power the console to **standby** (plug it in; don't turn it on). The ESP can
   run from the console's 3.3 V standby rail, or from its own USB — either way
   grounds must be common.
2. The screen shows **Trinity Flasher** with four buttons: **Detect · Dump ·
   Build · Flash**, a status line, and a progress bar.
3. Tap a button and watch it highlight. If taps land on the wrong button or feel
   offset, open `main/touch.c` and adjust the calibration block
   (`RAW_X_MIN/MAX`, `RAW_Y_MIN/MAX`, `SWAP_XY`, `INVERT_X`, `INVERT_Y`), then
   rebuild/reflash (Phases 4–5). Most 2.4" panels are close to the defaults.

---

## Phase 8 — The flashing workflow (on the screen)

**1. Detect.** Tap **Detect**. It stops the console's SMC and reads the flash
config. The info line should show `cfg 0x########`. A sane value (not all zeros
or all Fs) means your J2C1 wiring is good. If it fails, recheck the six J2C1
wires and the common ground.

**2. Dump.** Tap **Dump**. It reads the whole 16 MB NAND **twice** and compares
the two reads:
- Progress bar runs; status ends **green "dump verified"** if both reads match.
- If it ends **red "reads differ"**, your wiring is marginal — reseat and retry.
   A clean dual-read match is your safety net; never skip it.

The verified dump is held in PSRAM for the next step. *(This dump is also your
backup — the current firmware keeps it in RAM for the build; saving it to the SD
card is a sensible future add if you want an on-card copy before writing.)*

**3. Build.** Tap **Build**. It loads your CPU key from `cpukey.txt`, checks the
`xebuild/data` package, and assembles the glitch image from your dump.
- **Today this ends amber "image built but crypto stages not yet bench-verified"
  and does NOT mark the image flashable.** That's the safety gate — see the note
  at the top. It will stay gated until the ECC/keyvault crypto is verified
  against a known-good J-Runner build.

**4. Flash.** Tap **Flash**. It writes a **verified** image back to the NAND and
reads it back to confirm.
- Until Build produces a verified image, Flash reports **"no verified image"**
  and does nothing — by design, so you can't brick a console on unproven output.

---

## Phase 9 — Safety notes

- **Keep your first dump.** It holds your keyvault and is the only way back if
  anything goes wrong. Before the build pipeline is trusted, treat this tool as
  a **dumper** and build/flash your image in J-Runner from that dump as usual.
- **Never flash a build that didn't pass the gate.** The firmware enforces this;
  don't patch around it until you've done a byte-for-byte parity check against
  J-Runner on the bench.
- **Dump twice, always.** The dual-read compare is there for a reason — a single
  bad bit in a NAND image matters.

---

## Quick command reference

| Do this | Command (ESP-IDF PowerShell, in the project folder) |
|---|---|
| One-time target | `idf.py set-target esp32s3` |
| Build | `idf.py build` |
| Flash + watch logs | `idf.py -p COM5 flash monitor` |
| Just flash | `idf.py -p COM5 flash` |
| Exit the monitor | press `Ctrl+]` |

Replace `COM5` with your board's port (Device Manager → Ports).
