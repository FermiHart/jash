#!/usr/bin/env bash
# JINV-033/034 — a família de casamento de padrão, contra o bash.
#
# O repositório tem QUATRO matchers de padrão: expansão de caminho (glob),
# `case`, `${var#pat}`/`${var%pat}`, e `[[ x == pat ]]`. Em vez de unificá-los
# às cegas, este gate MEDE — 27 casos-armadilha com o bash como oráculo. A
# medição achou exatamente dois defeitos, e nenhum deles era "os quatro
# divergem entre si":
#
# **JINV-033 — o glob perdia o espaço de nomes de arquivo.** Este bug foi
# introduzido pelo próprio JINV-009, que criou o sentinela 0x06 para que
# `echo *` não partisse "meu arquivo.txt" em duas palavras — mas não o
# acrescentou à lista de sentinelas do ramo de resgate em `eval.c`. Como o
# resultado de um glob não contém aspas nenhuma, `jash_has_quotes` era falso, o
# ramo de remoção de aspas não rodava, o resgate não reconhecia o 0x06, e o
# sentinela chegava ATÉ A SAÍDA:
#
#   $ echo *.txt                    (com um arquivo "com espaco.txt")
#   com<0x06>espaco.txt             exibido como "comespaco.txt"
#
# O nome do arquivo saía corrompido, em silêncio. `printf` não sofria (está em
# skip_unquote e desblinda por conta própria) — e foi essa discrepância entre
# dois builtins que denunciou o bug.
#
# **JINV-034 — `[[ x == [!abc] ]]` nunca casava.** O scanner de palavra do
# `[[ ]]` parava no `!` incondicionalmente, então o padrão chegava mutilado ao
# matcher. A forma `[^abc]` funcionava, e a mesma negação funcionava no `case`
# — a discrepância entre as três é que revelou o defeito.
#
# Lição que este gate carrega: **acrescentar um sentinela novo exige varrer
# TODOS os pontos que testam a família**, não só o que motivou a mudança.

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
( cd "$T" && touch a.txt b.txt c.log ab.txt A.txt .oculto 'com espaco.txt' 1.txt \
  && mkdir -p sub && touch sub/d.txt sub/e.log )

cmp_bash() { # cmp_bash <rotulo> <comando>
    local gb gj
    gb=$(cd "$T" && bash   -c "$2" 2>/dev/null | tr '\n' ' ' | tr -s ' ')
    gj=$(cd "$T" && "$JASH" -c "$2" 2>/dev/null | tr '\n' ' ' | tr -s ' ')
    if [ "$gb" = "$gj" ]; then
        ok "$1  → $gj"
    else
        bad "$1" "bash=[$gb]  jash=[$gj]"
    fi
}

# ── matcher 1: expansão de caminho ───────────────────────────────────────
cmp_bash "estrela simples"       'echo *.txt'
cmp_bash "estrela tudo"          'echo *'
cmp_bash "interrogação"          'echo ?.txt'
cmp_bash "classe [ab]"           'echo [ab].txt'
cmp_bash "faixa [a-c]"           'echo [a-c]*'
cmp_bash "negação [!a]"          'echo [!a]*.txt'
cmp_bash "negação [^a]"          'echo [^a]*.txt'
cmp_bash "subdiretório"          'echo sub/*.txt'
cmp_bash "sem correspondência"   'echo naoexiste*.zzz'
cmp_bash "ocultos não entram"    'echo *o*'
cmp_bash "duas estrelas"         'echo *.*'

# ── JINV-033: o espaço no NOME é dado, não separador ────────────────────
cmp_bash "nome com espaço (glob)"    'echo *espaco*'
cmp_bash "nome com espaço (printf)"  'printf "[%s]" *espaco*'
# E o sentinela NUNCA pode chegar à saída — 0x06 é invisível num diff casual.
saida=$(cd "$T" && "$JASH" -c 'echo *espaco*' 2>/dev/null)
if printf '%s' "$saida" | LC_ALL=C grep -q "$(printf '\006')"; then
    bad "sentinela não vaza para a saída" \
        "o byte 0x06 apareceu no stdout — o nome do arquivo sai corrompido"
else
    ok "o sentinela 0x06 não vaza para a saída"
fi

# ── matcher 2: case ─────────────────────────────────────────────────────
cmp_bash "case estrela"          'case abc in a*) echo casou;; *) echo nao;; esac'
cmp_bash "case classe"           'case b in [abc]) echo casou;; *) echo nao;; esac'
cmp_bash "case negação"          'case d in [!abc]) echo casou;; *) echo nao;; esac'
cmp_bash "case interrogação"     'case xy in ??) echo casou;; *) echo nao;; esac'
cmp_bash "case alternativa"      'case b in a|b) echo casou;; *) echo nao;; esac'
cmp_bash "case faixa"            'case m in [a-z]) echo casou;; *) echo nao;; esac'

# ── matcher 3: ${var#pat} / ${var%pat} ──────────────────────────────────
cmp_bash "prefixo curto"         'V=abc.txt; echo ${V#*.}'
cmp_bash "prefixo longo"         'V=a.b.c; echo ${V##*.}'
cmp_bash "sufixo curto"          'V=a.b.c; echo ${V%.*}'
cmp_bash "sufixo longo"          'V=a.b.c; echo ${V%%.*}'
cmp_bash "classe em prefixo"     'V=abc; echo ${V#[ab]}'

# ── matcher 4: [[ x == pat ]] ───────────────────────────────────────────
cmp_bash "[[ ]] estrela"         '[[ abc == a* ]] && echo casou || echo nao'
cmp_bash "[[ ]] classe"          '[[ b == [abc] ]] && echo casou || echo nao'
cmp_bash "[[ ]] negação com !"   '[[ d == [!abc] ]] && echo casou || echo nao'
cmp_bash "[[ ]] negação com ^"   '[[ d == [^abc] ]] && echo casou || echo nao'
cmp_bash "[[ ]] negação que NÃO casa" '[[ b == [!abc] ]] && echo casou || echo nao'
cmp_bash "[[ ]] literal citado"  '[[ "a*" == "a*" ]] && echo casou || echo nao'
cmp_bash "[[ ]] operador != intacto"  '[[ a != b ]] && echo casou || echo nao'
cmp_bash "[[ ]] negação unária intacta" '[[ ! -f /nao/existe ]] && echo casou || echo nao'

echo ""
echo "=== JINV-033/034: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
