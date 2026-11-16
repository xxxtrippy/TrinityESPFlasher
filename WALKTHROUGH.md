# TrinityESPFlasher — complete walkthrough

From a bare ESP32-S3 to a flashed Trinity NAND, start to finish. Written for
someone new to the command line — every terminal step is spelled out.

> **What this tool is:** a standalone NAND flasher and RGH3 image builder for
> the Trinity. It dumps the NAND, re-targets a finished RGH3 image at that
> console on the ESP, and flashes it back, from a touch screen.

> **Read this first — current limits:** the image **Build** is verified (on a
> PC and with `REFIMG.BIN` on the device) against real J-Runner images. The
> **console link** (Detect / Dump / Flash) has not worked on the bench yet; see
> "Known issues" in the README for the two quick tests that narrow it down.

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
git clone https://github.com/xxxtrippy/TrinityESPFlasher.git
cd TrinityESPFlasher
```

If you don't have `git`, download the repo ZIP from the GitHub page
("Code → Download ZIP"), extract it to `Documents`, then
`cd ~\Documents\TrinityESPFlasher-main`.

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

Format the card **FAT32**, then put this on it:

```
/RGH3BASE.BIN            <- a finished RGH3 image (J-Runner's updflash.bin, 17,301,504 bytes)
/cpukey.txt              <- the target console's CPU key, 32 hex characters
/xell/TRINITY_RGH3.ecc   <- only for XEL on a stock console (J-Runner common/xell-images/glitch2)
/xell/xell-gggggg.bin    <- optional, newer XeLL for XEL (J-Runner xeBuild/data)
```

- **`RGH3BASE.BIN`** is the image the builder re-targets: every image the tool
  builds has its dashboard version and patch set. It is Microsoft-derived and
  holds a KeyVault, so it is not shipped with this project and should not be
  shared.
- **`cpukey.txt`**: the first 32 hex characters are used. With it the builder
  checks the dump's KeyVault before writing anything. A stock console's key is
  read with XEL + KEY.
- To try Build without a console link, also copy a NAND read of the console as
  both `NANDDUMP.BIN` and `BACKUP.BIN`. If the image you expect is on the card
  as `REFIMG.BIN`, Build compares against it and shows **Image = REFIMG**.
  Delete `REFIMG.BIN` afterwards: with it present, any other build is refused.

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

**1. FND (detect).** Stops the console's SMC and reads the flash config. The
info line shows `cfg 0x00023010` on a 16 MB Trinity. If nothing answers it
shows `nolink ...` instead; see "Known issues" in the README.

**2. DMP (dump).** Reads the whole 16 MB NAND **twice** and compares the reads.
Green "Dump verified" means both matched; the dump is saved on the card as
`NANDDUMP.BIN` and `BACKUP.BIN`. Red means the wiring is marginal: reseat and
retry.

**3. XEL and KEY (stock consoles only).** XEL writes an RGH3 XeLL image with the
console's own KeyVault; the console then boots XeLL and KEY reads the CPU key
over WiFi into `cpukey.txt`.

**4. BLD (build).** Needs only the SD card. Checks the dump and `RGH3BASE.BIN`,
authenticates the KeyVault with the CPU key, writes `IMAGE.BIN` and self-checks
it. Green "Image verified" unlocks FLSH.

**5. FLSH (flash).** Writes `IMAGE.BIN`, reads it back to confirm, then restarts
the console. It does nothing unless a build passed its self-check.

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
