#!/usr/bin/env bash
# JINV-068 — o MESMO programa tem que rodar igual pelo arquivo e pelo stdin.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
# Um programa é um programa, não importa por qual porta entrou. `jash prog.sh`,
# `jash < prog.sh` e `cat prog.sh | jash` têm que produzir a mesma saída e o
# mesmo rc. Se as duas portas divergem, uma delas tem um leitor próprio — e
# duas implementações da mesma coisa é a doença estrutural que esta auditoria
# encontrou em todo lugar (4 casadores de glob, 7 rtrims cegos, 2 funções irmãs
# onde só uma tinha guarda).
#
# ═══ O DEFEITO ═══
#
# O leitor de stdin era orientado a LINHA: lia uma linha e mandava avaliar.
# O de arquivo acumulava o programa inteiro e entregava a jash_run_multiline,
# que sabe juntar construções compostas. Resultado, com o MESMO programa:
#
#   printf 'if true\nthen\necho OK\nfi\n' > /tmp/s.sh
#   jash /tmp/s.sh          → OK             (rc 0)
#   printf … | jash         → syntax error   (rc 2)
#
# Toda construção multi-linha — if, for, while, until, case, função, heredoc —
# quebrava via pipe. Isso é `curl … | sh`, que é como metade do mundo instala
# software. Custo medido no corpus do Oils: 148 casos, ~5 pontos percentuais
# (audits/BENCHMARK-compatibilidade-externa.md §3.4).
#
# ═══ COMO ESTE GATE MEDE ═══
#
# `/bin/bash` é o ORÁCULO. Nenhuma saída esperada é escrita à mão: para cada
# programa, roda-se o MESMO texto em bash e em jash, pelas DUAS portas, e
# comparam-se as três coisas — jash-arquivo vs bash, jash-stdin vs bash, e
# jash-arquivo vs jash-stdin. A terceira é a que mata o defeito: ela não
# depende de o jash estar certo, só de ele ser COERENTE consigo mesmo.
#
# Gate: bash tests/test_jinv068_stdin_multiline.sh

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
BASH_BIN="${BASH_BIN:-/bin/bash}"
[ -x "$JASH" ]     || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$BASH_BIN" ] || { echo "SKIP: oráculo $BASH_BIN ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

H=$(mktemp -d) || exit 2
W=$(mktemp -d) || exit 2
trap 'rm -rf "$H" "$W"' EXIT INT TERM
DL="$PWD/tools/deadline.sh"

# Roda $2 (o texto do programa) no shell $1, pela porta $3 (file|stdin).
# Deixa stdout em $W/out e devolve o rc por echo. Sem pipe na captura do rc:
# o `$?` de um pipeline é o do último elo, nunca o do shell sob teste.
run_prog() {
    local shell="$1" prog="$2" porta="$3" rc
    printf '%s' "$prog" > "$W/prog.sh"
    if [ "$porta" = file ]; then
        HOME="$H" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off \
            "$DL" 15 "$shell" "$W/prog.sh" >"$W/out" 2>"$W/err" </dev/null
        rc=$?
    else
        HOME="$H" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off \
            "$DL" 15 "$shell" >"$W/out" 2>"$W/err" <"$W/prog.sh"
        rc=$?
    fi
    echo "$rc"
}

# Uma construção por caso. O nome é o que o programa exercita.
caso() {
    local nome="$1" prog="$2"

    local brc bout jfrc jfout jsrc jsout
    brc=$(run_prog "$BASH_BIN" "$prog" file);  bout=$(cat "$W/out")
    jfrc=$(run_prog "$JASH" "$prog" file);     jfout=$(cat "$W/out")
    jsrc=$(run_prog "$JASH" "$prog" stdin);    jsout=$(cat "$W/out")

    # O oráculo precisa estar de pé; um caso que o bash não roda não mede nada.
    if [ "$brc" != "0" ] || [ -z "$bout" ]; then
        bad "oráculo de '$nome'" "bash rc=$brc out='$bout' — caso inválido, corrija o programa"
        return
    fi

    # (a) a porta que já funcionava continua funcionando
    if [ "$jfout" != "$bout" ] || [ "$jfrc" != "$brc" ]; then
        bad "'$nome' por ARQUIVO" \
            "jash out='$jfout' rc=$jfrc ≠ bash out='$bout' rc=$brc (regressão na porta antiga)"
        return
    fi
    # (b) a porta consertada bate com o bash
    if [ "$jsout" != "$bout" ] || [ "$jsrc" != "$brc" ]; then
        bad "'$nome' por STDIN" \
            "jash out='$jsout' rc=$jsrc ≠ bash out='$bout' rc=$brc — o leitor de stdin quebrou a construção"
        return
    fi
    # (c) e as duas portas do jash concordam entre si — a asserção que não
    #     depende de o jash estar certo, só de ser coerente
    if [ "$jfout" != "$jsout" ] || [ "$jfrc" != "$jsrc" ]; then
        bad "'$nome' arquivo≡stdin" \
            "arquivo out='$jfout' rc=$jfrc  ≠  stdin out='$jsout' rc=$jsrc"
        return
    fi
    ok "'$nome': arquivo ≡ stdin ≡ bash (out='$bout' rc=$brc)"
}

# ── as construções multi-linha, uma a uma ───────────────────────────────
caso "if/then/fi"          'if true
then
echo OK
fi
'
caso "if/else"             'if false
then
echo NAO
else
echo ELSE
fi
'
caso "if/elif"             'if false
then
echo A
elif true
then
echo ELIF
fi
'
caso "for/do/done"         'for i in 1 2 3
do
echo n=$i
done
'
caso "while/do/done"       'i=0
while [ $i -lt 2 ]
do
echo w=$i
i=$((i+1))
done
'
caso "until/do/done"       'i=0
until [ $i -ge 2 ]
do
echo u=$i
i=$((i+1))
done
'
caso "case/esac"           'case abc in
abc)
echo MATCH
;;
esac
'
caso "definicao de funcao" 'f() {
echo DENTRO
}
f
'
caso "heredoc"             'cat <<FIM
corpo
FIM
'
caso "aninhado if+for"     'for i in 1 2
do
if [ $i = 2 ]
then
echo achou=$i
fi
done
'
caso "continuacao com \\"  'echo A\
B
'

# ── linhas simples: o caminho antigo não pode ter regredido ─────────────
caso "uma linha"           'echo A
'
caso "duas linhas"         'echo A
echo B
'
caso "comentario no meio"  'echo A
# comentario
echo B
'

# ── o rc do programa vem do último comando, e `exit N` vale ─────────────
prog='echo A
exit 3
echo NAO
'
brc=$(run_prog "$BASH_BIN" "$prog" stdin); bout=$(cat "$W/out")
jsrc=$(run_prog "$JASH" "$prog" stdin);    jsout=$(cat "$W/out")
if [ "$brc" = "0" ]; then
    bad "oráculo do exit" "bash devolveu 0 para um script com 'exit 3' — caso inválido"
elif [ "$jsrc" = "$brc" ] && [ "$jsout" = "$bout" ]; then
    ok "'exit N' via stdin encerra e propaga o rc (bash e jash: rc=$brc out='$bout')"
else
    bad "'exit N' via stdin" "jash rc=$jsrc out='$jsout' ≠ bash rc=$brc out='$bout'"
fi

# ── um programa GRANDE não pode ser executado pela metade (JINV-007) ────
# A mesma disciplina do leitor de arquivo tem que valer no stdin: o buffer
# cresce, e nada é truncado em silêncio. 4000 linhas passam de 64 KiB.
{
    i=0
    while [ "$i" -lt 4000 ]; do printf 'x=%s\n' "$i"; i=$((i+1)); done
    printf 'echo FIM=$x\n'
} > "$W/grande.txt"
prog=$(cat "$W/grande.txt")
brc=$(run_prog "$BASH_BIN" "$prog" stdin); bout=$(cat "$W/out")
jsrc=$(run_prog "$JASH" "$prog" stdin);    jsout=$(cat "$W/out")
if [ "$bout" != "FIM=3999" ]; then
    bad "oráculo do programa grande" "bash out='$bout' — caso inválido"
elif [ "$jsout" = "$bout" ]; then
    ok "programa grande via stdin chega ao FIM (out='$bout') — nada truncado"
else
    bad "programa grande via stdin" \
        "jash out='$jsout' rc=$jsrc ≠ bash out='$bout' — programa truncado ou parcial"
fi

# ── `-s` (ler de stdin, explícito no POSIX) toma o mesmo caminho ────────
prog='if true
then
echo SOK
fi
'
printf '%s' "$prog" > "$W/prog.sh"
bout=$(HOME="$H" "$DL" 15 "$BASH_BIN" -s <"$W/prog.sh" 2>/dev/null)
jsout=$(HOME="$H" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 \
        "$DL" 15 "$JASH" -s <"$W/prog.sh" 2>/dev/null)
if [ "$bout" != "SOK" ]; then
    bad "oráculo do -s" "bash -s out='$bout' — caso inválido"
elif [ "$jsout" = "$bout" ]; then
    ok "'-s' (stdin explícito) roda a construção multi-linha como o bash"
else
    bad "'-s' multi-linha" "jash='$jsout' ≠ bash='$bout'"
fi

echo ""
echo "=== JINV-068: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
