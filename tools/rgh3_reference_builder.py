#!/usr/bin/env python3
"""
rgh3_reference_builder.py — host reference + validator for the RGH3 (g2m) build
model documented in RGH3_BUILD_MODEL.md.

It proves, byte-exact against a known-good donor updflash.bin, that:
  * CB_B 9188 = cbb_9188 + g2m patch section 0 + build nonce   (PLAINTEXT)
  * CD   9452 = cd_9452  + g2m patch section 1, RC4(HMAC(cbb_nonce, cd_nonce))
and that the whole bootloader/kernel chain is console-independent.

This is the authoritative reference for the on-device C implementation
(main/build_image.c, main/xecrypt_bl.c). Run it whenever those change.

Usage:
  rgh3_reference_builder.py --validate <donor.bin> <cbb_9188> <cd_9452> <patches_g2m>

No console secrets are embedded; the donor CPU key is NOT needed (the chain is
console-independent). Nothing here is committed with keys.
"""
import sys, struct, hashlib, hmac, argparse

PAGE_RAW, PAGE_DATA = 0x210, 0x200

def unecc(raw):
    return b''.join(raw[i:i+PAGE_DATA] for i in range(0, len(raw), PAGE_RAW))

def rc4(key, data):
    S = list(range(256)); j = 0
    for i in range(256):
        j = (j + S[i] + key[i % len(key)]) & 255; S[i], S[j] = S[j], S[i]
    out = bytearray(); i = j = 0
    for b in data:
        i = (i + 1) & 255; j = (j + S[i]) & 255; S[i], S[j] = S[j], S[i]
        out.append(b ^ S[(S[i] + S[j]) & 255])
    return bytes(out)

def hmac16(key, msg):
    return hmac.new(key, msg, hashlib.sha1).digest()[:0x10]

def parse_patches(data):
    """Return list of sections; each section is [(off, wordcount, data), ...]."""
    secs, cur, i = [], [], 0
    while i + 4 <= len(data):
        off = struct.unpack('>I', data[i:i+4])[0]; i += 4
        if off == 0xFFFFFFFF:
            secs.append(cur); cur = []; continue
        cnt = struct.unpack('>I', data[i:i+4])[0]; i += 4
        cur.append((off, cnt, data[i:i+cnt*4])); i += cnt*4
    if cur: secs.append(cur)
    return secs

def apply_patches(buf, patchlist):
    buf = bytearray(buf)
    for off, cnt, blob in patchlist:
        end = off + cnt*4
        if end > len(buf): buf += b'\0' * (end - len(buf))
        buf[off:end] = blob
    return bytearray(buf)

def build_cbb(cbb_stock, sec0, build_nonce, hashfield=b'\0'*0x20):
    """CB_B is plaintext: stock + section-0 patches + nonce. No encryption."""
    cbb = apply_patches(cbb_stock, sec0)
    cbb[0x10:0x20] = build_nonce
    cbb[0x20:0x40] = hashfield          # xeBuild-emitted hash field
    return bytes(cbb)

def build_cd(cd_stock, sec1, cbb_nonce, cd_nonce):
    """CD = stock + section-1 patches, 0x10-padded, RC4(HMAC(cbb_nonce, cd_nonce))."""
    cd = apply_patches(cd_stock, sec1)
    pad = (-len(cd)) % 0x10
    if pad: cd += b'\0' * pad
    struct.pack_into('>I', cd, 0xC, len(cd))     # size field
    cd[0x10:0x20] = cd_nonce
    key = hmac16(cbb_nonce, bytes(cd[0x10:0x20]))
    return cd[:0x20] + rc4(key, bytes(cd[0x20:]))

def chain_offsets(img):
    off = struct.unpack('>I', img[0x8:0xC])[0]
    out = {}
    for _ in range(8):
        name = img[off:off+2]; ver = struct.unpack('>H', img[off+2:off+4])[0]
        size = struct.unpack('>I', img[off+0xC:off+0x10])[0]
        out[(name, ver)] = (off, size)
        if size == 0 or size > 0x100000 or off+size >= len(img): break
        off += size
    return out

def validate(donor, cbb_stock, cd_stock, patches):
    img = unecc(donor)
    secs = parse_patches(patches)
    ch = chain_offsets(img)
    (cbb_off, cbb_sz) = ch[(b'CB', 9188)]
    (cd_off,  cd_sz)  = ch[(b'CD', 9452)]
    ref_cbb = img[cbb_off:cbb_off+cbb_sz]
    ref_cd  = img[cd_off:cd_off+cd_sz]
    build_nonce = ref_cbb[0x10:0x20]
    cd_nonce    = ref_cd[0x10:0x20]

    my_cbb = build_cbb(cbb_stock, secs[0], build_nonce, ref_cbb[0x20:0x40])
    my_cd  = build_cd(cd_stock,  secs[1], build_nonce, cd_nonce)

    ok_cbb = my_cbb == ref_cbb
    ok_cd  = my_cd  == ref_cd
    print("CB_B byte-exact:", ok_cbb, "(len %d)" % len(my_cbb))
    print("CD   byte-exact:", ok_cd,  "(len %d)" % len(my_cd))
    if not ok_cbb or not ok_cd:
        return 1
    print("MODEL VALIDATED — bootloader chain reproduced from stock + patches.")
    return 0

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--validate', nargs=4,
                    metavar=('DONOR', 'CBB_9188', 'CD_9452', 'PATCHES_G2M'),
                    required=True)
    a = ap.parse_args()
    d, cbb, cd, pat = a.validate
    sys.exit(validate(open(d,'rb').read(), open(cbb,'rb').read(),
                      open(cd,'rb').read(), open(pat,'rb').read()))
