#!/usr/bin/env bash
# W7 — nenhum executavel ou objeto compartilhado pode estar no indice do git.
#
# Custo ja pago: um Mach-O de macOS versionado em build-speed/ que tres gates
# preferiam a build/jash (rc 126), um "hang" fantasma em test_jinv132 que era o
# Mach-O sendo executado, e o install_verify.sh a um passo de instala-lo como
# shell do sistema. Binario versionado e a unica classe de arquivo que fica
# ERRADA ao ser copiada entre maquinas sem ninguem perceber.
cd "$(dirname "$0")/.." || exit 2
bad=$(python3 - <<'PY'
import subprocess, os
MAGIC = (b'\x7fELF', b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe')
files = subprocess.run(['git','ls-files'],capture_output=True,text=True).stdout.split('\n')
for f in files:
    if not f or not os.path.isfile(f): continue
    try:
        if open(f,'rb').read(4)[:4] in MAGIC: print(f)
    except Exception: pass
PY
)
if [ -z "$bad" ]; then echo "ok  nenhum binario rastreado"; exit 0; fi
echo "FAIL  binarios no indice do git:"; printf '  %s\n' $bad
echo "  Remova com: git rm --cached <arquivo>  (o arquivo continua no disco)"
exit 1
