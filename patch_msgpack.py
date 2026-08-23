"""
PlatformIO pre-build script: expose hideakitai/MsgPack internals used by microLXMF.

LXMessage's wire format includes a `dict[int, Any]` fields map. To splice
arbitrary msgpack values into the stream without LXMessage knowing each value's
type, the encoder uses Packer::packRawBytes (private upstream) to write
pre-encoded bytes; the decoder uses Unpacker::indices / raw_data (also private
upstream) to capture each key+value byte span as an opaque slice.

This promotes the relevant access modifiers from private to public. Idempotent.

Ported from pyxis/patch_msgpack.py, which mirrors the patch microLXMF's
conformance-bridge applies via its CMakeLists.
"""
Import("env")
import os

MSGPACK_BASE = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "MsgPack", "MsgPack"
)


def apply_patch(filepath, old, new, label):
    if not os.path.exists(filepath):
        print(f"PATCH: {os.path.basename(filepath)} not found, skipping {label}")
        return
    with open(filepath, "r") as f:
        content = f.read()
    if old in content:
        with open(filepath, "w") as f:
            f.write(content.replace(old, new))
        print(f"PATCH: {label}")
    elif new in content or "patched for microLXMF" in content:
        print(f"PATCH: {label} (already applied)")
    else:
        print(f"PATCH: WARNING -- {label}: expected pattern not found")


apply_patch(
    os.path.join(MSGPACK_BASE, "Packer.h"),
    "    private:\n        void packRawByte",
    "    public:  // patched for microLXMF (needs packRawBytes)\n        void packRawByte",
    "Packer.h: expose packRawBytes/packRawByte as public",
)

apply_patch(
    os.path.join(MSGPACK_BASE, "Unpacker.h"),
    "    class Unpacker {\n        uint8_t* raw_data",
    "    class Unpacker {\n    public:  // patched for microLXMF (needs indices/raw_data)\n        uint8_t* raw_data",
    "Unpacker.h: expose indices/raw_data as public",
)
