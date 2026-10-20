#!/usr/bin/env bash
# JINV-036 — o que vem DEPOIS de `fi`/`esac` tem que executar.
#
# O bug mais caro desta campanha em impacto cotidiano.
#
# `jash_eval_line` chamava o tratador de bloco e fazia `return rc` na hora,
# descartando o resto da string. FORA de uma função isso nunca aparecia: o
# driver de script alimenta uma linha lógica por vez, então o comando seguinte
# chegava como chamada separada. DENTRO de uma função o corpo inteiro vai numa
# string só — e tudo depois do `fi` sumia, em silêncio:
#
#   setup() {
#     if [ ! -d "$D" ]; then mkdir "$D"; fi
#     cp arquivo "$D"        ← NUNCA EXECUTAVA
#   }
#
# A função fazia metade do trabalho e devolvia sucesso.
#
# Medido contra o bash: `if` e `case` sofriam; `for`, `while` e `until` NÃO —
# eles já rodavam o resto (`for_loop.c:253`, o idioma que este conserto copia).
# **Três tratadores certos e dois errados, e nenhum gate cobrava a diferença.**
#
# Isto também explicava por que TODA recursão de função parecia quebrada: a
# chamada recursiva vem depois do `if` de caso-base, então nunca acontecia. O
# que parecia "recursão não funciona" era este bug.
#
# ACHADO ABERTO, medido e NÃO corrigido — `fi` dentro de aspas:
#
#   if [ 0 -le 0 ]; then echo "diz fi aqui"; fi
#     bash → diz fi aqui        jash → `"diz aqui"; fi: command not found`
#
# O coletor de texto de `tokenize_if_block` (if_block.c) pula `[[ ]]` mas não
# pula strings citadas, então um `fi`/`then`/`else` dentro de um literal
# encerra o bloco — e a palavra é ENGOLIDA do texto. É pré-existente (o binário
# instalado tem), e vale dentro E fora de função. O `case` não sofre.
#
# Tentei a correção óbvia (pular aspas antes de casar palavra-chave) e ela
# TRAVOU o shell — laço infinito no tokenizador. Revertida: um hang é
# infinitamente pior que a saída errada. Fica registrada aqui e na lista de
# tarefas em vez de ser afirmada como resolvida.
#
# O teto de recursão de 32 NÃO é parte deste defeito: ele é um piso de
# segurança deliberado, calibrado empiricamente (o crash real fica entre 50 e
# 80 níveis), e o próprio código documenta a dívida — encolher a pilha por
# ciclo antes de subir o teto.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
command -v bash >/dev/null || { echo "SKIP: sem bash de referência"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: sem python3"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM

roda() { # roda <nome> <conteudo-do-script>
    printf '%s' "$2" > "$T/$1.sh"
    local gj gb
    # JASH_LOOPGUARD=off: o aviso do loopguard e ADVISORY e vai no stderr;
    # captura-lo aqui compararia ruido de UX com saida de programa.
    gj=$(JASH_LOOPGUARD=off JASH_NUDGE=0 "$JASH" "$T/$1.sh" 2>&1 | tr '\n' ' ')
    gb=$(bash    "$T/$1.sh" 2>&1 | tr '\n' ' ')
    if [ "$gj" = "$gb" ]; then
        ok "$1  → [$gj]"
    else
        bad "$1" "jash=[$gj]  bash=[$gb]"
    fi
}

# ── 1. o caso central: comando depois de `fi` dentro de função ───────────
roda "if_falso"   'f() {
  if [ 1 -le 0 ]; then echo A; fi
  echo DEPOIS
}
f
'
roda "if_verdade" 'f() {
  if [ 0 -le 0 ]; then echo A; fi
  echo DEPOIS
}
f
'
roda "if_else"    'f() {
  if [ 1 -le 0 ]; then echo A; else echo B; fi
  echo DEPOIS
}
f
'
roda "if_duas"    'f() {
  if [ 1 -le 0 ]; then echo A; fi
  echo UM
  echo DOIS
}
f
'
roda "if_aninhado" 'f() {
  if [ 0 -le 0 ]; then
    if [ 1 -le 0 ]; then echo X; fi
    echo INTERNO
  fi
  echo DEPOIS
}
f
'

# ── 2. `case` sofria do mesmo ─────────────────────────────────────────────
roda "case_depois" 'f() {
  case x in x) echo A;; esac
  echo DEPOIS
}
f
'

# ── 3. os que JÁ funcionavam não podem regredir ──────────────────────────
roda "for_depois"   'f() {
  for i in 1 2; do echo $i; done
  echo DEPOIS
}
f
'
roda "while_depois" 'f() {
  n=0
  while [ $n -lt 2 ]; do n=$((n+1)); done
  echo DEPOIS
}
f
'
roda "until_depois" 'f() {
  n=0
  until [ $n -ge 2 ]; do n=$((n+1)); done
  echo DEPOIS
}
f
'

# ── 4. recursão volta a funcionar (era vítima do mesmo bug) ─────────────
roda "recursao"     'f() {
  if [ "$1" -le 0 ]; then echo FIM; return 0; fi
  f $(( $1 - 1 ))
}
f 5
'
roda "recursao_var" 'f() {
  if [ "$1" -le 0 ]; then echo FIM; return 0; fi
  n=$(( $1 - 1 ))
  f $n
}
f 5
'

# ── 5. `return` ainda interrompe (a cauda NÃO pode rodar depois) ────────
roda "return_para"  'f() {
  if [ 0 -le 0 ]; then echo A; return 0; fi
  echo NAO_DEVE_SAIR
}
f
'

# ── 6. fora de função continua igual ────────────────────────────────────
roda "fora_if"      'if [ 1 -le 0 ]; then echo A; fi
echo DEPOIS
'
roda "fora_case"    'case x in x) echo A;; esac
echo DEPOIS
'

# ── 7. `esac` dentro de aspas é DADO (o `case` acerta) ─────────────────
roda "esac_aspas"   'f() {
  case x in x) echo "diz esac aqui";; esac
  echo DEPOIS
}
f
'

echo ""
echo "=== JINV-036: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
