#!/usr/bin/env bash
# JINV-083 — palavra-chave: PALAVRA INTEIRA, FORA DE ASPAS, EM POSIÇÃO.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
#   Uma palavra-chave (`for while until select do done if then elif else fi
#   case esac`) só é ESTRUTURA quando as TRÊS condições valem ao mesmo tempo:
#   é palavra inteira (fronteira dos DOIS lados), está FORA DE ASPAS, e está em
#   POSIÇÃO DE COMANDO. Em qualquer outra situação ela é DADO: texto que o
#   autor escreveu para ser impresso, comparado ou passado adiante.
#
#   As três são independentes — cada uma derruba pelo menos uma asserção deste
#   gate quando removida sozinha do código (prova negativa por mutação).
#
# ═══ POR QUE ESTE GATE EXISTE, SE JÁ EXISTE O JINV-072 ═══
#
# O JINV-072 consertou `then`/`else`/`fi` em `c/parser/script.c` trocando um
# `strstr(" fi")` por uma busca por PALAVRA e CONSCIENTE DE ASPAS. Um agente
# seguinte auditou `for_loop.c`/`while_loop.c`, viu `strncmp` **com** checagem
# de fronteira de palavra, e declarou as duas sãs.
#
# Ele estava certo sobre o que olhou, e o defeito existia assim mesmo:
#
#   **Fronteira de palavra não é consciência de aspas.**
#
# Em `echo "ask for help"` o `for ` É uma palavra bem delimitada — só que
# citada. São propriedades independentes, e faltavam três pedaços diferentes:
#
#   (1) SEM ASPAS, com fronteira → um `for `/`while `/`until ` CITADO subia a
#       profundidade do laço e o `done` que o autor escreveu deixava de fechar:
#
#           for i in 1; do echo "ask for help"; done
#             bash -> ask for help    jash -> syntax error: missing done
#           for i in 1; do echo "ask 4 help"; done      (controle)
#             bash -> ask 4 help      jash -> ask 4 help
#
#   (2) SEM FRONTEIRA À ESQUERDA → `strncmp(p,"done",4)` casava DENTRO de
#       `fordone`, `undone`, `whiledone`, e o rabo da palavra virava COMANDO:
#
#           for i in 1; do echo undone; done
#             bash -> undone          jash -> `un` + `done: command not found`
#
#   (3) SEM POSIÇÃO DE COMANDO → mesmo palavra inteira e sem aspa nenhuma, o
#       caso mais banal de todos continuava quebrado. POSIX 2.9: palavra
#       reservada só é reservada em posição de comando.
#
#           for i in 1; do echo done; done
#             bash -> done            jash -> (nada)
#           for i in 1; do echo a done b; done
#             bash -> a done b        jash -> `a`
#
# É a mesma doença do JINV-016/017/018/019/072: DADO virando ESTRUTURA por
# leitura cega de texto. A diferença é só onde a leitura era cega.
#
# ═══ O ALCANCE, MEDIDO ═══
#
# 6 dos 19 `configure` de autoconf da máquina erravam o `done` só por isto.
# Dois gatilhos reais, ambos cobertos na seção (f) abaixo:
#   · `echo "$0 --help for help"`  — mata o `configure` do zlib (o menor dos 19)
#   · o padrão de `case` `| --program-transfor \` — termina em `for `
#
# ═══ O CONSERTO — UMA LEITURA, NÃO UMA QUARTA ═══
#
# Havia QUATRO cópias da mesma varredura de `done` (for_loop ×2, while_loop,
# select_loop) e a de `select` nem fronteira de palavra exigia. Todas passaram
# a chamar `jash_find_loop_close()`, construída sobre o MESMO scanner do
# JINV-072, agora público como `jash_find_keyword_set()` (c/parser/script.h).
# Nenhuma varredura nova foi escrita; três foram apagadas do mapa. As outras
# leituras cegas (tokenizador do `if` multi-linha, corte de bloco, contagem de
# profundidade do separador de topo) passaram a usar a mesma regra.
#
# Oráculo: /bin/bash, em TODA asserção.
# Prova negativa: JASH_UNDER_TEST=<binário anterior ao conserto> -> VERMELHO.
#   (JASH_UNDER_TEST, nunca JASH_BIN — o próprio jash exporta JASH_BIN, e um
#    gate que a lê passa verde contra qualquer mutante.)
#
# Código: c/parser/script.c      (jash_find_keyword_set / jash_find_loop_close
#                                 / jash_at_cmd_pos — a regra, nas duas formas)
#         c/parser/for_loop.c    (dois sítios: `for … in` e `for((…))`)
#         c/parser/while_loop.c  (while/until)
#         c/parser/select_loop.c (a quarta cópia, a mais pobre)
#         c/parser/case_stmt.c   (`esac` sem fronteira à direita)
#         c/parser/if_block.c    (tokenizador perguntava `at_keyword` por BYTE;
#                                 e a busca da cauda depois do `fi`)
#         c/parser/eval.c        (`strstr(line,"done")` decidia pipeline;
#                                 `jash_block_split`; `find_top_separator`)
#         c/main.c               (contagem de uma linha, cega a aspas)

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="${JASH_UNDER_TEST:-$PWD/build/jash}"
DL="$PWD/tools/deadline.sh"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0 JASH_MEMO=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

T=$(mktemp -d) || exit 2
H=$(mktemp -d) || exit 2
trap 'rm -rf "$T" "$H"' EXIT INT TERM

# Cão de guarda em TODA porta. NUNCA `timeout`: não existe nesta máquina,
# devolve 127 sob bash, e o teste conclui "não travou" exatamente quando
# travou. Um laço que perde o `done` é candidato natural a girar para sempre.
cmp_c() {   # cmp_c <nome> <linha>
    local name="$1" cmd="$2"
    local bo bc jo jc
    bo=$(cd "$T" && HOME="$H" "$DL" 15 /bin/bash -c "$cmd" 2>/dev/null </dev/null); bc=$?
    jo=$(cd "$T" && HOME="$H" "$DL" 15 "$JASH"   -c "$cmd" 2>/dev/null </dev/null); jc=$?
    if [ "$bc" = 124 ]; then bad "$name" "o ORACULO travou — asserção inválida"; return; fi
    if [ "$bo" = "$jo" ] && [ "$bc" = "$jc" ]; then ok "$name"
    else bad "$name" "bash(rc=$bc)=[$bo]  jash(rc=$jc)=[$jo]"; fi
}

# Compara stdout, rc — E exige stderr vazio onde o do bash é vazio.
# Sem a parte do stderr este gate seria cego para metade do sintoma: quando o
# `done` erra o alvo, o rabo da palavra vai para o stderr como
# `done: command not found` enquanto o stdout coincide por acidente.
cmp_file() {   # cmp_file <nome> <corpo-do-script>
    local name="$1" body="$2"
    printf '%s' "$body" > "$T/c.sh"
    local bo bc jo jc be je
    bo=$(cd "$T" && HOME="$H" "$DL" 15 /bin/bash ./c.sh 2>"$T/be" </dev/null); bc=$?
    jo=$(cd "$T" && HOME="$H" "$DL" 15 "$JASH"   ./c.sh 2>"$T/je" </dev/null); jc=$?
    if [ "$bc" = 124 ]; then bad "$name" "o ORACULO travou — asserção inválida"; return; fi
    be=$(wc -c < "$T/be" | tr -d ' '); je=$(wc -c < "$T/je" | tr -d ' ')
    if [ "$bo" != "$jo" ] || [ "$bc" != "$jc" ]; then
        bad "$name" "bash(rc=$bc)=[$bo]  jash(rc=$jc)=[$jo]"
    elif [ "$be" = "0" ] && [ "$je" != "0" ]; then
        bad "$name (stderr)" "bash calou, jash reclamou: $(head -c 140 "$T/je")"
    else
        ok "$name"
    fi
}

echo "── (a) o controle: a MESMA linha sem a palavra já funcionava ──────────"

# Sem este par o gate não prova nada: ele mostra que o que muda o veredito é a
# PALAVRA, não o tamanho, nem a citação, nem o laço.
cmp_file "controle: 'ask 4 help' (sem palavra-chave)" \
'for i in 1
do
  echo "ask 4 help"
done
echo cauda
'
cmp_file "o mesmo, com 'ask for help'" \
'for i in 1
do
  echo "ask for help"
done
echo cauda
'

echo "── (b) cada palavra-chave dentro de ASPAS DUPLAS é DADO ───────────────"

cmp_c 'for'    'for i in 1; do echo "ask for help"; done; echo cauda'
cmp_c 'while'  'for i in 1; do echo "wait while busy"; done; echo cauda'
cmp_c 'until'  'for i in 1; do echo "wait until dawn"; done; echo cauda'
cmp_c 'select' 'for i in 1; do echo "please select one"; done; echo cauda'
cmp_c 'do'     'for i in 1; do echo "just do it"; done; echo cauda'
cmp_c 'done'   'for i in 1; do echo "all done here"; done; echo cauda'
cmp_c 'if'     'for i in 1; do echo "ask if needed"; done; echo cauda'
cmp_c 'then'   'for i in 1; do echo "a then b"; done; echo cauda'
cmp_c 'elif'   'for i in 1; do echo "a elif b"; done; echo cauda'
cmp_c 'else'   'for i in 1; do echo "a else b"; done; echo cauda'
cmp_c 'fi'     'for i in 1; do echo "the fi word"; done; echo cauda'
cmp_c 'case'   'for i in 1; do echo "in case of fire"; done; echo cauda'
cmp_c 'esac'   'for i in 1; do echo "the esac word"; done; echo cauda'

echo "── (b2) o SEPARADOR dentro das aspas também é dado ────────────────────"

# Estas quatro são as que EXIGEM consciência de aspas — a regra de posição de
# comando sozinha não salva nenhuma delas: um `;` ou uma nova-linha DENTRO das
# aspas põe o texto seguinte em posição de comando para quem não enxerga aspa.
cmp_c 'aspas com "; for x in y; do"' 'for i in 1; do echo "; for x in y; do"; done; echo cauda'
cmp_c 'aspas com "; done"'           'for i in 1; do echo "tail; done"; done; echo cauda'
cmp_c "aspas simples com '; done'"   "for i in 1; do echo 'tail; done'; done; echo cauda"
cmp_c 'nova-linha dentro de aspas'   'for i in 1; do echo "a
done"; done; echo cauda'

echo "── (c) cada palavra-chave dentro de ASPAS SIMPLES é DADO ──────────────"

cmp_c "'for'"    "for i in 1; do echo 'ask for help'; done; echo cauda"
cmp_c "'while'"  "for i in 1; do echo 'wait while busy'; done; echo cauda"
cmp_c "'until'"  "for i in 1; do echo 'wait until dawn'; done; echo cauda"
cmp_c "'done'"   "for i in 1; do echo 'all done here'; done; echo cauda"
cmp_c "'esac'"   "for i in 1; do echo 'the esac word'; done; echo cauda"
cmp_c "'fi'"     "for i in 1; do echo 'the fi word'; done; echo cauda"

echo "── (d) palavra-chave NO MEIO DE UMA PALAVRA é DADO ────────────────────"

# À DIREITA (a palavra-chave é prefixo): `do` em `download`, `for` em
# `foreign`, `until` em `untilted`.
cmp_c 'download (do…)'    'for i in 1; do echo download; done; echo cauda'
cmp_c 'foreign (for…)'    'for i in 1; do echo foreign; done; echo cauda'
cmp_c 'donefile (done…)'  'for i in 1; do echo donefile; done; echo cauda'
cmp_c 'casement (case…)'  'for i in 1; do echo casement; done; echo cauda'

# À ESQUERDA (a palavra-chave é sufixo) — esta metade NENHUMA das varreduras
# antigas pegava: `strncmp(p,"done",4)` não olha o byte anterior.
cmp_c 'undone (…done)'      'for i in 1; do echo undone; done; echo cauda'
cmp_c 'fordone (…done)'     'for i in 1; do echo fordone; done; echo cauda'
cmp_c 'whiledone (…done)'   'for i in 1; do echo whiledone; done; echo cauda'
cmp_c 'transfor (…for)'     'for i in 1; do echo --program-transfor; done; echo cauda'
cmp_c 'beforehand (…for…)'  'for i in 1; do echo beforehand; done; echo cauda'

# Esta é a asserção que só a FRONTEIRA À ESQUERDA salva. `}` põe o que vem
# depois em posição de comando, mas NÃO é fronteira de palavra — então num
# leitor que só olhasse posição, o `done` de `a}done` fecharia o laço:
#     bash -> a}done      leitor sem fronteira -> `a}` + `done: not found`
cmp_c 'a}done (} é posição, não fronteira)' 'for i in 1; do echo a}done; done; echo cauda'

echo "── (e) posição de ARGUMENTO: palavra reservada só reserva em posição ──"

# A TERCEIRA propriedade, e a que faltava depois das aspas: POSIX 2.9 — uma
# palavra reservada só é reservada em POSIÇÃO DE COMANDO. Sem ela, o caso mais
# banal de todos continuava quebrado, mesmo sem uma aspa à vista:
#     for i in 1; do echo done; done   →  bash: `done`   jash: (nada)
cmp_c 'echo done (nu, argumento)' 'for i in 1; do echo done; done; echo cauda'
cmp_c 'echo a done b'             'for i in 1; do echo a done b; done; echo cauda'
cmp_c 'argumento de printf'       'for i in 1; do printf "%s\n" "for while done"; done; echo cauda'
cmp_c 'argumento de test'         'for i in 1; do [ "done" = "done" ] && echo eq; done; echo cauda'
cmp_c 'valor de variavel'         'for i in 1; do x="for done"; echo "$x"; done; echo cauda'
cmp_c 'x=done (sem aspas)'        'for i in 1; do x=done; echo $x; done; echo cauda'
cmp_c 'padrao de case'            'for i in 1; do case done in done) echo hit;; esac; done; echo cauda'

echo "── (e2) em posição de COMANDO ela CONTINUA sendo estrutura ────────────"

# Um gate que só provasse "citado é dado" passaria verde num shell que nunca
# mais reconhecesse laço nenhum. Esta metade é a que impede esse "conserto".
cmp_c 'laco simples ainda roda'  'for i in 1 2 3; do echo $i; done'
cmp_c 'while simples ainda roda' 'n=0; while [ $n -lt 3 ]; do n=$((n+1)); echo $n; done'
cmp_c 'until simples ainda roda' 'n=0; until [ $n -ge 3 ]; do n=$((n+1)); echo $n; done'
cmp_c 'done | pipe'              'for i in 1 2; do echo $i; done | /usr/bin/wc -l'
cmp_c 'done apos }'              'for i in 1; do { echo x; }; done; echo cauda'
cmp_c 'done;done colado'         'for i in 1; do for j in 2; do echo $j; done;done; echo cauda'
cmp_c 'laco dentro de $( )'      'x=$(for i in 1 2; do echo -n $i; done); echo [$x]'
cmp_c 'for depois de then'       'if true; then for j in 2; do echo $j; done; fi; echo cauda'

cmp_file "done depois de um redirecionamento" \
'for i in 1 2; do echo $i; done > out.txt
/bin/cat out.txt
'
cmp_file "done depois de comentario" \
'for i in 1
do
  echo x   # esta nota fala de done e de for
done
echo cauda
'
cmp_file "done depois de junção de linha (\\ + nova-linha)" \
'for i in 1
do
  echo x; \
done
echo cauda
'

echo "── (f) ANINHADO ──────────────────────────────────────────────────────"

cmp_file "for dentro de for, com 'for' citado no corpo interno" \
'for i in 1
do
  for j in 2
  do
    echo "ask for help"
  done
done
echo cauda
'
cmp_file "while dentro de for, com 'done' citado" \
'for i in 1
do
  n=0
  while [ $n -lt 1 ]
  do
    n=1
    echo "all done here"
  done
done
echo cauda
'
cmp_c 'for(( aninhado sem espaco' 'for i in 1; do for((j=0;j<2;j++)); do echo $j; done; done; echo cauda'
cmp_c 'until dentro de while'     'n=0; while [ $n -lt 1 ]; do n=1; m=0; until [ $m -ge 1 ]; do m=1; echo "wait until dawn"; done; done; echo cauda'

echo "── (g) \$( ) e crase ──────────────────────────────────────────────────"

cmp_c 'cmdsub citado com for'   'for i in 1; do echo "$(echo for)"; done; echo cauda'
cmp_c 'cmdsub nu com for'       'for i in 1; do echo $(echo for); done; echo cauda'
cmp_c 'cmdsub citado com done'  'for i in 1; do echo "$(echo done)"; done; echo cauda'
cmp_c 'crase citada com for'    'for i in 1; do echo "`echo for`"; done; echo cauda'

echo "── (h) HEREDOC: o corpo é dado, sempre ────────────────────────────────"

cmp_file "heredoc com 'for' no corpo" \
'for i in 1
do
  /bin/cat <<EOF
ask for help
EOF
done
echo cauda
'
cmp_file "heredoc com 'done' no corpo" \
'for i in 1
do
  /bin/cat <<EOF
all done here
EOF
done
echo cauda
'
cmp_file "heredoc com 'done' sozinho numa linha" \
'for i in 1
do
  /bin/cat <<EOF
done
EOF
done
echo cauda
'

echo "── (i) 'esac' também é PALAVRA (c/parser/case_stmt.c) ─────────────────"

cmp_c 'padrao esacx' 'case esacx in esacx) echo hit;; *) echo miss;; esac'
cmp_c 'padrao esac*' 'case esacX in esac*) echo hit;; *) echo miss;; esac'
cmp_c 'esac fecha mesmo' 'case a in a) echo hit;; esac; echo cauda'

echo "── (i2) 'if'/'fi' — a irmã em c/parser/if_block.c e script.c ──────────"

# O JINV-072 consertou `find_keyword` em script.c, mas o TOKENIZADOR do bloco
# multi-linha (if_block.c) perguntava `at_keyword()` a cada byte — sem
# fronteira à esquerda, sem aspas, sem posição — e o `if` de uma linha só
# procurava `fi` sem exigir posição de comando.
cmp_c 'echo fi (argumento) num if'  'if true; then echo fi; fi; echo cauda'
cmp_c 'echo "a fi b"'               'if true; then echo "a fi b"; fi; echo cauda'
cmp_c 'echo "a if b"'               'if true; then echo "a if b"; fi; echo cauda'
cmp_file "bloco multi-linha: echo fi" \
'if true
then
  echo fi
fi
echo cauda
'
cmp_file "bloco multi-linha: echo wififi (sufixo)" \
'if true
then
  echo wififi
fi
echo cauda
'
cmp_file "bloco multi-linha: echo sci-fi x" \
'if true
then
  echo sci-fi x
fi
echo cauda
'
cmp_file "bloco multi-linha: echo \"a fi b\"" \
'if true
then
  echo "a fi b"
fi
echo cauda
'
cmp_file "if aninhado ainda funciona" \
'if true
then
  if false
  then
    echo inner
  else
    echo other
  fi
fi
echo cauda
'

echo "── (j) OS DOIS GATILHOS REAIS DOS 19 configure ────────────────────────"

# (1) o laço principal do `configure` do zlib — o menor dos 19. O corpo tem
#     `echo "$0 --help for help"`; o `for ` citado engolia o `done`.
cmp_file "zlib: \$0 --help for help" \
'set -- -h
while test $# -ge 1
do
case "$1" in
  -h*) echo usage; shift;;
  *) echo "$0 --help for help"; exit 1;;
esac
done
echo cauda
'

# (2) o `for ac_option` de autoconf: o padrão de `case` termina em `for `
#     porque a barra de continuação vem logo depois de `--program-transfor`.
cmp_file "autoconf: | --program-transfor \\" \
'for ac_option in --x --y
do
  case $ac_option in
  -program-prefix | --program-prefix \
  | --program-transfor)
    echo hit ;;
  *) echo "opt $ac_option" ;;
  esac
done
echo cauda
'

printf '\n=== JINV-083: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
