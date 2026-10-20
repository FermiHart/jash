#!/usr/bin/env bash
# JINV-081 — o acumulador de enunciado composto nao pode truncar em SILENCIO.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
#   Um enunciado composto (`for`/`while`/`until`/`if`/`case`/`select`/funcao)
#   e montado por `jash_run_multiline` num buffer antes de ser avaliado. Esse
#   buffer NAO tem teto arbitrario: o enunciado que chega ao avaliador e
#   EXATAMENTE o que o autor escreveu, do `for` ate o `done`, tenha ele 40
#   bytes ou 200 KB. Se algum dia faltar memoria, a recusa e ALTA (stderr +
#   rc != 0 + NADA executa). O que nao pode existir e o meio-termo: rodar um
#   programa diferente do escrito.
#
# ═══ O DEFEITO ═══
#
#   Tres buffers fixos no MESMO laco de c/main.c, todos com corte mudo:
#
#     char sline[4096];                       // o enunciado montado
#     if (si >= sizeof(sline) - 1) { sline[sizeof(sline)-1] = '\0'; break; }
#
#     char ppeek[4096];                       // a linha FISICA lida por vez
#     while (*peek && *peek != '\n' && pi < sizeof(ppeek) - 1) ...
#
#     while (*rp && pb < JASH_SCRIPT_PROC_CAP - 1)   // o script pre-processado
#
#   O primeiro entregava ao avaliador um `for` sem o `done`, um `if` sem o
#   `fi`. Penhasco medido (arquivo inteiro, corpo de recheio):
#
#     for    4043 B  OK   │ for    4713 B  "missing done"
#     if     4040 B  OK   │ if     4710 B  "missing fi"
#     while  4061 B  OK   │ while  4731 B  "missing done"
#
#   O terceiro cortava o SCRIPT em 128 KiB: 125.623 B rodava igual ao bash,
#   139.823 B nao.
#
# ═══ POR QUE ISSO DECIDIA TUDO ═══
#
#   Todo `configure` de autoconf abre com um bloco
#   `for ac_option / do … case … esac / done` de ~7 KB, e 11 dos 19 medidos
#   sao maiores que 128 KiB. 28 das 38 execucoes morriam exatamente no
#   primeiro teto; nenhuma passava do segundo.
#
# PROVA NEGATIVA: JASH_UNDER_TEST=<binario sem o conserto> deixa este gate
# VERMELHO (JINV-061). Use JASH_UNDER_TEST, NUNCA JASH_BIN — o proprio jash
# exporta JASH_BIN apontando para o binario real, e um gate que a leia passa
# verde contra qualquer mutante.
#
# ORACULO: /bin/bash em toda asercao.
# Gate: bash tests/test_jinv081_compound_accumulator.sh

set -u
cd "$(dirname "$0")/.." || exit 2

JASH="${JASH_UNDER_TEST:-$PWD/build/jash}"
DL="$PWD/tools/deadline.sh"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0 JASH_MEMO=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$DL" ]   || { echo "FAIL: $DL ausente (cao de guarda obrigatorio)"; exit 1; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

SB=$(mktemp -d) || exit 2
trap 'rm -rf "$SB"' EXIT INT TERM

# O cao de guarda tem que estar VIVO. `timeout(1)` nao existe nesta maquina e
# devolveria 127 — o teste concluiria "nao travou" quando travou.
"$DL" 2 /bin/sleep 5 >/dev/null 2>&1
[ $? = 124 ] || { echo "FAIL: deadline.sh nao protege (rc != 124)"; exit 1; }

# ── gerador ─────────────────────────────────────────────────────────────────
# $1 = numero de linhas de recheio (~68 B cada)
# $2 = for | while | until | if | case | func
# Escreve $SB/t.sh. Todo enunciado imprime MARCA no fim do corpo e cauda
# depois de fechado — o que some primeiro num corte e justamente a MARCA.
gera() {
    local n="$1" k="$2" i=0
    {
        case "$k" in
          for)   printf 'for x in a\ndo\n' ;;
          while) printf 'n=0\nwhile [ $n -lt 1 ]\ndo\n  n=1\n' ;;
          until) printf 'n=0\nuntil [ $n -gt 0 ]\ndo\n  n=1\n' ;;
          if)    printf 'if true\nthen\n' ;;
          case)  printf 'v=a\ncase $v in\n  a)\n' ;;
          func)  printf 'f() {\n' ;;
        esac
        while [ "$i" -lt "$n" ]; do
            printf '  r%s=rrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr%s\n' "$i" "$i"
            i=$((i + 1))
        done
        printf '  echo MARCA\n'
        case "$k" in
          for|while|until) printf 'done\n' ;;
          if)              printf 'fi\n' ;;
          case)            printf '  ;;\nesac\n' ;;
          func)            printf '}\nf\n' ;;
        esac
        printf 'echo cauda\n'
    } > "$SB/t.sh"
}

# Rc por FUNCAO, nunca captura inline com os dois canais juntos (JINV-061).
jfout() { (cd "$SB" && "$DL" 60 "$JASH"   ./t.sh 2>/dev/null); }
bfout() { (cd "$SB" && "$DL" 60 /bin/bash ./t.sh 2>/dev/null); }
jferr() { (cd "$SB" && "$DL" 60 "$JASH"   ./t.sh 2>&1 >/dev/null); }
jfrc()  { (cd "$SB" && "$DL" 60 "$JASH"   ./t.sh >/dev/null 2>&1); printf '%s' "$?"; }
bfrc()  { (cd "$SB" && "$DL" 60 /bin/bash ./t.sh >/dev/null 2>&1); printf '%s' "$?"; }

comparado() {
    local desc="$1" jo bo jr br
    jo=$(jfout); jr=$(jfrc)
    bo=$(bfout); br=$(bfrc)
    if [ "$jr" = "124" ]; then
        bad "$desc" "TRAVOU (deadline 60s)"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$desc"
    else
        bad "$desc" "jash rc=$jr out=[${jo:0:120}] | bash rc=$br out=[${bo:0:120}]"
    fi
}

# Rc de `-c` tambem por FUNCAO (JINV-061 MERGED-EXACT: uma captura inline com
# os dois canais na mesma expressao deixa um aviso de UX decidir o veredito).
jcout() { (cd "$SB" && "$DL" 60 "$JASH"   -c "$1" 2>/dev/null); }
bcout() { (cd "$SB" && "$DL" 60 /bin/bash -c "$1" 2>/dev/null); }
jcrc()  { (cd "$SB" && "$DL" 60 "$JASH"   -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }
bcrc()  { (cd "$SB" && "$DL" 60 /bin/bash -c "$1" >/dev/null 2>&1); printf '%s' "$?"; }

# Mesma comparacao, mas o programa vem por `-c` (string multilinha), nao por
# arquivo: e o outro portao de entrada do MESMO acumulador.
comparado_c() {
    local desc="$1" prog jo bo jr br
    prog=$(cat "$SB/t.sh")
    jo=$(jcout "$prog"); jr=$(jcrc "$prog")
    bo=$(bcout "$prog"); br=$(bcrc "$prog")
    if [ "$jr" = "124" ]; then
        bad "$desc" "TRAVOU (deadline 60s)"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$desc"
    else
        bad "$desc" "jash rc=$jr out=[${jo:0:120}] | bash rc=$br out=[${bo:0:120}]"
    fi
}

echo "── (1) os dois lados do penhasco de 4096 bytes, por ARQUIVO ─────────"
# 55 linhas de recheio ficam logo ABAIXO (~4,0 KB de arquivo);
# 65 ficam logo ACIMA (~4,7 KB) — era ali que o `done`/`fi` desaparecia.
#
# `func` NAO entra nesta comparacao: o corpo de uma funcao tem um teto MENOR
# e INDEPENDENTE deste — `char body[2048]` em c/function_defs.h, copiado com
# strncpy mudo em jash_function_define(). Medido: corpo de 1354 B igual ao
# bash, 2694 B perde a cauda, rc 0, stderr vazio. Como esse teto e mais baixo
# que o do acumulador, nenhum tamanho consegue exercitar o acumulador pela
# porta da funcao. Fica registrado na secao (9) como bloqueador aberto, e o
# que da para travar aqui (paridade abaixo dele) esta na secao (8).
for k in for while until if case; do
    for n in 55 65; do
        gera "$n" "$k"
        sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
        comparado "$k com $n linhas de corpo (arquivo de $sz B)"
    done
done

echo
echo "── (2) o mesmo penhasco pela porta do -c ────────────────────────────"
for k in for if case; do
    for n in 55 65; do
        gera "$n" "$k"
        sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
        comparado_c "-c: $k com $n linhas ($sz B)"
    done
done

echo
echo "── (3) MUITO acima do teto antigo: ~100 KB de corpo ─────────────────"
for k in for while until if case; do
    gera 1500 "$k"
    sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
    comparado "$k com corpo de $sz B (~25x o teto antigo)"
done

echo
echo "── (4) aninhados: composto dentro de composto, os dois grandes ──────"
{
    printf 'for i in 1 2\ndo\n'
    k=0; while [ $k -lt 60 ]; do
        printf '  o%s=oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo%s\n' "$k" "$k"; k=$((k+1)); done
    printf '  case $i in\n    *)\n'
    k=0; while [ $k -lt 60 ]; do
        printf '      c%s=cccccccccccccccccccccccccccccccccccccccccccccccccccccccc%s\n' "$k" "$k"; k=$((k+1)); done
    printf '      while [ "$i" = 9 ]\n      do\n'
    k=0; while [ $k -lt 60 ]; do
        printf '        w%s=wwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwww%s\n' "$k" "$k"; k=$((k+1)); done
    printf '        echo nunca\n      done\n'
    printf '      echo "ramo=$i"\n    ;;\n  esac\n'
    printf '  echo "fora=$i"\ndone\necho cauda\n'
} > "$SB/t.sh"
sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
comparado "for > case > while, os tres acima de 4 KB (arquivo de $sz B)"

{
    printf 'if true\nthen\n'
    k=0; while [ $k -lt 70 ]; do
        printf '  a%s=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa%s\n' "$k" "$k"; k=$((k+1)); done
    printf '  if false\n  then\n    echo nunca\n  else\n'
    k=0; while [ $k -lt 70 ]; do
        printf '    b%s=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb%s\n' "$k" "$k"; k=$((k+1)); done
    printf '    echo interno\n  fi\n  echo externo\nfi\necho cauda\n'
} > "$SB/t.sh"
sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
comparado "if dentro de if, os dois acima de 4 KB (arquivo de $sz B)"

echo
echo "── (5) a LINHA FISICA longa dentro do composto (o teto do ppeek) ────"
# Uma unica linha de ~9 KB dentro de um composto: `char ppeek[4096]` a lia
# ate 4095 bytes e o resto sumia — o comando executado nao era o escrito.
# A linha e feita de MUITOS comandos curtos (nao de um valor gigante): assim
# o que se mede aqui e a leitura da linha, e nao o teto de valor de variavel
# (`len=1023`, defeito separado e ainda aberto — ver secao (9)).
for kind in for if case; do
    {
        case "$kind" in
          for)  printf 'for i in 1\ndo\n' ;;
          if)   printf 'if true\nthen\n' ;;
          case) printf 'v=a\ncase $v in\n  a)\n' ;;
        esac
        printf ' '
        k=0; while [ $k -lt 400 ]; do printf ' echo m%s;' "$k"; k=$((k+1)); done
        printf '\n'
        case "$kind" in
          for)  printf 'done\n' ;;
          if)   printf 'fi\n' ;;
          case) printf '  ;;\nesac\n' ;;
        esac
        printf 'echo cauda\n'
    } > "$SB/t.sh"
    sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
    comparado "$kind com uma linha unica de ~4,5 KB e 400 comandos ($sz B)"
done

echo
echo "── (6) o script INTEIRO acima de 128 KiB (o teto do pbuf) ───────────"
# 1800 linhas ficam LOGO ABAIXO de 131072 (passava antes tambem); 2000 e a
# primeira medida acima; 2500 e bem acima. O penhasco medido foi
# 125.623 B ok / 139.823 B truncado.
for n in 1800 2000 2500; do
    gera "$n" for
    sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
    lado="abaixo de"; [ "$sz" -gt 131072 ] && lado="ACIMA de"
    comparado "script de $sz B ($lado JASH_SCRIPT_PROC_CAP=131072)"
done
# … e com o corpo composto de enunciados INDEPENDENTES (o corte do pbuf caia
# entre enunciados: a metade final do script simplesmente nao existia, rc 0).
{
    i=0
    while [ $i -lt 2400 ]; do
        printf 'p%s=pppppppppppppppppppppppppppppppppppppppppppppppppppppppppp%s\n' "$i" "$i"
        i=$((i + 1))
    done
    printf 'echo ULTIMO\n'
} > "$SB/t.sh"
sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
[ "$sz" -gt 131072 ] || bad "fixture fraca" "o script tem $sz B, abaixo de JASH_SCRIPT_PROC_CAP (131072) — nao exercita o teto"
comparado "script de $sz B sem composto nenhum — a cauda existe?"

echo
echo "── (7) A propriedade central: NADA se perde em silencio ─────────────"
gera 1500 for
jo=$(jfout); je=$(jferr); jr=$(jfrc); bo=$(bfout)
if [ "$jo" = "$bo" ]; then
    ok "enunciado de 100 KB produz a saida do bash (nem truncado, nem mudo)"
elif [ -n "$je" ] && [ "$jr" != "0" ]; then
    ok "enunciado de 100 KB recusado ALTO (rc=$jr + stderr) — nao e morte silenciosa"
else
    bad "morte silenciosa" "rc=$jr, stderr vazio, jash=[${jo:0:80}] != bash=[${bo:0:80}]"
fi

echo
echo "── (8) o caminho curto (< 4 KB) nao regrediu ────────────────────────"
oracle_c() {
    local desc="$1" code="$2" jo bo jr br
    jo=$(jcout "$code"); jr=$(jcrc "$code")
    bo=$(bcout "$code"); br=$(bcrc "$code")
    if [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then ok "$desc"
    else bad "$desc" "jash rc=$jr out=[$jo] | bash rc=$br out=[$bo] | $code"; fi
}
oracle_c "for de uma linha"        'for i in 1 2 3; do echo "[$i]"; done'
oracle_c "if multilinha"           $'if true\nthen\n  echo sim\nfi'
oracle_c "case multilinha"         $'v=b\ncase $v in\n  a) echo A ;;\n  b) echo B ;;\nesac'
oracle_c "while multilinha"        $'n=0\nwhile [ $n -lt 3 ]\ndo\n  n=$((n+1))\n  echo "n=$n"\ndone'
oracle_c "funcao multilinha"       $'g() {\n  echo dentro\n}\ng'
oracle_c "array multilinha"        $'a=(x\ny z\n)\necho "${a[@]}"'
oracle_c "heredoc"                 $'cat <<EOF\nlinha1\nlinha2\nEOF'
oracle_c "comentario com apostrofo" $'# it\'s a note\necho depois'
oracle_c "continuacao de barra"    $'echo um \\\ndois'
oracle_c "rc do composto vaza"     'if true; then false; fi; echo rc=$?'
# Funcao: paridade ABAIXO do teto proprio dela (c/function_defs.h, 2048 B).
{
    printf 'f() {\n'
    k=0; while [ $k -lt 18 ]; do
        printf '  h%s=hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh%s\n' "$k" "$k"; k=$((k+1)); done
    printf '  echo MARCA\n}\nf\necho cauda\n'
} > "$SB/t.sh"
sz=$(wc -c < "$SB/t.sh" | tr -d ' ')
comparado "funcao multilinha de $sz B (abaixo do teto de function_defs.h)"

echo
echo "── (9) tetos VIZINHOS ainda abertos — medidos, nao consertados aqui ─"
# Nenhuma asercao: e um lembrete executavel de onde esta o proximo penhasco.
# Nao entram neste invariante porque nao sao o acumulador; sao buffers de
# OUTROS estagios, cada um com seu conserto e seu gate.
# CONSERTADOS depois desta onda — mantidos aqui como historico, com o gate que
# agora os cobre. Uma nota que continua anunciando um bug ja morto e tao ruim
# quanto uma que esconde um vivo: manda o proximo agente cacar um fantasma.
echo "note  [CONSERTADO — JINV-086, tests/test_jinv086_silent_caps.sh] corpo de funcao: char body[2048] em c/function_defs.h — era 1354 B ok, 2694 B perdia a cauda (rc 0, mudo)"
echo "note  [CONSERTADO — JINV-086] valor de variavel: corte em 1023 bytes no avaliador — \`s=<1280 chars>; echo \${#s}\` dizia 1023"
echo "note  palavra-chave em ASPAS conta como estrutura: \`while …; do echo \"ask for help\"; done\` -> missing done"
echo "note  ABERTO: texto do COMANDO dentro de \$( ) corta em 511 B (c/parser/cmdsub.c char inner[512]) — \`V=\$(printf %s <505 a's>)\` devolve 500; nao e perda de dado, e \$( ) executando outro comando"

printf '\n=== JINV-081: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
