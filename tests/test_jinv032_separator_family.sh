#!/usr/bin/env bash
# JINV-032 — a família inteira de varreduras de separador, contra o bash.
#
# O repositório tem NOVE implementações distintas de "procurar separador
# respeitando aspas" (`eval.c:206`, `pipeline.c:739`, `pipeline.c:1857`,
# `builtin_explain.c:87`, `pipeline.c:191`, `builtin_redir.c:97`,
# `jash_liquid.c:658`, `fschange.c:341`, `posix_parser.c:81`), com graus
# diferentes de correção. O JINV-020 consertou uma delas.
#
# Em vez de refatorar as outras oito às cegas, este gate MEDE: uma tabela de
# casos-armadilha rodada contra as entradas observáveis de cada varredura,
# usando o **bash como oráculo**. O resultado da medição foi que oito já
# estavam corretas na prática (algumas por ordem de execução — a expansão roda
# antes delas, então recebem texto já com as aspas balanceadas), e exatamente
# UMA divergia:
#
#   case x in x) echo "a;;b";; esac
#     bash → a;;b        jash → "a
#
# O `;;` que fecha o ramo era procurado com `strncmp(p, ";;", 2)` puro, sem
# rastreio de citação. Mesma classe do JINV-016, do JINV-020 e do JINV-028: um
# caractere que era DADO virava estrutura. Aqui o estrago é duplo — o comando
# executa truncado E o resto do texto vira o próximo ramo do `case`.
#
# Este teste existe para que a família não volte a divergir. Ele não afirma
# que as nove cópias foram unificadas (não foram); afirma que o COMPORTAMENTO
# das nove bate com o bash, que é a propriedade que interessa a quem usa.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
# Avisos de UX (nudge/adoption/loopguard) vao para o STDERR — o que esta
# certo. Silencia-los aqui evita comparar ruido de interface com saida de
# programa; ja quebrou este gate uma vez.
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
command -v bash >/dev/null || { echo "SKIP: sem bash de referência"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM

cmp_bash() { # cmp_bash <rotulo> <comando>
    local gb gj
    gb=$(bash   -c "$2" 2>/dev/null | head -3 | tr '\n' '|')
    gj=$("$JASH" -c "$2" 2>/dev/null | head -3 | tr '\n' '|')
    if [ "$gb" = "$gj" ]; then
        ok "$1  → $gj"
    else
        bad "$1" "bash='$gb'  jash='$gj'"
    fi
}

# ── find_top_separator (JINV-020) ────────────────────────────────────────
cmp_bash "';' em \$( ) dentro de aspas"   'echo "[$(echo "a;b")]"'
cmp_bash "'&&' em \$( ) dentro de aspas"  'echo "[$(echo "a&&b")]"'

# ── split_pipeline — o '|' ───────────────────────────────────────────────
cmp_bash "'|' literal em \$( )"           'echo "[$(echo "a|b")]"'
cmp_bash "'|' em aspas + pipe real"       'echo "[$(echo "a|b")]" | tr a A'
cmp_bash "'|' vindo de variável"          'V="x|y"; echo "$V" | tr x X'

# ── parse_redirs / builtin_redir — '>' e '<' ────────────────────────────
cmp_bash "'>' literal em \$( )"           'echo "[$(echo "a>b")]"'
cmp_bash "'<' literal em \$( )"           'echo "[$(echo "a<b")]"'
cmp_bash "'>>' dentro de aspas"           'echo "x>>y"'

# ── line_has_op — corpo de heredoc é dado ──────────────────────────────
cmp_bash "'|' em corpo de heredoc"        'cat << '"'"'EOF'"'"'
a|b
EOF'
cmp_bash "'>' em corpo de heredoc"        'cat << '"'"'EOF'"'"'
a>b
EOF'

# ── stripper de comentário ─────────────────────────────────────────────
cmp_bash "'#' literal em \$( )"           'echo "[$(echo "a#b")]"'
cmp_bash "'#' dentro de aspas"            'echo "tem # dentro"'

# ── aninhamentos mais fundos ───────────────────────────────────────────
cmp_bash "\$( ) aninhado duplo"           'echo "[$(echo "$(echo "p;q")")]"'
cmp_bash "crase dentro de aspas com ';'"  'echo "[`echo "a;b"`]"'
cmp_bash "\${} com ';' no valor"          'V="a;b"; echo "[${V}]"'

# ── case: o ';;' — a divergência que este invariante matou ─────────────
cmp_bash "';;' dentro de aspas no case"   'case x in x) echo "a;;b";; esac'
cmp_bash "';' dentro de aspas no case"    'case x in x) echo "a;b";; esac'
cmp_bash "\$( ) com ';' no corpo do case" 'case x in x) echo "[$(echo "p;q")]";; esac'
cmp_bash "case de dois ramos intacto"     'case y in x) echo um;; y) echo dois;; esac'
cmp_bash "case com pipe no ramo"          'case x in x) echo abc | tr a-z A-Z;; esac'

# ── e os separadores REAIS continuam separando ─────────────────────────
cmp_bash "';' de verdade separa"          'echo um; echo dois'
cmp_bash "'&&' de verdade"                'true && echo depois'
cmp_bash "pipeline de verdade"            'echo x | tr x y'
cmp_bash "redirect de verdade"            "echo conteudo > $T/r.txt; cat $T/r.txt"

echo ""
echo "=== JINV-032: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
