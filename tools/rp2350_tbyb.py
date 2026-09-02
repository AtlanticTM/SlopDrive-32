#!/usr/bin/env python3
"""rp2350_tbyb -- emit the try-before-you-buy variant of an RP2350 image.

Sets PICOBIN_IMAGE_TYPE_EXE_TBYB (0x8000) in every IMAGE_TYPE item that follows
a PICOBIN block marker, and writes the result beside the input as
<name>-tbyb.<ext>. The plain image is left untouched: the bootrom skips a TBYB
image on a normal boot by design, so the plain one stays the USB rescue image
and only the S3 link ships the variant (build-test-deploy.md).

Unsigned images carry no hash over the block, which is what makes a byte patch
legal. Constants are pico-sdk boot/picobin.h; a signed image would need
`picotool seal` instead and this script refuses it by construction (a HASH item
in the block fails the census below).

PlatformIO: `extra_scripts = post:tools/rp2350_tbyb.py` on env rp2350_motion.
Standalone: `python tools/rp2350_tbyb.py <firmware.bin|firmware.uf2> [--check]`.
"""
import struct
import sys
from pathlib import Path

MARKER = struct.pack("<I", 0xFFFFDED3)
ITEM_IMAGE_TYPE = 0x42
ITEM_HASH_DEF = 0x47
TBYB = 0x8000

UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
UF2_BLOCK = 512


def block_items(img: bytes, at: int):
    """Yield (offset, type) for each item of the block whose marker is at `at`."""
    pos = at + 4
    while pos + 4 <= len(img):
        kind = img[pos]
        if kind == 0xFF:  # LAST item closes the block
            return
        if kind & 0x80:   # 2BS item: 16-bit size
            words = img[pos + 1] | (img[pos + 2] << 8)
        else:             # 1BS item: 8-bit size
            words = img[pos + 1]
        yield pos, kind
        if words == 0:
            return
        pos += 4 * words


def image_type_items(img: bytes):
    """Offsets of every IMAGE_TYPE item that heads a PICOBIN block."""
    at = img.find(MARKER)
    while at >= 0:
        items = list(block_items(img, at))
        if items and items[0][1] == ITEM_IMAGE_TYPE:
            # A hashed block cannot be byte-patched: refuse rather than brick.
            if any(k == ITEM_HASH_DEF for _, k in items):
                raise SystemExit("rp2350_tbyb: block carries a HASH item; use picotool seal")
            yield items[0][0]
        at = img.find(MARKER, at + 4)


def patch_image(img: bytearray) -> int:
    """Set TBYB on every image-type item; return how many were set."""
    n = 0
    for item in list(image_type_items(img)):
        flags = struct.unpack_from("<H", img, item + 2)[0]
        struct.pack_into("<H", img, item + 2, flags | TBYB)
        n += 1
    return n


def check_image(img: bytes) -> tuple[int, int]:
    """(items found, items carrying TBYB)."""
    found = flagged = 0
    for item in image_type_items(img):
        found += 1
        if struct.unpack_from("<H", img, item + 2)[0] & TBYB:
            flagged += 1
    return found, flagged


def uf2_image(raw: bytes) -> bytes:
    blocks = list(uf2_blocks(raw))
    base = min(a for _, a, _ in blocks)
    top = max(a + s for _, a, s in blocks)
    img = bytearray(b"\xff" * (top - base))
    for off, addr, size in blocks:
        img[addr - base:addr - base + size] = raw[off + 32:off + 32 + size]
    return bytes(img)


def uf2_blocks(raw: bytes):
    for off in range(0, len(raw), UF2_BLOCK):
        b = raw[off:off + UF2_BLOCK]
        m0, m1, _flags, addr, size = struct.unpack_from("<IIIII", b, 0)
        if m0 != UF2_MAGIC0 or m1 != UF2_MAGIC1 or struct.unpack_from("<I", b, 508)[0] != UF2_MAGIC_END:
            raise SystemExit(f"rp2350_tbyb: bad UF2 block at {off}")
        yield off, addr, size


def patch_uf2(raw: bytes) -> tuple[bytearray, int]:
    # Reassemble by address so a marker at a payload edge still patches whole.
    blocks = list(uf2_blocks(raw))
    base = min(a for _, a, _ in blocks)
    top = max(a + s for _, a, s in blocks)
    img = bytearray(b"\xff" * (top - base))
    for off, addr, size in blocks:
        img[addr - base:addr - base + size] = raw[off + 32:off + 32 + size]
    n = patch_image(img)
    out = bytearray(raw)
    for off, addr, size in blocks:
        out[off + 32:off + 32 + size] = img[addr - base:addr - base + size]
    return out, n


def convert(src: Path) -> Path:
    raw = src.read_bytes()
    if src.suffix.lower() == ".uf2":
        out, n = patch_uf2(raw)
        found, flagged = check_image(uf2_image(bytes(out)))
    else:
        out = bytearray(raw)
        n = patch_image(out)
        found, flagged = check_image(out)
    if n == 0 or found != flagged:
        raise SystemExit(f"rp2350_tbyb: {src.name}: no image-type item found (set {n}, {flagged}/{found})")
    dst = src.with_name(f"{src.stem}-tbyb{src.suffix}")
    dst.write_bytes(out)
    print(f"rp2350_tbyb: {dst.name}: TBYB set on {n} image-type item(s)")
    return dst


def _main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    src = Path(argv[0])
    if "--check" in argv:
        raw = src.read_bytes()
        found, flagged = check_image(uf2_image(raw) if src.suffix.lower() == ".uf2" else raw)
        print(f"rp2350_tbyb: {src.name}: {flagged}/{found} image-type item(s) carry TBYB")
        return 0 if found and flagged == found else 1
    convert(src)
    return 0


try:
    Import("env")  # noqa: F821  (SCons injects this when run as a PlatformIO script)
except NameError:
    if __name__ == "__main__":
        sys.exit(_main(sys.argv[1:]))
else:
    def _emit(name):
        def action(target, source, env):
            convert(Path(env.subst("$BUILD_DIR")) / name)
        return action

    # The UF2 is a post action of the ELF (registered by the platform before
    # this script loads, so it runs first); the BIN is its own objcopy target
    # that builds after the ELF's post actions. One hook per artifact.
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", _emit("firmware.uf2"))
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _emit("firmware.bin"))
