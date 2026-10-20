#!/usr/bin/env bash
# Usage: bash modules/build_module.sh modules/<name>
# Output: modules/<name>/<name>.modjash
set -e

MOD_DIR="$1"
if [ -z "$MOD_DIR" ]; then
    echo "Usage: $0 modules/<name>" >&2
    exit 1
fi

MOD_NAME="$(basename "$MOD_DIR")"
SRC="$MOD_DIR/${MOD_NAME}_module.c"
MANIFEST="$MOD_DIR/manifest.toml"
OUT="$MOD_DIR/${MOD_NAME}.modjash"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -f "$SRC" ]; then
    echo "Error: source not found: $SRC" >&2
    exit 1
fi

# ── Host detection ──────────────────────────────────────────────────────────
# Mirrors the osif target selection in CMakeLists.txt. A .modjash embeds a
# NATIVE shared object, so it is platform-specific by construction: a bundle
# built on macOS carries a Mach-O and cannot load on Linux (and vice versa).
# Always rebuild modules on the host that will run them.
UNAME_S="$(uname -s)"
case "$UNAME_S" in
    Darwin)
        LIB_EXT="dylib"
        # -undefined dynamic_lookup: symbols from the host jash binary are
        # resolved at dlopen() time, not link time. Standard macOS plugin
        # pattern; ld64 needs to be told, ELF allows it by default.
        HOST_CFLAGS=(-D_DARWIN_C_SOURCE -mmacosx-version-min=12.0)
        HOST_LDFLAGS=(-Wl,-undefined,dynamic_lookup)
        ;;
    Linux)
        LIB_EXT="so"
        HOST_CFLAGS=()
        HOST_LDFLAGS=()
        ;;
    *)
        # BSDs and friends: ELF defaults are the right starting point.
        LIB_EXT="so"
        HOST_CFLAGS=()
        HOST_LDFLAGS=()
        echo "[build_module] warning: untested host '$UNAME_S' — using ELF defaults" >&2
        ;;
esac

# Temp dir for the intermediate shared object
TMP_DIR="${HOME}/.jash/tmp"
mkdir -p "$TMP_DIR"
chmod 0700 "${HOME}/.jash" "$TMP_DIR"
TMP_LIB="${TMP_DIR}/${MOD_NAME}_body.${LIB_EXT}"
INSTALL_TMP=
OUTPUT_TMP=
cleanup() {
    rm -f "$TMP_LIB"
    [ -z "$INSTALL_TMP" ] || rm -f "$INSTALL_TMP"
    [ -z "$OUTPUT_TMP" ] || rm -f "$OUTPUT_TMP"
}
trap cleanup EXIT

echo "[build_module] Compiling $SRC ($UNAME_S)..."
MODULE_CC="${CC:-cc}"
# Test/package trees may symlink the main source while its private includes
# remain beside the real file. Resolve that directory without changing cwd.
MODULE_SOURCE_DIR="$(python3 -c 'import os,sys; print(os.path.dirname(os.path.realpath(sys.argv[1])))' "$SRC")"
"$MODULE_CC" -O2 -fPIC -shared -Wall -Wextra -Wpedantic -Werror \
    -I"$MODULE_SOURCE_DIR" \
    -I"$ROOT/c" -I"$ROOT/c/sys" -I"$ROOT/c/osif" -I"$ROOT/libjef" \
    -DJASH_TARGET_POSIX \
    -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    "${HOST_CFLAGS[@]}" \
    -Wno-unused-function -Wno-unused-parameter \
    "${HOST_LDFLAGS[@]}" \
    -o "$TMP_LIB" "$SRC"

echo "[build_module] Packaging $OUT..."
OUTPUT_TMP="$(mktemp "$MOD_DIR/.${MOD_NAME}.modjash.XXXXXX")"
python3 - "$MANIFEST" "$TMP_LIB" "$OUTPUT_TMP" << 'PYEOF'
import sys, struct

manifest_path = sys.argv[1]
body_path     = sys.argv[2]
out_path      = sys.argv[3]

with open(manifest_path, 'rb') as f: manifest = f.read()
with open(body_path, 'rb') as f:     body     = f.read()

MAGIC = b'JMODv01\x00'  # 8 bytes
# Header layout (64 bytes total, little-endian):
#   magic[8], abi_version(u32), flags(u32), body_blake3[32],
#   manifest_len(u32), body_len(u32), sig_len(u32), reserved(u32)
header = (MAGIC
          + struct.pack('<I', 1)              # abi_version
          + struct.pack('<I', 0)              # flags
          + b'\x00' * 32                     # body_blake3 (zeros = unverified)
          + struct.pack('<I', len(manifest))  # manifest_len
          + struct.pack('<I', len(body))      # body_len
          + struct.pack('<I', 0)              # sig_len
          + struct.pack('<I', 0))             # reserved

assert len(header) == 64, f"header is {len(header)} bytes, expected 64"

with open(out_path, 'wb') as f:
    f.write(header + manifest + body)

total = 64 + len(manifest) + len(body)
print(f"[build_module] OK: {out_path} ({total} bytes, "
      f"manifest={len(manifest)}, body={len(body)})")
PYEOF

chmod 0600 "$OUTPUT_TMP"
mv -f "$OUTPUT_TMP" "$OUT"
OUTPUT_TMP=

# Install to ~/.jash/modules/ so the module resolves from ANY cwd (not just the
# dev tree). Required for JASH_SIGIL>=1 to find sigil.modjash when the agent
# runs jash from an arbitrary directory. (ROADMAP W1.1)
INSTALL_DIR="${HOME}/.jash/modules"
mkdir -p "$INSTALL_DIR"
chmod 0700 "${HOME}/.jash" "$INSTALL_DIR"
INSTALL_TMP="$(mktemp "$INSTALL_DIR/.${MOD_NAME}.modjash.XXXXXX")"
cp "$OUT" "$INSTALL_TMP"
chmod 0600 "$INSTALL_TMP"
mv -f "$INSTALL_TMP" "$INSTALL_DIR/${MOD_NAME}.modjash"
INSTALL_TMP=
echo "[build_module] Installed: $INSTALL_DIR/${MOD_NAME}.modjash"
