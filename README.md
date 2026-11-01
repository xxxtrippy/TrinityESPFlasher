# TrinityESPFlasher

A **fully standalone Xbox 360 Slim (Trinity) NAND flasher** on an ESP32-S3 with
a 2.4" touch screen. **No PC, no phone, no WiFi** — dump the NAND, build the
RGH/glitch image on the ESP32 itself (from the xeBuild data package on a microSD
card), and flash it back, all driven from on-screen buttons.

> **Status: foundation complete, build engine staged behind safety gates.**
> Read [Project status](#project-status) before you flash anything. The read /
> dump path is the solid part; the image-*build* path is wired end-to-end but
> its crypto/ECC stages are deliberately gated off until verified on real
> hardware, so the firmware will **not** flash an unverified image.

---

## Why ESP32-S3 (not a classic WROOM)

- **8 MB PSRAM (N16R8)** — a full Trinity dump is ~16.9 MiB raw. The two-pass
  verify and the image build both need that headroom. A plain WROOM's ~520 KB
  SRAM can't hold a dump at all.
- **16 MB flash** — room for the firmware + bundled UI.
- **Spare SPI buses + GPIO** — three SPI peripherals (Xbox SB, SD card, LCD +
  touch) need the room the S3 provides.

**Board: ESP32-S3-DevKitC-1 N16R8** (16 MB flash / 8 MB octal PSRAM).

---

## Wiring

All logic is **3.3 V on both sides — no level shifting**. A **common ground is
mandatory.** These are the same J2C1 header points the RP2040 PicoFlasher uses.

### Xbox 360 Trinity — J2C1 header

| ESP32-S3 GPIO | Xbox J2C1 signal | Notes |
|---|---|---|
| GPIO13 | SPI_MISO | SB → ESP data |
| GPIO11 | SPI_MOSI | ESP → SB data |
| GPIO12 | SPI_CLK  | clock |
| GPIO10 | SPI_SS_N | chip select |
| GPIO14 | SMC_DBG_EN | SMC debug enable |
| GPIO21 | SMC_RST_XDK_N | SMC reset |
| GND | GND | **required** |

### microSD card + 2.4" ILI9341 touch LCD (shared SPI bus)

The SD card, the display, and its XPT2046 touch controller all share one SPI
bus (SPI3), each with its own chip-select. The Xbox bus (SPI2) stays isolated.
The blue "V474"-style SD breakout is **SPI-only** (`3v3 CS MOSI CLK MISO GND`)
and 3.3V-native, like the LCD.

| ESP32-S3 GPIO | Goes to | Shared? |
|---|---|---|
| GPIO5 | SD CLK · LCD SCK · touch CLK | shared clock |
| GPIO6 | SD MOSI · LCD MOSI · touch DIN | shared data-out |
| GPIO7 | SD MISO · LCD MISO · touch DO | shared data-in |
| GPIO4 | SD CS | |
| GPIO15 | LCD CS | |
| GPIO16 | LCD DC | |
| GPIO17 | LCD RST | |
| GPIO18 | LCD LED (backlight) | |
| GPIO8 | touch CS (T_CS) | |
| GPIO9 | touch IRQ (T_IRQ) | |
| 3V3 | SD 3v3 · LCD VCC · touch VCC | |
| GND | all grounds | **required** |

> Keep clear: GPIO19/20 (native USB) and GPIO35/36/37 (PSRAM) are off-limits.
> Touch calibration constants live in [`main/touch.c`](main/touch.c) — tweak
> them if taps land off-target on your panel.

> **Do not use GPIO35/36/37** — the N16R8's octal PSRAM uses them.
> GPIO19/20 are reserved for the future J-Runner-USB mode.

Pins are defined in [`main/pins.h`](main/pins.h) — change them there if your board
differs.

---

## What you need

1. **ESP32-S3-DevKitC-1 N16R8**, a 2.4" ILI9341 + XPT2046 touch LCD, and the
   SD module — wired as above.
2. **A microSD card** holding the **xeBuild data package** and your CPU key
   (see below).
3. **Your console's CPU key** — on a *first* RGH install this is read off the
   glitched console by xeLL. It cannot be computed from a dump (that's the
   360's security model, not a limitation of this tool — J-Runner needs it
   too). Put it on the SD card so the standalone flow can use it.

### What goes on the microSD card

```
/cpukey.txt            <- your 32-hex-char CPU key (first 32 hex chars are used)
/xebuild/data/
   cb.bin
   cbb.bin
   cd.bin
   ce.bin
   patches.bin
   smc.bin
   xell-reloaded.bin   (optional, for first-boot CPU-key read)
```

The `xebuild/data` folder is the patched building blocks for your target
dashboard (bootloaders, patch set, glitch SMC, xeLL). **These are
Microsoft-derived and are not distributed here** — but **you already have
them**: it's the same `data/` folder your working J-Runner-With-Extras install
uses. The exact bootloader filenames depend on the dashboard/CB version;
rename to the above or edit the ingredient list in
[`main/build_image.c`](main/build_image.c).

---

## Build & flash the firmware

You only do this once, to put *this tool* onto the ESP32. Two options — pick one.

### Option A — ESP-IDF (recommended; this is what the repo is built against)

**1. Install ESP-IDF v5.3** (one time). On Windows, install the
[ESP-IDF Windows Installer](https://dl.espressif.com/dl/esp-idf/); it gives you
an **"ESP-IDF 5.3 PowerShell"** shortcut. On Linux/macOS:

```bash
git clone -b v5.3.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
. ./export.sh
```

**2. Open the ESP-IDF terminal** (the "ESP-IDF 5.3 PowerShell" on Windows, or a
shell where you ran `. ./export.sh`).

**3. Go to the project folder.** Replace the path with wherever you cloned it:

```
cd C:\Users\you\TrinityWebFlasher        # Windows
cd ~/TrinityWebFlasher                   # Linux/macOS
```

**4. Set the chip target** (one time):

```
idf.py set-target esp32s3
```

**5. Build:**

```
idf.py build
```

Expect it to end with `Project build complete.` and a line showing the `.bin`
size.

**6. Plug the ESP32-S3 into USB and flash.** Find the port first:
- Windows: Device Manager → Ports → something like `COM5`.
- Linux: usually `/dev/ttyACM0` or `/dev/ttyUSB0`.
- macOS: `/dev/cu.usbmodem*`.

```
idf.py -p COM5 flash monitor        # use your port
```

`monitor` shows the boot log; you should see `SoftAP up` and
`web server up at http://192.168.4.1/`. Press `Ctrl+]` to exit the monitor.

### Option B — PlatformIO

A `platformio.ini` is included. In VS Code with the PlatformIO extension, open
the folder and click **Build**, then **Upload**. (Note: PlatformIO fetches its
toolchain from its own registry; if that's blocked on your network, use Option
A.)

---

## Using it (all on the touch screen)

1. Put `cpukey.txt` and the `xebuild/data` folder on the microSD card and
   insert it.
2. Power the console to **standby** (the ESP can run from the console's 3.3 V
   standby, same as a Pico, or from its own USB with a common ground).
3. On the screen, tap through the flow:
   - **Detect** — stops the SMC and shows the flash config (confirms wiring).
   - **Dump** — reads the NAND twice and compares. The verified dump is held in
     PSRAM for the build.
   - **Build** — reads your CPU key from `cpukey.txt`, checks the SD package,
     and assembles the image. (Gated — see Project status.)
   - **Flash** — writes a verified image back.

The progress bar and status line track each step; green = done, red = problem,
amber = working. No phone, PC, or network involved.

---

## Project status

Honest breakdown of what's proven vs. what needs work, so you know exactly what
you're holding:

| Part | State |
|---|---|
| SB SPI register protocol (`spiex.c`) | **Ported from proven PicoFlasher/espflasher values.** |
| NAND read / erase / write sequences (`xbox.c`) | **Ported from proven values.** |
| SMC stop/start timing | **Ported from proven values.** |
| Two-pass dump + compare | Implemented. |
| Standalone touch UI (ILI9341 + XPT2046) | Implemented. |
| microSD xeBuild-package + cpukey loader | Implemented. |
| NAND spare **ECC** generation (`nand_ecc.c`) | **Reconstructed — NOT verified. Gated off.** |
| Keyvault decrypt / image crypto chain | **Staged — NOT implemented/verified. Gated off.** |
| **"Build a flashable image"** | **Runs end-to-end but returns `NOT_VERIFIED`; flashing a built image is blocked** until both gates are cleared. |

**The two gates** live in [`main/build_image.c`](main/build_image.c):
`BUILD_ECC_VERIFIED` and `BUILD_CRYPTO_VERIFIED`, both `0`. Until they're `1`
(after checking this tool's output **byte-for-byte against a known-good J-Runner
build** on the bench), the Flash button refuses a built image. **Reading/dumping
is safe and usable now;** building a bootable image is the remaining real work.

This design is deliberate: *it compiles* and *it is safe to flash* are kept as
two separate claims. Don't flash a built image until you've cleared the gates
with a verified parity test.

---

## License

GPL-3.0 (see [LICENSE](LICENSE)). Attribution in [NOTICE](NOTICE). For use on a
console you own.
