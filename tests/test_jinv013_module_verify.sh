#!/usr/bin/env bash
# JINV-013: manifests and module bytes must be verified before policy/cache use
# and dlopen must execute the same file descriptor that was verified.

set -u
cd "$(dirname "$0")/.." || exit 2
export JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off

prereq_fail() {
    printf 'FAIL  prerequisite\n        %s\n' "$1"
    exit 1
}

[ -f c/module/loader.c ] || prereq_fail "c/module/loader.c missing"
command -v cc >/dev/null 2>&1 || prereq_fail "cc unavailable"
command -v python3 >/dev/null 2>&1 || prereq_fail "python3 unavailable"

binary_is_fresh() {
    local binary=$1 source
    [ -x "$binary" ] || return 1
    for source in c/main.c c/module/loader.c c/module/loader.h c/module/modjash.h \
                  c/module/sigil.c c/builtins/sigil.c c/builtins/module.c \
                  c/module/blake3.c libjef/ed25519.c; do
        [ "$source" -nt "$binary" ] && return 1
    done
    return 0
}

if [ -z "${JASH:-}" ]; then
    for candidate in "$PWD/build/jash" "$PWD/build-speed/jash" \
                     "$PWD/build-speed/jash-release"; do
        if binary_is_fresh "$candidate" &&
           "$candidate" -c 'exit 0' >/dev/null 2>&1; then
            JASH=$candidate
            break
        fi
    done
fi
[ -n "${JASH:-}" ] || prereq_fail "no fresh runnable jash binary"
binary_is_fresh "$JASH" || prereq_fail "$JASH is stale relative to loader/bootstrap sources"
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

pass=0
fail=0
skip=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }
skip() { skip=$((skip+1)); printf 'skip  %s\n' "$1"; }

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM
export HOME="$TMP/home"
mkdir -p "$HOME/.jash/modules" "$HOME/.jash/keys" "$TMP/modules"
chmod 0700 "$HOME/.jash" "$HOME/.jash/modules" "$HOME/.jash/keys" "$TMP/modules"

# Build the loader under test from every source that implements its security
# decisions. This avoids accepting an unrelated or stale build/jash binary.
cat >"$TMP/loader_harness.c" <<'C'
#define _GNU_SOURCE
#include <module/loader.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--resolve-small") == 0) {
        char path[16];
        errno = 0;
        return !jmod_resolve_installed(argv[2], path, sizeof(path)) &&
               path[0] == '\0' && errno == ENAMETOOLONG ? 0 : 1;
    }
    if (argc == 4 && strcmp(argv[1], "--lifetime") == 0) {
        if (jmod_load(argv[2], UINT32_MAX) != 0 ||
            jmod_load(argv[3], UINT32_MAX) != 0 || g_modules.count != 2)
            return 1;
        int first_fd = g_modules.entries[0].exec_fd;
        int second_fd = g_modules.entries[1].exec_fd;
        char first_name[64], second_name[64];
        strcpy(first_name, g_modules.entries[0].name);
        strcpy(second_name, g_modules.entries[1].name);
        if (first_fd == second_fd || first_fd < 0 || second_fd < 0 ||
            g_modules.entries[0].dlhandle == g_modules.entries[1].dlhandle ||
            fcntl(first_fd, F_GETFD) < 0 || fcntl(second_fd, F_GETFD) < 0)
            return 1;
        if (jmod_unload(first_name) != 0 ||
            !(fcntl(first_fd, F_GETFD) < 0 && errno == EBADF) ||
            fcntl(second_fd, F_GETFD) < 0 || jmod_unload(second_name) != 0)
            return 1;
        return 0;
    }
    if (argc == 6 && strcmp(argv[1], "--mutation") == 0) {
        jash_module_security_init();
        FILE *f = fopen(argv[4], "w");
        if (!f || fputs("policyprobe: +crypto\n", f) < 0 || fclose(f) != 0)
            return 2;
        unsetenv("JASH_MODULE_REQUIRE_HASH");
        unsetenv("JASH_MODULE_CAPS_STRICT");
        if (setenv("HOME", argv[5], 1) != 0) return 2;
        int hash_loosened = jmod_load(argv[2], UINT32_MAX) == 0;
        int policy_loosened = jmod_load(argv[3], UINT32_MAX) == 0;
        return hash_loosened || policy_loosened ? 1 : 0;
    }
    if (argc != 2) return 2;
    return jmod_load(argv[1], UINT32_MAX) == 0 ? 0 : 1;
}
C

cat >"$TMP/module_crypto.c" <<'C'
#include <ed25519.h>
#include <module/blake3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file(const char *path, uint8_t **out, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END) != 0) return -1;
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t *p = malloc((size_t)n ? (size_t)n : 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p); fclose(f); return -1;
    }
    fclose(f); *out = p; *len = (size_t)n; return 0;
}

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    uint8_t *input = NULL, *secret = NULL;
    size_t input_len = 0, secret_len = 0;
    if (argc == 4 && strcmp(argv[1], "hash") == 0 &&
        read_file(argv[2], &input, &input_len) == 0) {
        uint8_t digest[32];
        blake3_hash(input, input_len, digest);
        free(input); return write_file(argv[3], digest, sizeof(digest)) == 0 ? 0 : 1;
    }
    if (argc == 5 && strcmp(argv[1], "keypair") == 0 &&
        read_file(argv[2], &input, &input_len) == 0 && input_len == ED25519_SEED) {
        uint8_t pk[ED25519_PUBKEY], sk[ED25519_SECKEY];
        ed25519_keypair_from_seed(input, pk, sk);
        free(input);
        return write_file(argv[3], pk, sizeof(pk)) == 0 &&
               write_file(argv[4], sk, sizeof(sk)) == 0 ? 0 : 1;
    }
    if (argc == 5 && strcmp(argv[1], "sign") == 0 &&
        read_file(argv[2], &input, &input_len) == 0 && input_len <= UINT32_MAX &&
        read_file(argv[3], &secret, &secret_len) == 0 && secret_len == ED25519_SECKEY) {
        uint8_t sig[ED25519_SIG];
        int rc = ed25519_sign(sig, input, (uint32_t)input_len, secret);
        free(input); free(secret);
        return rc == 0 && write_file(argv[4], sig, sizeof(sig)) == 0 ? 0 : 1;
    }
    free(input); free(secret); return 2;
}
C

DL_LIB=
[ "$(uname -s)" = Linux ] && DL_LIB=-ldl
LOADER="$TMP/loader_harness"
CRYPTO="$TMP/module_crypto"
if ! cc -std=c11 -I"$PWD/c" -I"$PWD/libjef" \
        -o "$LOADER" "$TMP/loader_harness.c" c/module/loader.c \
        c/module/blake3.c libjef/ed25519.c $DL_LIB; then
    prereq_fail "could not build loader harness from current sources"
fi
if ! cc -std=c11 -I"$PWD/c" -I"$PWD/libjef" -o "$CRYPTO" \
        "$TMP/module_crypto.c" c/module/blake3.c libjef/ed25519.c; then
    prereq_fail "could not build module fixture crypto helper"
fi

load_module() { "$LOADER" "$1"; }

if "$LOADER" --resolve-small sigil; then
    ok "resolver rejects truncation and clears the output path"
else
    bad "resolver long-path handling" "a truncated installed path was reported as usable"
fi
long_path="$TMP/$(printf 'x%.0s' {1..5000})"
if out=$(load_module "$long_path" 2>&1); then rc=0; else rc=$?; fi
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'path too long'; then
    ok "loader rejects paths that cannot be stored truthfully for reload"
else
    bad "loader path storage bound" "rc=$rc output=[$out]"
fi

cat >"$TMP/probe.c" <<'C'
#include <module/modjash.h>

static int probe_init(void) { return 0; }
static int probe_dispatch(const jmod_call_t *call) { (void)call; return -1; }

jmod_ops_t jmod_ops = {
    .init = probe_init,
    .dispatch = probe_dispatch,
    .name = "loader-probe",
    .version = "1.0.0",
    .description = "module loader regression probe",
    .caps_used = 0,
};
C

case "$(uname -s)" in
    Darwin) BODY="$TMP/probe.dylib" ;;
    *)      BODY="$TMP/probe.so" ;;
esac
if ! cc -shared -fPIC -I"$PWD/c" -o "$BODY" "$TMP/probe.c"; then
    prereq_fail "could not compile loader probe"
fi

make_module() {
    local name=$1 target=$2 hash_mode=${3:-zero} flags=${4:-0} sig=${5:-} caps=${6:-0}
    local hash_file=
    if [ "$hash_mode" = valid ]; then
        hash_file="$TMP/body.blake3"
        "$CRYPTO" hash "$BODY" "$hash_file" || prereq_fail "could not hash probe body"
    fi
    python3 - "$name" "$target" "$BODY" "$hash_mode" "$flags" "$sig" "$hash_file" "$caps" <<'PY'
import struct
import sys

name, target, body_path, hash_mode, flags, sig_path, hash_path, caps = sys.argv[1:]
body = open(body_path, 'rb').read()
manifest = (f"name={name}\nversion=1.0.0\n"
            f"description=module loader regression probe\ncaps={caps}\n").encode()
if hash_mode == 'valid':
    body_hash = open(hash_path, 'rb').read()
elif hash_mode == 'wrong':
    body_hash = b'\xAB' * 32
else:
    body_hash = b'\0' * 32
signature = open(sig_path, 'rb').read() if sig_path else b''
header = struct.pack('<8sII32sIIII', b'JMODv01\0', 1, int(flags), body_hash,
                     len(manifest), len(body), len(signature), 0)
open(target, 'wb').write(header + manifest + body + signature)
PY
}

assert_invalid_manifest() {
    local label=$1 name=$2 out rc
    local file="$TMP/modules/$label.modjash"
    make_module "$name" "$file"
    if out=$(load_module "$file" 2>&1); then rc=0; else rc=$?; fi
    if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -qi 'invalid manifest'; then
        ok "$label module name is rejected at manifest parsing"
    else
        bad "$label module name" "rc=$rc output=[$out]"
    fi
}

# Regression: jmod_load must honor jmod_parse_manifest's error return. Empty,
# leading-dot, traversal, and >32-byte names all stop at that boundary.
assert_invalid_manifest empty ""
assert_invalid_manifest leading_dot ".hidden"
assert_invalid_manifest traversal "../escape"
assert_invalid_manifest oversized "123456789012345678901234567890123"

if compgen -G "$HOME/.jash/cache/jmod_*" >/dev/null; then
    bad "invalid names do not reach the cache" "a malformed manifest created a cache body"
else
    ok "invalid names are rejected before cache use"
fi

# Header section arithmetic must be validated against the file size before the
# source snapshot allocation. Sparse files plus a low address-space ceiling
# make allocation-first behavior fail deterministically.
assert_invalid_layout() {
    local label=$1 manifest_len=$2 body_len=$3
    local file="$TMP/modules/layout-$label.modjash" out rc
    python3 - "$file" "$manifest_len" "$body_len" <<'PY'
import struct
import sys
path, manifest_len, body_len = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
header = struct.pack('<8sII32sIIII', b'JMODv01\0', 1, 0, b'\0' * 32,
                     manifest_len, body_len, 0, 0)
with open(path, 'wb') as f:
    f.write(header)
    f.truncate(256 * 1024 * 1024)
PY
    if out=$( (ulimit -v 131072; load_module "$file") 2>&1); then
        rc=0
    else
        rc=$?
    fi
    if [ "$rc" -ne 0 ] &&
       printf '%s' "$out" | grep -q 'invalid section layout' &&
       ! printf '%s' "$out" | grep -q 'snapshot allocation failed'; then
        ok "$label oversized layout is rejected before source allocation"
    else
        bad "$label oversized layout" "rc=$rc output=[$out]"
    fi
}

assert_invalid_layout manifest_span 4294967295 0
assert_invalid_layout body_span 0 4294967295
assert_invalid_layout combined_span 4294967264 64

# A declared, incorrect hash must fail before dlopen.
make_module badhash "$TMP/modules/badhash.modjash" wrong
if out=$(load_module "$TMP/modules/badhash.modjash" 2>&1); then rc=0; else rc=$?; fi
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -qi 'BLAKE3 nao confere'; then
    ok "incorrect body_blake3 is rejected before dlopen"
else
    bad "incorrect body_blake3" "output=[$out]"
fi

# A correctly declared nonzero hash must reach and load the same body bytes.
make_module goodhash "$TMP/modules/goodhash.modjash" valid
if [ "$(uname -s)" = Darwin ]; then
    if out=$(load_module "$TMP/modules/goodhash.modjash" 2>&1); then rc=0; else rc=$?; fi
    if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'immutable execution image unavailable'; then
        ok "macOS refuses verified modules without an immutable execution image"
    else
        bad "macOS verified-module fail-closed gate" "rc=$rc output=[$out]"
    fi
elif [ "$(uname -s)" = Linux ]; then
    if out=$(load_module "$TMP/modules/goodhash.modjash" 2>&1); then
        ok "correct nonzero body_blake3 module loads"
    else
        bad "correct nonzero body_blake3" "output=[$out]"
    fi
else
    skip "successful nonzero-hash load has no immutable execution backend on $(uname -s)"
fi

# Compatibility contract: a zero hash remains loadable unless strict mode is on.
make_module legacy "$TMP/modules/legacy.modjash"
if out=$(load_module "$TMP/modules/legacy.modjash" 2>&1); then
    ok "legacy zero body_blake3 module still loads"
else
    bad "legacy zero body_blake3" "output=[$out]"
fi
if out=$(JASH_MODULE_REQUIRE_HASH=1 load_module "$TMP/modules/legacy.modjash" 2>&1); then
    rc=0
else
    rc=$?
fi
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'JASH_MODULE_REQUIRE_HASH'; then
    ok "strict mode rejects a zero body_blake3 module"
else
    bad "strict body hash mode" "output=[$out]"
fi

# dlopen keys descriptor paths. Closing the first immutable fd immediately lets
# the next module reuse the same /proc/self/fd/N spelling and alias the handle.
make_module lifetimeone "$TMP/modules/lifetimeone.modjash" valid
make_module lifetimetwo "$TMP/modules/lifetimetwo.modjash" valid
if [ "$(uname -s)" != Linux ]; then
    skip "immutable descriptor lifetime requires Linux memfd seals"
elif "$LOADER" --lifetime "$TMP/modules/lifetimeone.modjash" \
               "$TMP/modules/lifetimetwo.modjash" >/dev/null 2>&1; then
    ok "two modules retain distinct immutable descriptors until unload"
else
    bad "immutable descriptor lifetime" "second module aliased or unload did not close its fd"
fi

# Cache writes are observable but advisory: a hit is validated, publication is
# atomic, invalid entries are preserved, and an unavailable cache cannot veto
# verified execution.
CACHE_HASH=valid
[ "$(uname -s)" = Darwin ] && CACHE_HASH=zero
make_module cacheprobe "$TMP/modules/cacheprobe.modjash" "$CACHE_HASH"
if out=$(JASH_MODULE_CACHE_EVENTS=1 load_module \
         "$TMP/modules/cacheprobe.modjash" 2>&1) &&
   printf '%s' "$out" | grep -q 'cache event=stored module=cacheprobe'; then
    ok "cache store emits a stored event"
else
    bad "cache stored event" "output=[$out]"
fi
if out=$(JASH_MODULE_CACHE_EVENTS=1 load_module \
         "$TMP/modules/cacheprobe.modjash" 2>&1) &&
   printf '%s' "$out" | grep -q 'cache event=hit module=cacheprobe'; then
    ok "cache hit is validated and marked"
else
    bad "cache hit event" "output=[$out]"
fi

cache_path=
for candidate in "$HOME"/.jash/cache/jmod_cacheprobe_body_*; do
    [ -f "$candidate" ] && cache_path=$candidate
done
make_module cacheinvalid "$TMP/modules/cacheinvalid.modjash" "$CACHE_HASH"
invalid_cache=${cache_path/jmod_cacheprobe_body_/jmod_cacheinvalid_body_}
if [ -n "$cache_path" ]; then
    printf 'preserve-invalid-cache\n' >"$invalid_cache"
    chmod 500 "$invalid_cache"
fi
if [ -n "$cache_path" ] &&
   out=$(JASH_MODULE_CACHE_EVENTS=1 load_module \
         "$TMP/modules/cacheinvalid.modjash" 2>&1) &&
   printf '%s' "$out" | grep -q 'cache event=invalid-preserved' &&
   grep -q '^preserve-invalid-cache$' "$invalid_cache"; then
    ok "invalid cache entries are non-destructively preserved"
else
    bad "non-destructive invalid cache" "output=[$out]"
fi

CACHE_FAIL_HOME="$TMP/cache-fail-home"
mkdir -p "$CACHE_FAIL_HOME/.jash"
printf 'not-a-directory\n' >"$CACHE_FAIL_HOME/.jash/cache"
make_module cacheoptional "$TMP/modules/cacheoptional.modjash" "$CACHE_HASH"
if out=$(HOME="$CACHE_FAIL_HOME" JASH_MODULE_CACHE_EVENTS=1 \
         load_module "$TMP/modules/cacheoptional.modjash" 2>&1) &&
   printf '%s' "$out" | grep -q 'cache event=unavailable'; then
    ok "cache failure cannot abort verified execution"
else
    bad "best-effort cache failure" "output=[$out]"
fi

# Strict knobs, the policy trust root, and policy bytes are startup decisions.
# The harness pins them, then mutates HOME, environment, and policy on disk.
ALT_HOME="$TMP/alternate-home"
mkdir -p "$ALT_HOME/.jash/modules"
chmod 0700 "$ALT_HOME/.jash" "$ALT_HOME/.jash/modules"
printf 'policyprobe: +crypto\n' >"$ALT_HOME/.jash/modules/policy.conf"
printf 'policyprobe: -crypto\n' >"$HOME/.jash/modules/policy.conf"
chmod 0600 "$ALT_HOME/.jash/modules/policy.conf" \
           "$HOME/.jash/modules/policy.conf"
make_module mutationzero "$TMP/modules/mutationzero.modjash"
make_module policyprobe "$TMP/modules/policyprobe.modjash" valid 0 '' 128
if HOME="$HOME" JASH_MODULE_REQUIRE_HASH=1 JASH_MODULE_CAPS_STRICT=1 \
   "$LOADER" --mutation "$TMP/modules/mutationzero.modjash" \
   "$TMP/modules/policyprobe.modjash" \
   "$HOME/.jash/modules/policy.conf" "$ALT_HOME" >/dev/null 2>&1; then
    ok "hash strict, caps strict, policy HOME, and policy bytes remain pinned"
else
    bad "startup-pinned module policy" "environment or policy mutation loosened a gate"
fi

# Automatic Sigil bootstrap must never consume a repository-controlled CWD.
HOSTILE="$TMP/hostile-cwd"
HOSTILE_HOME="$TMP/hostile-home"
mkdir -p "$HOSTILE/modules/sigil" "$HOSTILE_HOME"
chmod 0700 "$HOSTILE/modules" "$HOSTILE/modules/sigil"
make_module sigil "$HOSTILE/modules/sigil/sigil.modjash" valid
chmod 0600 "$HOSTILE/modules/sigil/sigil.modjash"
if out=$(cd "$HOSTILE" && HOME="$HOSTILE_HOME" JASH_SIGIL=1 \
         JASH_MODULE_DEV_ROOT= \
         JASH_SIGIL_ALLOW_DEGRADED=0 "$JASH" -c 'echo SHOULD_NOT_RUN' 2>&1); then
    rc=0
else
    rc=$?
fi
if [ "$rc" -eq 125 ] && ! printf '%s' "$out" | grep -q 'SHOULD_NOT_RUN'; then
    ok "automatic Sigil bootstrap ignores hostile CWD modules"
else
    bad "hostile CWD automatic bootstrap" "rc=$rc output=[$out]"
fi

if out=$(cd "$HOSTILE" && HOME="$HOSTILE_HOME" JASH_SIGIL=1 \
         JASH_MODULE_DEV_ROOT="$HOSTILE/modules" "$JASH" \
         -c 'echo DEV_AUTOLOAD_EXPLICIT' 2>&1); then rc=0; else rc=$?; fi
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q 'DEV_AUTOLOAD_EXPLICIT'; then
    ok "explicit development root permits automatic modules"
else
    bad "development-root automatic opt-in" "rc=$rc output=[$out]"
fi

UNTRUSTED_HOME="$TMP/untrusted-installed-home"
mkdir -p "$UNTRUSTED_HOME/.jash/modules"
cp "$HOSTILE/modules/sigil/sigil.modjash" \
   "$UNTRUSTED_HOME/.jash/modules/sigil.modjash"
chmod 0666 "$UNTRUSTED_HOME/.jash/modules/sigil.modjash"
if out=$(HOME="$UNTRUSTED_HOME" JASH_SIGIL=1 "$JASH" \
         -c 'echo UNTRUSTED_RAN' 2>&1); then rc=0; else rc=$?; fi
if [ "$rc" -eq 125 ] && ! printf '%s' "$out" | grep -q 'UNTRUSTED_RAN'; then
    ok "automatic bootstrap rejects writable installed native modules"
else
    bad "automatic installed-module trust" "rc=$rc output=[$out]"
fi

SAFE_HOME="$TMP/safe-without-undo-home"
mkdir -p "$SAFE_HOME/.jash/modules"
chmod 0700 "$SAFE_HOME/.jash" "$SAFE_HOME/.jash/modules"
cp "$HOSTILE/modules/sigil/sigil.modjash" "$SAFE_HOME/.jash/modules/sigil.modjash"
chmod 0600 "$SAFE_HOME/.jash/modules/sigil.modjash"
if out=$(HOME="$SAFE_HOME" JASH_SIGIL=0 JASH_SAFE=1 JASH_MODULE_DEV_ROOT= \
         "$JASH" \
         -c 'echo UNSAFE_COMMAND_RAN' 2>&1); then rc=0; else rc=$?; fi
if [ "$rc" -eq 125 ] && ! printf '%s' "$out" | grep -q 'UNSAFE_COMMAND_RAN' &&
   printf '%s' "$out" | grep -q 'requires trusted undo'; then
    ok "safe mode refuses to run when undo is unavailable"
else
    bad "safe mode undo bootstrap gate" "rc=$rc output=[$out]"
fi

if out=$(HOME="$HOSTILE_HOME" JASH_SIGIL=0 JASH_SAFE=1 \
         JASH_MODULE_DEV_ROOT= \
         JASH_SIGIL_ALLOW_DEGRADED=0 "$JASH" -c 'echo SHOULD_NOT_RUN' 2>&1); then
    rc=0
else
    rc=$?
fi
if [ "$rc" -eq 125 ] && ! printf '%s' "$out" | grep -q 'SHOULD_NOT_RUN'; then
    ok "agent mode chooses SIGN before load and fails closed"
else
    bad "agent Sigil bootstrap gate" "rc=$rc output=[$out]"
fi

# Deterministic verify/execute races. The preload shim can mutate the source
# when cache creation starts, replace the cache path, or modify the verified
# cache inode in place immediately before dlopen.
if [ "$(uname -s)" = Linux ]; then
    cat >"$TMP/swap_dlopen.c" <<'C'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int (*real_open)(const char *, int, ...);
static int (*real_linkat)(int, const char *, int, const char *, int);

static void resolve_open(void)
{
    if (!real_open) *(void **)(&real_open) = dlsym(RTLD_NEXT, "open");
}

static void mutate_source(void)
{
    static int done;
    const char *source = getenv("JMOD_MUTATE_SOURCE");
    if (done || !source) return;
    done = 1;
    resolve_open();
    int fd = real_open(source, O_RDWR);
    unsigned char lenbuf[4], byte;
    if (fd >= 0 && pread(fd, lenbuf, sizeof(lenbuf), 48) == 4) {
        uint32_t manifest_len = (uint32_t)lenbuf[0] |
            ((uint32_t)lenbuf[1] << 8) | ((uint32_t)lenbuf[2] << 16) |
            ((uint32_t)lenbuf[3] << 24);
        off_t body_off = 64 + (off_t)manifest_len;
        if (pread(fd, &byte, 1, body_off) == 1) {
            byte ^= 0xff;
            (void)pwrite(fd, &byte, 1, body_off);
        }
    }
    if (fd >= 0) close(fd);
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    resolve_open();
    if (path && strstr(path, "jmod_sourceprobe_body_")) mutate_source();
    return flags & O_CREAT ? real_open(path, flags, mode) : real_open(path, flags);
}

int linkat(int olddirfd, const char *oldpath, int newdirfd,
           const char *newpath, int flags)
{
    if (!real_linkat) *(void **)(&real_linkat) = dlsym(RTLD_NEXT, "linkat");
    if (newpath && strstr(newpath, "jmod_sourceprobe_body_")) mutate_source();
    return real_linkat(olddirfd, oldpath, newdirfd, newpath, flags);
}

void *dlopen(const char *path, int flags)
{
    static void *(*real_dlopen)(const char *, int);
    if (!real_dlopen) *(void **)(&real_dlopen) = dlsym(RTLD_NEXT, "dlopen");
    const char *cache = getenv("JMOD_RACE_CACHE");
    const char *replacement = getenv("JMOD_RACE_REPLACEMENT");
    const char *mode = getenv("JMOD_RACE_MODE");
    if (cache && mode && path &&
        (strcmp(path, cache) == 0 || strncmp(path, "/proc/self/fd/", 14) == 0)) {
        if (strcmp(mode, "rename") == 0 && replacement) {
            rename(replacement, cache);
        } else if (strcmp(mode, "inplace") == 0) {
            resolve_open();
            (void)chmod(cache, 0700);
            int fd = real_open(cache, O_WRONLY | O_TRUNC);
            if (fd >= 0) {
                (void)write(fd, "not a shared object\n", 20);
                close(fd);
            }
        }
    }
    return real_dlopen(path, flags);
}
C

    if cc -shared -fPIC -o "$TMP/swap_dlopen.so" "$TMP/swap_dlopen.c" -ldl; then
        make_module sourceprobe "$TMP/modules/sourceprobe.modjash" valid
        if out=$(JMOD_MUTATE_SOURCE="$TMP/modules/sourceprobe.modjash" \
                 LD_PRELOAD="$TMP/swap_dlopen.so" \
                 load_module "$TMP/modules/sourceprobe.modjash" 2>&1) &&
           python3 - "$BODY" "$TMP/modules/sourceprobe.modjash" <<'PY'
import struct
import sys
expected = open(sys.argv[1], 'rb').read(1)
raw = open(sys.argv[2], 'rb').read()
manifest_len = struct.unpack_from('<I', raw, 48)[0]
raise SystemExit(0 if raw[64 + manifest_len:65 + manifest_len] != expected else 1)
PY
        then
            ok "source mutation cannot change the verified execution bytes"
        else
            bad "immutable source snapshot" "output=[$out]"
        fi

        make_module raceprobe "$TMP/modules/raceprobe.modjash" valid
        load_module "$TMP/modules/raceprobe.modjash" >/dev/null 2>&1 || true
        cache_path=
        for candidate in "$HOME"/.jash/cache/jmod_raceprobe_body_*.so; do
            [ -f "$candidate" ] && cache_path=$candidate
        done
        printf 'not a shared object\n' >"$TMP/replacement.so"
        if [ -n "$cache_path" ] && out=$(JMOD_RACE_CACHE="$cache_path" \
                 JMOD_RACE_REPLACEMENT="$TMP/replacement.so" \
                 JMOD_RACE_MODE=rename \
                 LD_PRELOAD="$TMP/swap_dlopen.so" \
                 load_module "$TMP/modules/raceprobe.modjash" 2>&1); then
            ok "cache pathname replacement cannot change the execution image"
        else
            bad "verified cache descriptor" "output=[$out]"
        fi

        make_module inplaceprobe "$TMP/modules/inplaceprobe.modjash" valid
        load_module "$TMP/modules/inplaceprobe.modjash" >/dev/null 2>&1 || true
        inplace_cache=
        for candidate in "$HOME"/.jash/cache/jmod_inplaceprobe_body_*.so; do
            [ -f "$candidate" ] && inplace_cache=$candidate
        done
        if [ -n "$inplace_cache" ] && out=$(JMOD_RACE_CACHE="$inplace_cache" \
                 JMOD_RACE_MODE=inplace LD_PRELOAD="$TMP/swap_dlopen.so" \
                 load_module "$TMP/modules/inplaceprobe.modjash" 2>&1) &&
           grep -q 'not a shared object' "$inplace_cache"; then
            ok "in-place cache mutation cannot change the verified execution bytes"
        else
            bad "immutable execution inode" "output=[$out]"
        fi
    else
        bad "TOCTOU test setup" "dlopen interposer unavailable"
    fi
else
    skip "Linux memfd race interposer is not applicable on $(uname -s)"
fi

# Signature framing must be rejected before ed25519_verify, which always reads
# ED25519_SIG bytes. Short and oversized encodings must all fail identically.
assert_bad_sig_len() {
    local sig_len=$1 sig="$TMP/module-$1.sig"
    local file="$TMP/modules/signedlen$1.modjash" out rc
    python3 - "$sig" "$sig_len" <<'PY'
import sys
open(sys.argv[1], 'wb').write(b'\xA5' * int(sys.argv[2]))
PY
    make_module "signedlen$sig_len" "$file" zero 1 "$sig"
    if out=$(load_module "$file" 2>&1); then rc=0; else rc=$?; fi
    if [ "$rc" -ne 0 ] &&
       printf '%s' "$out" | grep -q "invalid sig_len=$sig_len (expected 64)" &&
       ! printf '%s' "$out" | grep -q 'signature verification failed'; then
        ok "signed module with sig_len=$sig_len is rejected before verification"
    else
        bad "signed module sig_len=$sig_len" "rc=$rc output=[$out]"
    fi
}

assert_bad_sig_len 1
assert_bad_sig_len 63
assert_bad_sig_len 65

# Signed modules require a pinned, regular, private, exact-size public key.
python3 - "$TMP/module.seed" <<'PY'
import sys
open(sys.argv[1], 'wb').write(bytes(range(32)))
PY
"$CRYPTO" keypair "$TMP/module.seed" "$TMP/module.pub" "$TMP/module.sec" || \
    prereq_fail "could not derive deterministic signing fixture"
python3 - "$TMP/module-placeholder.sig" <<'PY'
import sys
open(sys.argv[1], 'wb').write(b'\0' * 64)
PY
make_module signedgood "$TMP/modules/signedgood.modjash" valid 9 \
    "$TMP/module-placeholder.sig"
python3 - "$TMP/modules/signedgood.modjash" "$TMP/signed-envelope" <<'PY'
import struct
import sys
raw = open(sys.argv[1], 'rb').read()
manifest_len, body_len = struct.unpack_from('<II', raw, 48)
open(sys.argv[2], 'wb').write(raw[:64 + manifest_len + body_len])
PY
"$CRYPTO" hash "$TMP/signed-envelope" "$TMP/signed-envelope.blake3" || \
    prereq_fail "could not hash signed fixture envelope"
"$CRYPTO" sign "$TMP/signed-envelope.blake3" "$TMP/module.sec" "$TMP/module-good.sig" || \
    prereq_fail "could not sign deterministic fixture"
make_module signedgood "$TMP/modules/signedgood.modjash" valid 9 "$TMP/module-good.sig"

assert_key_rejected() {
    local label=$1 expected=$2 out rc
    if out=$(load_module "$TMP/modules/signedgood.modjash" 2>&1); then rc=0; else rc=$?; fi
    if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -qi "$expected"; then
        ok "$label signing key is rejected before signature verification"
    else
        bad "$label signing key" "rc=$rc output=[$out]"
    fi
}

rm -f "$HOME/.jash/keys/module_signing_pubkey"
assert_key_rejected missing 'trusted signing key'
python3 - "$HOME/.jash/keys/module_signing_pubkey" <<'PY'
import sys
open(sys.argv[1], 'wb').write(b'X' * 31)
PY
chmod 600 "$HOME/.jash/keys/module_signing_pubkey"
assert_key_rejected malformed 'malformed signing key'
cp "$TMP/module.pub" "$HOME/.jash/keys/module_signing_pubkey"
chmod 644 "$HOME/.jash/keys/module_signing_pubkey"
assert_key_rejected insecure 'insecure signing key'
rm -f "$HOME/.jash/keys/module_signing_pubkey"
ln -s "$TMP/module.pub" "$HOME/.jash/keys/module_signing_pubkey"
assert_key_rejected symlink 'symlink signing key'

rm -f "$HOME/.jash/keys/module_signing_pubkey"
cp "$TMP/module.pub" "$HOME/.jash/keys/module_signing_pubkey"
chmod 600 "$HOME/.jash/keys/module_signing_pubkey"

# Legacy signed modules cover only the body. They remain explicit trusted-native
# compatibility loads when strict policy is absent, but strict signature policy
# requires the metadata-binding envelope flag.
python3 - "$TMP/module-legacy.sig" <<'PY'
import sys
open(sys.argv[1], 'wb').write(b'\x5a' * 64)
PY
make_module signedlegacy "$TMP/modules/signedlegacy.modjash" valid 1 \
    "$TMP/module-legacy.sig"
if out=$(load_module "$TMP/modules/signedlegacy.modjash" 2>&1); then rc=0; else rc=$?; fi
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'signature verification failed' &&
   ! printf '%s' "$out" | grep -q 'lacks an envelope signature'; then
    ok "non-strict policy still routes legacy body-only signatures to verification"
else
    bad "legacy signed compatibility path" "rc=$rc output=[$out]"
fi
if out=$(JASH_MODULE_REQUIRE_SIGNATURE=1 \
         load_module "$TMP/modules/signedlegacy.modjash" 2>&1); then
    rc=0
else
    rc=$?
fi
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'lacks an envelope signature'; then
    ok "strict signature mode rejects legacy body-only signatures"
else
    bad "strict legacy signature rejection" "rc=$rc output=[$out]"
fi

# One long-lived shell must not switch its trust root after `export HOME=...`
# or reload a key replaced at the original pathname.
PIN_HOME="$TMP/pin-home"
mkdir -p "$PIN_HOME/.jash/keys"
python3 - "$PIN_HOME/.jash/keys/module_signing_pubkey" <<'PY'
import sys
open(sys.argv[1], 'wb').write(b'Y' * 32)
PY
chmod 600 "$PIN_HOME/.jash/keys/module_signing_pubkey"
if out=$(HOME="$HOME" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 \
         JASH_LOOPGUARD=off "$JASH" -c \
         "export HOME=$PIN_HOME; module load $TMP/modules/signedgood.modjash" 2>&1); then
    ok "module signing trust root remains pinned after HOME changes"
else
    bad "pinned module signing trust root" "output=[$out]"
fi

cp "$PIN_HOME/.jash/keys/module_signing_pubkey" "$TMP/replacement-module-key"
if out=$(HOME="$HOME" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 \
         JASH_LOOPGUARD=off "$JASH" -c \
         "mv $TMP/replacement-module-key $HOME/.jash/keys/module_signing_pubkey; module load $TMP/modules/signedgood.modjash" 2>&1); then
    ok "module signing key bytes remain pinned after pathname replacement"
else
    bad "pinned module signing key bytes" "output=[$out]"
fi
cp "$TMP/module.pub" "$HOME/.jash/keys/module_signing_pubkey"
chmod 600 "$HOME/.jash/keys/module_signing_pubkey"

# Self-declared signedness is mutable. Exact framing rejects a flag-only
# downgrade, while trusted strict policy rejects a fully stripped signature.
cp "$TMP/modules/signedgood.modjash" "$TMP/modules/signed-flag-cleared.modjash"
python3 - "$TMP/modules/signed-flag-cleared.modjash" <<'PY'
import struct, sys
path = sys.argv[1]
raw = bytearray(open(path, 'rb').read())
struct.pack_into('<I', raw, 12, 0)
open(path, 'wb').write(raw)
PY
if out=$(load_module "$TMP/modules/signed-flag-cleared.modjash" 2>&1); then rc=0; else rc=$?; fi
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'invalid unsigned file framing'; then
    ok "clearing only signature flags cannot bypass exact framing"
else
    bad "signature flag-only downgrade" "rc=$rc output=[$out]"
fi

cp "$TMP/modules/signedgood.modjash" "$TMP/modules/signed-stripped.modjash"
python3 - "$TMP/modules/signed-stripped.modjash" <<'PY'
import struct, sys
path = sys.argv[1]
raw = bytearray(open(path, 'rb').read())
manifest_len, body_len = struct.unpack_from('<II', raw, 48)
struct.pack_into('<I', raw, 12, 0)
struct.pack_into('<I', raw, 56, 0)
open(path, 'wb').write(raw[:64 + manifest_len + body_len])
PY
if out=$(JASH_MODULE_REQUIRE_SIGNATURE=1 \
         load_module "$TMP/modules/signed-stripped.modjash" 2>&1); then
    rc=0
else
    rc=$?
fi
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'JASH_MODULE_REQUIRE_SIGNATURE'; then
    ok "trusted signature policy rejects fully stripped signed framing"
else
    bad "full signature downgrade under strict policy" "rc=$rc output=[$out]"
fi
if out=$(JASH_MODULE_REQUIRE_SIGNATURE=1 HOME="$HOME" JASH_SIGIL=0 \
         JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off "$JASH" -c \
         "unset JASH_MODULE_REQUIRE_SIGNATURE; module load $TMP/modules/signed-stripped.modjash" \
         2>&1); then
    rc=0
else
    rc=$?
fi
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'JASH_MODULE_REQUIRE_SIGNATURE'; then
    ok "signature policy remains pinned after shell environment mutation"
else
    bad "mutable signature policy" "rc=$rc output=[$out]"
fi

if [ "$(uname -s)" = Linux ]; then
    if out=$(load_module "$TMP/modules/signedgood.modjash" 2>&1); then
        ok "valid signature loads with configured module_signing_pubkey"
    else
        bad "valid signed module" "output=[$out]"
    fi
elif [ "$(uname -s)" = Darwin ]; then
    if out=$(load_module "$TMP/modules/signedgood.modjash" 2>&1); then rc=0; else rc=$?; fi
    if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'immutable execution image unavailable' &&
       ! printf '%s' "$out" | grep -q 'signature verification failed'; then
        ok "valid macOS signature reaches the immutable-image fail-closed gate"
    else
        bad "valid macOS signed module" "rc=$rc output=[$out]"
    fi
else
    skip "valid signed load has no immutable execution backend on $(uname -s)"
fi

# The signature covers manifest policy metadata, not only the shared object.
cp "$TMP/modules/signedgood.modjash" "$TMP/modules/signedtampered.modjash"
python3 - "$TMP/modules/signedtampered.modjash" <<'PY'
import sys
path = sys.argv[1]
raw = bytearray(open(path, 'rb').read())
needle = b'description=module loader regression probe'
offset = raw.index(needle)
raw[offset + len(b'description=')] ^= 1
open(path, 'wb').write(raw)
PY
if out=$(load_module "$TMP/modules/signedtampered.modjash" 2>&1); then rc=0; else rc=$?; fi
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'signature verification failed'; then
    ok "valid body signature rejects tampered manifest metadata"
else
    bad "signed manifest coverage" "output=[$out]"
fi

# A well-framed invalid signature must still fail after the trusted key loads.
python3 - "$TMP/module-bad.sig" <<'PY'
import sys
open(sys.argv[1], 'wb').write(b'\xA5' * 64)
PY
make_module signedbad "$TMP/modules/signedbad.modjash" valid 9 "$TMP/module-bad.sig"
if out=$(load_module "$TMP/modules/signedbad.modjash" 2>&1); then rc=0; else rc=$?; fi
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q 'signature verification failed'; then
    ok "invalid signed module remains rejected"
else
    bad "invalid signed module" "output=[$out]"
fi

printf '\n=== JINV-013: %d ok / %d fail / %d skip ===\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
