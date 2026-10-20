#!/usr/bin/env bash
# JINV-001 / JINV-002 — o build tem que poder FALHAR.
#
# JINV-001: `ninja ... | tail -1` devolvia o status do `tail` (sempre 0). Um
#           build QUEBRADO saía com 0, o `@echo BUILD` rodava assim mesmo, e o
#           binário ANTERIOR continuava no disco — tornando todo "passou nos
#           testes" infalsificável.
# JINV-002: o Makefile forçava `-G Ninja` enquanto o CLAUDE.md documenta
#           `cmake -B build` (gerador default). Um caminho poluía o outro e o
#           `2>/dev/null` escondia o erro do cmake.
#
# ARMADILHA REGISTRADA: a correção "óbvia" (`.SHELLFLAGS := -o pipefail -c`)
# NÃO funciona no macOS — GNU Make 3.81 (2006) ignora `.SHELLFLAGS` em
# silêncio. Este teste pegou exatamente esse fix-que-não-aplicava. Se alguém
# reintroduzir `.SHELLFLAGS` achando que resolve, o teste 1 reprova de novo.
#
# Este teste testa COMPORTAMENTO (o build falha de verdade), não mecanismo.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)" || exit 2
# Exercise the real Makefile against the current source bytes, including local
# edits, without changing the checkout or replacing its qualified binaries.
# Copy no CMake cache: its absolute source/build paths belong to the caller.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/jinv001.XXXXXX")" || exit 2
trap 'rm -rf "$WORK"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
SOURCE="$WORK/source"
python3 - "$ROOT" "$SOURCE" <<'PY' || exit 2
import os
from pathlib import Path
import shutil
import subprocess
import sys

root, destination = map(Path, sys.argv[1:])
destination.mkdir()
names = subprocess.check_output(
    ['git', '-C', str(root), 'ls-files', '-z', '--cached', '--others', '--exclude-standard'])
for encoded in sorted(set(names.split(b'\0'))):
    if not encoded:
        continue
    relative = Path(os.fsdecode(encoded))
    top = relative.parts[0]
    if (top == 'build' or top.startswith(('build-', 'cmake-build-')) or
            'CMakeFiles' in relative.parts or relative.name == 'CMakeCache.txt'):
        continue
    source = root / relative
    if not source.exists() and not source.is_symlink():
        continue  # Preserve a working-tree deletion instead of restoring HEAD.
    target = destination / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target, follow_symlinks=False)
assert (destination / 'Makefile').is_file()
assert (destination / 'c/boot.c').is_file()
PY
cd "$SOURCE" || exit 2

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

VICTIM="c/boot.c"
BACKUP="$WORK/boot.c.original"
cp "$VICTIM" "$BACKUP" || exit 2
restore() { cp "$BACKUP" "$VICTIM" || exit 2; }

# ── 1. Um build QUEBRADO derruba o make (o teste central) ───────────────────
printf '\n#error JINV001_PROBE_DELIBERADO\n' >> "$VICTIM"
if make build >/dev/null 2>&1; then
    bad "build quebrado derruba o make" \
        "make saiu 0 com um #error no fonte — o binario velho ficou no disco"
else
    ok "build quebrado derruba o make"
fi

# ── 2. O erro do compilador fica VISIVEL (nao engolido) ────────────────────
if make build 2>&1 | grep -q "JINV001_PROBE_DELIBERADO"; then
    ok "erro do compilador aparece na saida do make"
else
    bad "erro do compilador visivel" \
        "o #error nao apareceu — a saida esta indo para /dev/null?"
fi
restore

# ── 3. Falha de configure tambem e propagada ───────────────────────────────
if make build CMAKE=false >/dev/null 2>&1; then
    bad "falha de configure derruba o build" "make saiu 0 com cmake quebrado"
else
    ok "falha de configure derruba o build"
fi

# ── 4. Os dois caminhos de build coexistem (JINV-002) ──────────────────────
# O Makefile nao pode forcar um gerador que conflite com o cache existente.
if grep -qE '^\s*@?\$\(CMAKE\)\s+-G' Makefile; then
    bad "Makefile nao forca gerador" \
        "'-G' fixo na receita volta a conflitar com o cache do CLAUDE.md"
else
    ok "Makefile nao forca gerador fixo (coexiste com cmake -B build)"
fi

# ── 5. O build normal funciona e produz binario executavel ─────────────────
make build >/dev/null 2>&1
./build/jash -c 'exit 7' >/dev/null 2>&1
if [ $? -eq 7 ]; then
    ok "build normal produz um binario funcional"
else
    bad "build normal" "make build falhou ou o binario nao propaga exit code"
fi

echo ""
echo "=== JINV-001/002: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
