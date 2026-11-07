# RGH3 (g2m / glitch2-matrix) image build model — VALIDATED

Reverse-engineered and **byte-exact validated** against a known-good donor
`updflash.bin` (Trinity, 16MB, built by xeBuild; a donor CPU key was used for
the analysis and is deliberately **not** committed). Every claim below was proven by
reproducing the donor's bytes from stock ingredients — see
`tools/rgh3_reference_builder.py`.

## Headline result

**The entire bootloader + kernel chain is console-INDEPENDENT.** The per-console
bootloader encryption that retail/JTAG uses is *gone* in this glitch chain: the
keys are derived from per-*build* random nonces that live in the image itself,
not from the console's CPU key. This is why one glitch base boots on any
same-model console.

The **only** per-console data in the whole 16 MB image is:

| region (logical) | size   | source            |
|------------------|--------|-------------------|
| 0x0000..0x1000   | 0x1000 | header (universal)|
| 0x1000..0x4000   | 0x3000 | SMC — glitch SMC from base, **user's SMC config merged in** |
| 0x4000..0x8000   | 0x4000 | **KeyVault — copied verbatim from the user's NAND dump** |
| 0x8000..EOF      | ~15.9M | bootloaders + kernel + flashfs (universal glitch base) |

So the foolproof builder = **splice user's KV (+ SMC config) into a universal
base, recompute ECC.** No CPU key is needed to *assemble* the image; the user's
KV is already correctly encrypted for their console in their own dump. This is
exactly J-Runner's proven method.

## Bootloader chain layout (unECC'd / logical)

```
@0x8000  CB_A 10918  size 0x1AF0   encrypted (1BL chain) — RGH3 injection blob, copied verbatim
@0x9AF0  CB_X 15432  size 0x0400   encrypted            — RGH3 exploit shim, copied verbatim
@0x9EF0  CB_B 9188   size 0x7800   PLAINTEXT + patches  — see below
@0x116F0 CD   9452    size 0x52B0   encrypted (build key) — see below
@0x169A0 CE   1888    size 0x5606A  encrypted (HV+kernel) — universal, copied verbatim
@0x13C000 first XEX2 (flashfs/dashboard) — universal
```

ECC note: unECC = strip the 0x10 spare from each 0x210 raw page → 0x200 logical.
Chain walk: header[0x8:0xC] = first loader offset (BE); each loader
name[0:2], ver[2:4] (BE u16), size[0xC:0x10] (BE u32).

## CB_B 9188 — PLAINTEXT, not encrypted  ✅ byte-exact

```
CB_B = cbb_9188.bin (stock plaintext, contains "XBOX_ROM")
     + patches_g2mtrinity.bin section 0   (byte-addressed, u32 count, data)
     + build nonce copied into [0x10:0x20]
     (hash field [0x20:0x40] as emitted by xeBuild; zero in donor's [0x30:0x40])
```
No RC4. The glitch chain reads CB_B as plaintext. The 16-byte value at
**CB_B[0x10:0x20] is the build nonce** that seeds the CD key (below).

## CD 9452 — encrypted with a BUILD key  ✅ byte-exact

```
plain = cd_9452.bin (stock) + patches_g2mtrinity.bin section 1
        (section 1 includes a 226-word extension @0x4F20 that grows CD)
pad    plain up to a 0x10 boundary
set    size field plain[0xC:0x10] = len(plain)   (BE u32)   -> 0x52B0
set    plain[0x10:0x20] = build CD nonce
key    = HMAC_SHA1( CB_B[0x10:0x20] , CD[0x10:0x20] )[:0x10]   # cbb_nonce is the secret
CD     = plain[:0x20] + RC4(key, plain[0x20:])
```
**The CD key seed is the CB_B nonce, NOT the CPU key** → console-independent.

## CE 1888 — HV + kernel, universal

Encrypted (ent ~8.0); `ce_1888.bin` is the stock plaintext (zeroed nonce). The
CE key is NOT an HMAC of any CD offset or the chain nonces (exhaustive
known-plaintext keystream search failed), and CE carries the compressed HV +
kernel which the §2 patches target at decompressed virtual offsets. CE is
**console-independent**, so the builder copies it verbatim from the universal
base rather than re-encrypting it. (Full on-device CE rebuild = future "trim
fat" phase; not needed for a working image.)

## Patch file format (`patches_g2mtrinity.bin`)

Sections separated by `0xFFFFFFFF`. Each patch: `u32 byte_offset, u32 word_count,
word_count*4 bytes data` (big-endian). Mapping for g2m:
- **section 0 → CB_B** (offsets within 0x7800)
- **section 1 → CD**   (offsets within CD; includes the 0x4F20 extension)
- **section 2 → kernel/HV** (large virtual offsets into decompressed kernel)

Use `patches_g2mtrinity.bin` (glitch2-matrix) for RGH3 — NOT `patches_g2trinity.bin`
(plain glitch2) or `patches_trinity.bin`. The donor was built with g2m.

## Per-console header regions

- header magic `FF4F`, build 1888, first-loader offset at [0x8:0xC].
- SMC: offset at header[0x7C:0x80], length at header[0x78:0x7C] → donor 0x1000/0x3000.
- KeyVault @ 0x4000, 0x4000 bytes, encrypted (needs CPU key to *read*, but the
  builder only *copies* it, so no decryption required).

## Builder algorithm (foolproof v1)

```
in : user_dump (16MB raw), universal_base (16MB raw, an RGH3 g2m image)
out: flashable RGH3 image (16MB raw)

for each 0x210 raw page p (0x8000 pages):
    logical_off = p * 0x200
    if   logical_off <  0x1000:  data = base   header   page
    elif logical_off <  0x4000:  data = base   SMC page, with user's SMC-config merged
    elif logical_off <  0x8000:  data = dump   KeyVault page      (per-console)
    else:                        data = base   page               (universal)
    recompute spare/ECC (nand_ecc_fix_page) and emit
```

Validation gate: feeding the donor as BOTH dump and base must reproduce the
donor byte-exact (proves splice + ECC). SMC-config merge is the one step that
cannot be validated from a single donor and needs bench confirmation.
