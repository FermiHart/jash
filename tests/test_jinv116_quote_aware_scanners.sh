#!/usr/bin/env bash
# JINV-116/117 — um único scanner ciente de aspas
#
# Historia: oito lugares do parser varriam a linha procurando `;`, `|`, `<`, `>`
# com uma maquininha de aspas COPIADA em cada um. As copias divergiram. A pior
# delas (c/parser/pipeline.c:line_has_op) fechava a aspa dupla num `\"` escapado,
# entao `echo "a\">arquivo"` era classificado como redirect e o shell TRUNCAVA
# um arquivo que o usuario nunca mandou tocar. Perda de dados silenciosa.
#
# Agora todos delegam para jash_scan_unquoted() (c/parser/quotes.c): a regra da
# aspa dupla mora num lugar so. Este gate prova as duas metades:
#   (a) o `;`/`|`/`>` CITADO nao separa nada  (falso-positivo -> perda de dados)
#   (b) o `;`/`|`/`>` NU continua separando   (falso-negativo -> shell quebrado)
#
# Author: F E R M I ∞ H A R T <contact@fermihart.com>
set -u

J="${JASH:-$(cd "$(dirname "$0")/.." && pwd)/build/jash}"
export JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok()   { pass=$((pass+1)); printf '  ok   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL %s\n     esperado: [%s]\n     obtido  : [%s]\n' "$1" "$2" "$3"; }

# eq <nome> <esperado> <comando>
eq() {
  local name="$1" want="$2" cmd="$3" got
  got="$("$J" -c "$cmd" 2>&1)"
  [ "$got" = "$want" ] && ok "$name" || bad "$name" "$want" "$got"
}

echo "=== JINV-117: aspas NAO podem virar redirect (perda de dados) ==="

# O caso original. `\"` dentro de aspas duplas e uma aspa LITERAL: a string
# continua aberta, o `>` esta dentro dela, e nada deve ser redirecionado.
printf 'INTACTO\n' > "$TMP/alvo.txt"
"$J" -c "echo \"a\\\">$TMP/alvo.txt\"" >/dev/null 2>&1
if [ "$(cat "$TMP/alvo.txt")" = "INTACTO" ]; then
  ok "aspa escapada antes de > nao trunca o arquivo citado"
else
  bad "aspa escapada antes de > nao trunca o arquivo citado" "INTACTO" "$(cat "$TMP/alvo.txt")"
fi

# Mesma armadilha para o pipe: `\"` seguido de `|` dentro da string.
eq "aspa escapada antes de | nao vira pipeline" 'a"|wc' 'echo "a\"|wc"'

# `>` simplesmente citado (sem escape) tambem nao redireciona.
printf 'INTACTO\n' > "$TMP/alvo2.txt"
"$J" -c "echo \"texto > $TMP/alvo2.txt\"" >/dev/null 2>&1
if [ "$(cat "$TMP/alvo2.txt")" = "INTACTO" ]; then
  ok "> dentro de aspas duplas nao trunca"
else
  bad "> dentro de aspas duplas nao trunca" "INTACTO" "$(cat "$TMP/alvo2.txt")"
fi

echo "=== JINV-116: ; citado nao fecha a condicao/lista dos compostos ==="

# for: o `;` citado ficava marcado como fim da lista de palavras e o resto do
# comando desaparecia.
eq "for: ; citado sobrevive na lista de palavras" \
   'a;b
c' \
   'for f in "a;b" c; do echo "$f"; done'

# while: a condicao terminava no `;` de dentro da string e o `do` sumia.
eq "while: ; citado nao encerra a condicao" \
   'igual' \
   'x="a;b"; while [ "$x" = "a;b" ]; do echo igual; break; done'

# case tambem le lista de padroes ate `;;` — o `;` citado nao conta.
eq "case: ; citado nao vira ;;" \
   'match' \
   'case "a;b" in "a;b") echo match ;; *) echo miss ;; esac'

echo "=== controle: o operador NU continua funcionando (sem falso-negativo) ==="

eq "; nu ainda separa"           $'um\ndois' 'echo um; echo dois'
eq "| nu ainda faz pipe"         '2'         'printf "a\nb\n" | wc -l | tr -d " "'
eq "for com ; nu ainda roda"     $'1\n2'     'for i in 1 2; do echo $i; done'
eq "while com ; nu ainda roda"   'ok'        'i=0; while [ $i -lt 1 ]; do echo ok; i=1; done'

# `>` nu ainda redireciona de verdade.
"$J" -c "echo gravado > $TMP/saida.txt" >/dev/null 2>&1
if [ "$(cat "$TMP/saida.txt" 2>/dev/null)" = "gravado" ]; then
  ok "> nu ainda redireciona"
else
  bad "> nu ainda redireciona" "gravado" "$(cat "$TMP/saida.txt" 2>/dev/null)"
fi

echo "=== estrutural: nenhuma copia nova da maquininha de aspas ==="
# O ponto do JINV-116 e que a regra existe UMA vez. Se alguem colar outra
# varredura `quote = *p` num parser, este gate cai e explica o porque.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
copies=$(/usr/bin/grep -rlE "quote = \*p|quote == \*p" "$ROOT/c/parser" 2>/dev/null | /usr/bin/grep -v "quotes\.c$" | wc -l | tr -d ' ')
if [ "$copies" = "0" ]; then
  ok "a regra da aspa dupla mora so em c/parser/quotes.c"
else
  bad "a regra da aspa dupla mora so em c/parser/quotes.c" "0 copias" "$copies copias: $(/usr/bin/grep -rlE 'quote = \*p|quote == \*p' "$ROOT/c/parser" | /usr/bin/grep -v 'quotes\.c$' | tr '\n' ' ')"
fi

echo
echo "=== JINV-116/117: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ] || exit 1
exit 0
