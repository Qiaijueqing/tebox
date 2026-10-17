#!/usr/bin/env python3
"""Android 15 system: make the Android 16 KeyMint V4 NDK library report V3.

Android 15's keystore2 accepts KeyMint V1-V3 only (hal_version 100-300) and
aborts on 400. V4 keeps every V3 transaction, so the service can be offered
as V3. The version is a constant compiled into the library, both in
BnKeyMintDevice::getInterfaceVersion and inlined into onTransact, so the two
`mov wN, #4` instructions are patched to #3. A V4 library regenerated with
--version=3 is no substitute: libkeymint_fake_latest was built against the
Android 16 generated classes and their vtable layout.

usage: keymint-v4-as-v3.py <in: android.hardware.security.keymint-V4-ndk.so> <out>
"""
import hashlib
import sys

# sha256 of the Android 16 (ColorOS OP64DDL1 16) library this was made for.
EXPECTED = "d68514815239c4a31b751a6e4b8d99a2baae2fc4f8baed53a77175c78c13f5e4"
PATCHES = [
    # (file offset, original, patched) — little-endian A64 words
    (0xF39C, 0x52800081, 0x52800061),  # onTransact: mov w1, #4 -> #3
    (0x16128, 0x52800088, 0x52800068),  # getInterfaceVersion: mov w8, #4 -> #3
]

src, dst = sys.argv[1], sys.argv[2]
data = bytearray(open(src, "rb").read())
digest = hashlib.sha256(data).hexdigest()
if EXPECTED and digest != EXPECTED:
    sys.exit(f"unexpected input {digest}")
for off, old, new in PATCHES:
    word = int.from_bytes(data[off:off + 4], "little")
    if word != old:
        sys.exit(f"0x{off:x}: found 0x{word:08x}, expected 0x{old:08x}")
    data[off:off + 4] = new.to_bytes(4, "little")
open(dst, "wb").write(data)
print(f"patched {src} ({digest[:12]}) -> {dst}")
