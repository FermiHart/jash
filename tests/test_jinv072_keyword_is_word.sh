#!/usr/bin/env bash
# JINV-072 — palavra-chave e PALAVRA; separador dentro de aspas ou de parenteses nao separa.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
#   O leitor de bloco so termina um bloco numa palavra-chave que seja uma
#   PALAVRA INTEIRA e esteja FORA de aspas. E so quebra um enunciado num `;`
#   ou nova-linha que esteja fora de aspas E fora de parenteses.
#
# ═══ AS DUAS METADES, E COMO CADA UMA FAZIA DADO VIRAR COMANDO ═══
#
# (1) BUSCA POR SUBSTRING. `c/parser/script.c` procurava o terminador com
#     `strstr(p, " fi")`. `" fi"` casa DENTRO de `" first;"`, `" file"`,
#     `" fine"`, `" filter"`, `" final"`:
#
#         $ if true; then echo first; fi
#         bash -> first
#         jash -> (nada)  +  rst;: command not found
#
#     O corpo NAO rodava e o RABO da palavra `first` era executado como
#     comando. Tres agentes diagnosticaram isto como "if de uma linha trava".
#     A mesma busca cega tratava `echo "quoted fi"` como fim de bloco, e o
#     ponteiro do "resto" (`fi_pos + 2`) caia no meio da palavra.
#
# (2) DIVISOR DE ENUNCIADO CEGO. `split_statements` (c/parser/if_block.c)
#     contava `if`/`case`/`for` mas nao sabia o que e aspa nem parentese, e
#     quebrava num `;` dentro dos dois:
#
#         if (FOO=FOO; unset FOO) >/dev/null 2>&1; then ...
#         bash -> roda o subshell
#         jash -> `(FOO=FOO: command not found`
#
#     Essa e a LINHA 62 de todo `configure` de autoconf. Idem `echo "a;b"`.
#
# As duas sao a MESMA doenca do JINV-016/017/018/019: uma fronteira lida por
# texto cru, e o dado do usuario atravessando para o lado de comando. E a
# unica leitura correta e a CONSCIENTE DE ASPAS — a mesma que as outras
# portas desta casa ja usam.
#
# ═══ O QUE MAIS ESTE GATE PEGA ═══
#
# `if true; then echo hi; fi; echo tail` imprimia `tail` DUAS VEZES: com o
# `fi` casado no lugar errado, a cauda ganhava dois donos. Uma cauda que roda
# duas vezes num script de instalacao nao e um erro de saida — e um efeito
# colateral repetido.
#
# Oraculo: /bin/bash, em TODA assercao.
# Prova negativa: JASH_UNDER_TEST=<binario anterior ao conserto> -> secoes (a)/(b) VERMELHAS.
#
# Codigo: c/parser/script.c   (find_keyword — busca por palavra, com aspas)
#         c/parser/if_block.c (split_statements — aspas + profundidade de parentese)

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="${JASH_UNDER_TEST:-$PWD/build/jash}"
DL="$PWD/tools/deadline.sh"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

T=$(mktemp -d) || exit 2
H=$(mktemp -d) || exit 2
trap 'rm -rf "$T" "$H"' EXIT INT TERM

# Cao de guarda em TODA porta — inclusive as de preparo. NUNCA `timeout`:
# nao existe nesta maquina, devolve 127 sob bash, e faz o teste concluir
# "nao travou" exatamente quando travou.
cmp_c() {   # cmp_c <nome> <comando>   (via -c, uma linha)
    local name="$1" cmd="$2"
    local bo bc jo jc
    bo=$(cd "$T" && HOME="$H" "$DL" 15 /bin/bash -c "$cmd" 2>/dev/null </dev/null); bc=$?
    jo=$(cd "$T" && HOME="$H" "$DL" 15 "$JASH"   -c "$cmd" 2>/dev/null </dev/null); jc=$?
    if [ "$bo" = "$jo" ] && [ "$bc" = "$jc" ]; then
        ok "$name"
    else
        bad "$name" "bash(rc=$bc)=[$bo]  jash(rc=$jc)=[$jo]"
    fi
}

# Compara stdout, rc — E EXIGE stderr vazio onde o do bash e vazio.
#
# Sem a parte do stderr esta funcao seria cega justamente para o caso do
# subshell na condicao: o `(FOO=FOO: command not found` ia para o stderr, o
# `[unset]` do stdout coincidia por acidente, e a assercao passava VERDE com
# o bug presente. Foi medido: com o binario anterior ao conserto ela passava.
cmp_file() {   # cmp_file <nome> <corpo-do-script>
    local name="$1" body="$2"
    printf '%s' "$body" > "$T/c.sh"
    local bo bc jo jc be je
    bo=$(cd "$T" && HOME="$H" "$DL" 15 /bin/bash ./c.sh 2>"$T/be" </dev/null); bc=$?
    jo=$(cd "$T" && HOME="$H" "$DL" 15 "$JASH"   ./c.sh 2>"$T/je" </dev/null); jc=$?
    be=$(wc -c < "$T/be" | tr -d ' '); je=$(wc -c < "$T/je" | tr -d ' ')
    if [ "$bo" != "$jo" ] || [ "$bc" != "$jc" ]; then
        bad "$name" "bash(rc=$bc)=[$bo]  jash(rc=$jc)=[$jo]"
    elif [ "$be" = "0" ] && [ "$je" != "0" ]; then
        bad "$name (stderr)" "bash calou, jash reclamou: $(head -c 120 "$T/je")"
    else
        ok "$name"
    fi
}

echo "── (a) 'fi' dentro de uma palavra NAO fecha o bloco ───────────────────"

# Cada uma destas imprimia nada e executava o rabo da palavra como comando.
cmp_c "corpo com 'first'"  'if true; then echo first; fi'
cmp_c "corpo com 'fine'"   'if true; then echo fine; fi'
cmp_c "corpo com 'file'"   'if true; then echo file; fi'
cmp_c "corpo com 'filter'" 'if true; then echo filter; fi'
cmp_c "ramo else com 'first'" 'if false; then echo A; else echo first; fi'
cmp_c "corpo com 'thence' (mesma doenca em 'then')" 'if true; then echo thence; fi'
cmp_c "corpo com 'elsewhere' (mesma doenca em 'else')" 'if true; then echo elsewhere; fi'

echo "── (b) palavra-chave entre aspas e DADO ───────────────────────────────"

cmp_c 'aspas duplas: echo "quoted fi"' 'if true; then echo "quoted fi"; fi'
cmp_c "aspas simples: echo 'fi'"       "if true; then echo 'fi'; fi"
cmp_c 'aspas duplas com then'          'if true; then echo "a then b"; fi'
cmp_c 'aspas duplas com else'          'if true; then echo "a else b"; fi'

echo "── (c) a cauda depois do 'fi' roda UMA vez ────────────────────────────"

cmp_c "cauda simples"           'if true; then echo hi; fi; echo tail'
cmp_c "cauda com corpo em 'fi'" 'if true; then echo first; fi; echo tail'

echo "── (d) ';' dentro de parenteses ou aspas nao quebra o enunciado ───────"

# A linha 62 de TODO configure de autoconf.
cmp_file "subshell como condicao de um if multi-linha (linha 62 do autoconf)" \
'if (FOO=FOO; unset FOO) >/dev/null 2>&1; then
  r=unset
else
  r=false
fi
echo "[$r]"
'

cmp_file "';' dentro de aspas no corpo do bloco" \
'if true; then
echo "a;b"
fi
'

cmp_file "';' dentro de aspas simples no corpo do bloco" \
"if true; then
echo 'a;b'
fi
"

cmp_file "subshell no CORPO do bloco" \
'if true; then
(echo one; echo two)
fi
'

echo "── (e) o que ja funcionava continua funcionando ───────────────────────"

cmp_c "if/else simples"      'if true; then echo A; else echo B; fi'
cmp_c "if/else, ramo falso"  'if false; then echo A; else echo B; fi'
cmp_c "condicao com [ ]"     'if [ 1 -eq 1 ]; then echo yes; fi'
cmp_c "condicao com [[ ]]"   'if [[ -n x ]]; then echo yes; fi'
cmp_c "variavel na condicao" 'x=5; if [ $x -gt 3 ]; then echo big; else echo small; fi'
cmp_c "'fi' sozinho como argumento e palavra comum" 'if true; then echo A; fi; echo fi'

cmp_file "elif multi-linha" \
'x=2
if [ $x -eq 1 ]; then
echo one
elif [ $x -eq 2 ]; then
echo two
else
echo other
fi
'

printf '\n=== JINV-072: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
