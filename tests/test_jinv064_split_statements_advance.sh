#!/usr/bin/env bash
# JINV-064 — `split_statements` sempre avança: fechador órfão é palavra, não fim.
#
# ═══ A propriedade que este gate protege ═══
#
# Toda volta do laço externo de `split_statements` (`c/parser/if_block.c`) TEM
# que consumir pelo menos um byte. Quando não consome, a iteração seguinte
# começa no estado exato da anterior: o shell gira a 100% de CPU, sem saída,
# sem mensagem, sem log, sem core — e nunca volta. Não é uma saída errada que
# alguém confere depois; é o desaparecimento do shell.
#
# ═══ Por que ele existia ═══
#
# O coletor tratava `fi`, `esac` e `done` como TERMINADORES em qualquer
# posição. Quando o fechador não tinha abertura correspondente na profundidade
# corrente, ele fazia `break` sem consumir nada. Se o enunciado COMEÇAVA no
# fechador — e ele sempre começava, porque o `break` anterior tinha parado o
# ponteiro exatamente ali — o coletor devolvia `p == start`, `end == start`,
# nada era adicionado à lista, e o laço externo recomeçava no mesmo byte.
#
# O `break` na profundidade 0 era, portanto, um travamento garantido: não
# existia entrada que passasse por ele e terminasse. Trocá-lo por "consome
# como texto" não pode regredir nada — o caminho antigo nunca produzia
# resultado nenhum — e ainda alinha o jash com o bash, que lê `done` fora de
# posição de comando como uma palavra qualquer (`echo done` imprime `done`).
#
# ═══ Por que importa que seja um gate, e não um conserto ═══
#
# A função IRMÃ no mesmo arquivo, `tokenize_if_block`, já tinha exatamente esta
# guarda desde o JINV-043 — cujo comentário cita a primeira tentativa de
# conserto revertida por travar o shell de outro jeito. A disciplina foi
# aplicada a uma das duas irmãs e não à outra, e ninguém percebeu porque nada
# mecânico olhava para a segunda. Duas tentativas anteriores nesta mesma função
# foram revertidas. Um conserto que não vira gate volta.
#
# ═══ O gatilho, para quem for mexer aqui ═══
#
# O caminho defeituoso só é alcançado quando a linha do `if` contém `elif`, OU
# um `\n`, OU um ` if ` aninhado (o portão em `c/parser/eval.c`). Por isso os
# testes rápidos e óbvios (`if true; then echo "fi"; fi`) NÃO reproduziam: iam
# pelo caminho rápido, que é são. E `fi` é justamente a palavra imune, porque o
# tokenizador a consome como TOK_FI antes de o coletor vê-la. Quem trava é
# `done` e `esac` — os dois fechadores que o tokenizador deixa passar.
#
# ═══ Prazo em TODA invocação ═══
#
# Cada comando roda sob `tools/deadline.sh`. Um gate para travamento que roda
# sem prazo não falha: ele pendura a suíte inteira, que é um sintoma pior e
# mais difícil de atribuir do que um FAIL. `timeout(1)` NÃO existe neste macOS
# (e `command -v timeout` mente quem pergunta é o jash, onde é builtin) — por
# isso `deadline.sh`, que distingue 124 de 127 e RECUSA rodar sem mecanismo.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
DL="$PWD/tools/deadline.sh"
BSH=/bin/bash
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$DL" ]   || { echo "SKIP: tools/deadline.sh ausente"; exit 0; }
[ -x "$BSH" ]  || { echo "SKIP: $BSH ausente (oráculo)"; exit 0; }

DEADLINE=5

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

JOUT=""; JRC=0; BOUT=""; BRC=0

# stdout puro: comparar saída com 2>&1 misturaria dica/aviso com dado.
jrun() { JOUT=$("$DL" "$DEADLINE" "$JASH" -c "$1" 2>/dev/null); JRC=$?; }
brun() { BOUT=$("$DL" "$DEADLINE" "$BSH"  -c "$1" 2>/dev/null); BRC=$?; }

# Só exige que TERMINE. Para os casos em que a saída do jash diverge do bash
# por defeito de OUTRA função (documentados abaixo), travar continua proibido.
nohang() {
    jrun "$2"
    [ "$JRC" = 124 ] && { bad "$1" "TRAVOU (${DEADLINE}s): $2"; return 1; }
    ok "$1"
}

# Termina E concorda com o oráculo.
oracle() {
    jrun "$2"
    [ "$JRC" = 124 ] && { bad "$1" "TRAVOU (${DEADLINE}s): $2"; return 1; }
    brun "$2"
    [ "$JOUT" = "$BOUT" ] && { ok "$1"; return 0; }
    bad "$1" "jash=[$(printf '%s' "$JOUT" | tr '\n' '|')] bash=[$(printf '%s' "$BOUT" | tr '\n' '|')] em: $2"
}

# ── 1. As duas reproduções registradas ──────────────────────────────────
# Mínima: rota por `elif`, fechador `done` no corpo.
oracle "mínima (elif + \`echo done\`) responde como o bash" \
       'if false; then echo A; elif true; then echo done; fi'
oracle "mínima com \`esac\` no lugar de \`done\`" \
       'if false; then echo A; elif true; then echo esac; fi'
# Realista: script multi-linha, sem nenhum `elif` — a rota por `\n`. É assim
# que o defeito morde de verdade: qualquer if/then/fi escrito em linhas.
oracle "rota por \\n (if/then/fi multi-linha)" \
       'if true
then
echo done
fi'
oracle "rota por \` if \` aninhado" \
       'if true; then if true; then echo done; fi; fi'

# Mesmo caso, como ARQUIVO de script — o modo em que um usuário encontra isto.
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT INT TERM
printf 'if true\nthen\necho done\nfi\n' > "$T/ml.sh"
sout=$("$DL" "$DEADLINE" "$JASH" "$T/ml.sh" 2>/dev/null); src=$?
bout=$("$DL" "$DEADLINE" "$BSH"  "$T/ml.sh" 2>/dev/null)
if [ "$src" = 124 ]; then
    bad "script multi-linha no disco" "TRAVOU (${DEADLINE}s) ao rodar $T/ml.sh"
elif [ "$sout" = "$bout" ]; then
    ok "script multi-linha no disco responde como o bash"
else
    bad "script multi-linha no disco" "jash=[$sout] bash=[$bout]"
fi

# ── 2. `done` e `esac` em TODAS as posições ─────────────────────────────
# `at_keyword` exige que o fechador seja palavra (seguido de espaço, tab,
# `\n`, `;` ou fim). Por isso a varredura cobre início, meio, fim, repetido
# e sozinho — cada posição entra no coletor por um caminho diferente.
oracle "fechador no fim da palavra-de-comando"     'if false; then :; elif true; then echo x done; fi'
oracle "fechador no início do argumento"           'if false; then :; elif true; then echo done x; fi'
oracle "fechador entre dois enunciados sãos"       'if false; then :; elif true; then echo a; echo done; echo b; fi'
oracle "fechador repetido"                         'if false; then :; elif true; then echo done done; fi'
oracle "dois fechadores diferentes na mesma linha" 'if false; then :; elif true; then echo esac done; fi'
oracle "fechador entre aspas duplas"               'if false; then :; elif true; then echo "done"; fi'
oracle "fechador entre aspas simples"              "if false; then :; elif true; then echo 'esac'; fi"
oracle "fechador no ramo else"                     'if false
then
echo A
else
echo done
fi'
oracle "fechador na CONDIÇÃO, não no corpo"        'if test done = done
then
echo sim
fi'
oracle "fechador no último enunciado antes do fi"  'if false; then :; elif true; then echo a; echo esac; fi'
# Fechador SOZINHO como enunciado: o bash chama de erro de sintaxe, o jash de
# comando inexistente. Diferem no diagnóstico — não podem diferir em TERMINAR.
nohang "fechador sozinho como enunciado (só não pode travar)" \
       'if false; then :; elif true; then done; fi'

# ── 3. `fi` continua imune — e continua não travando ───────────────────
# `fi` nunca chega ao coletor: o tokenizador a consome como TOK_FI antes.
# A asserção existe para o dia em que alguém mexer nesse tokenizador: se `fi`
# passar a chegar aqui, ela tem que chegar num coletor que avança.
#
# DIVERGÊNCIA CONHECIDA, de OUTRA função: `echo fi` dentro de um bloco roteado
# imprime linha vazia + "fi: command not found", porque o coletor de
# `tokenize_if_block` corta a palavra `fi` do texto do comando. É saída errada,
# não travamento, e mora em `tokenize_if_block` — fora do alcance do JINV-064.
nohang "\`echo fi\` não trava (imunidade preservada)" \
       'if false; then echo A; elif true; then echo fi; fi'
nohang "\`fi\` entre aspas não trava"  'if false; then :; elif true; then echo "fi"; fi'
nohang "\`fi\` multi-linha não trava"  'if true
then
echo fi
fi'

# ── 4. O que já funcionava tem que continuar idêntico ───────────────────
# Se a guarda mudasse a divisão de enunciados, é aqui que apareceria.
oracle "if/elif simples"            'if true; then echo A; elif true; then echo B; fi'
oracle "elif escolhido"             'if false; then echo A; elif true; then echo B; fi'
oracle "else depois de dois elif"   'if false; then echo A; elif false; then echo B; else echo C; fi'
oracle "elif encadeado escolhe o 3º" 'if false; then echo A; elif false; then echo B; elif true; then echo C; else echo D; fi'
oracle "multi-linha then/fi"        'if true
then
echo A
fi'
oracle "multi-linha then/else/fi"   'if false
then
echo A
else
echo B
fi'
oracle "multi-linha com dois enunciados no corpo" 'if false
then
echo A
elif true
then
echo B
echo B2
fi'
oracle "if aninhado"                'if true; then if true; then echo N; fi; fi'
oracle "if aninhado com else"       'if true; then if false; then echo N; else echo M; fi; fi'
oracle "if aninhado dentro de elif" 'if false; then :; elif true; then if true; then echo N; fi; fi'
oracle "cauda depois do fi"         'if false; then echo A; elif true; then echo B; fi; echo tail'
oracle "; e \\n misturados"         'if false; then echo A
elif true; then echo B
fi'

# ── 5. Fechador LEGÍTIMO: o par abertura/fechamento não pode ser afetado ─
# Aqui `done`/`esac` fecham de verdade. Se a mudança tivesse comido o
# fechador de um `for`/`case` real, o enunciado sairia partido ao meio e
# estes casos quebrariam — é o teste que distingue "fechador é palavra" de
# "fechador foi ignorado".
oracle "case/esac dentro do if"       'if false; then :; elif true; then case abc in a*) echo hit;; *) echo miss;; esac; fi'
oracle "case/esac multi-linha + cauda" 'if true
then
case abc in
a*) echo hit ;;
*) echo miss ;;
esac
echo depois
fi'
oracle "case/esac seguido de outro enunciado" 'if false; then :; elif true; then case x in x) echo c;; esac; echo depois; fi'
oracle "for/do/done real (corpo sem \$)" 'if false; then :; elif true; then for i in 1 2; do echo hi; done; fi'
oracle "for/do/done multi-linha"      'if true
then
for i in 1 2
do
echo hi
done
echo depois
fi'
oracle "while que não itera + cauda"  'if false; then :; elif true; then while false; do echo x; done; echo pos; fi'

# DIVERGÊNCIA CONHECIDA, mais funda que o JINV-064 e deliberadamente NÃO
# consertada aqui: quando o fechador aparece como DADO DENTRO de um laço/case
# REAL (`for i in 1; do echo done; done`), a contagem de profundidade — que é
# por palavra, sem noção de posição-de-comando — decrementa no `done` do `echo`
# e o `;` seguinte parte o enunciado ao meio. Saída errada, rc≠0, sem
# travamento. Antes do JINV-064 estes dois casos TRAVAVAM; hoje terminam.
#
# A regra que faltaria é a do bash: um fechador só fecha em POSIÇÃO DE COMANDO
# (precedido, fora de brancos, por início, `;`, `\n`, `&`, `|`). Aplicá-la muda
# a contagem de TODOS os fechadores — blast radius maior que uma guarda de
# avanço, e esta função já teve dois consertos revertidos. Fica registrado
# aqui, com reprodução, para quem for fazer esse conserto com gate próprio.
nohang "fechador como dado DENTRO de for real (divergência conhecida, não trava)" \
       'if false; then :; elif true; then for i in 1; do echo done; done; fi'
nohang "fechador como dado DENTRO de case real (divergência conhecida, não trava)" \
       'if false; then :; elif true; then case x in x) echo esac;; esac; fi'
oracle "for aninhado em case dentro do if" \
       'if false; then :; elif true; then case x in x) for i in 1; do echo dentro; done;; esac; fi'

# ── 6. A guarda tem que ESTAR no código (não só no comportamento) ───────
# Comportamento verde com a guarda removida é possível — basta que nenhuma
# entrada do gate alcance o caminho. Esta asserção olha para a estrutura, que
# é o que o JINV-043 provou ser a propriedade real: cada volta consome um
# byte, por construção, e não por sorte de amostragem.
SRC="c/parser/if_block.c"
if [ -f "$SRC" ]; then
    body=$(/usr/bin/awk '/^static void split_statements/{f=1} f{print} f&&/^}/{exit}' "$SRC")
    if printf '%s' "$body" | grep -q 'iter_start' &&
       printf '%s' "$body" | grep -q 'p == iter_start'; then
        ok "split_statements mantém a guarda de avanço explícita"
    else
        bad "guarda ausente no código" \
            "split_statements perdeu \`iter_start\`/\`p == iter_start\` — o travamento deixou de ser impossível por construção"
    fi
    if printf '%s' "$body" | grep -q 'JINV-064'; then
        ok "o marcador JINV-064 está no código"
    else
        bad "marcador ausente" "sem /* JINV-064 */ ninguém que leia o código sabe por que isto está aqui"
    fi
else
    bad "fonte ausente" "$SRC não encontrado"
fi

echo ""
echo "=== JINV-064: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
