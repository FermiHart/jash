#!/usr/bin/env bash
# lint — truncar em silêncio é proibido no parser.
#
# JINV-029. A família documentada em `audits/ACHADO-buffers-fixos.md` (oito
# invariantes, de JINV-021 a 028) existe porque este padrão foi usado como se
# fosse defensivo:
#
#     if (len >= TETO) len = TETO - 1;    /* corta e segue */
#
# Ele é o oposto de defensivo. Um buffer que estoura e para produz um
# resultado PLAUSÍVEL E ERRADO — e quando o corte cai numa linha de comando, o
# que se perde é o FINAL, onde moram o `>`, o `|` e o `&&`. Foi assim que o
# corpo de um heredoc virou comandos executados (JINV-022), que um `| wc -c`
# desapareceu (JINV-026) e que um `> arquivo` sumiu em 1 KB (JINV-028).
#
# A alternativa correta é sempre uma das duas: **crescer** ou **recusar alto**.
#
# Este lint não conserta nada. Ele obriga cada ocorrência a se declarar: ou
# some, ou entra na lista de exceções com uma justificativa escrita. É a
# catraca que impede as oito correções de serem desfeitas com o tempo — que é
# exatamente como esta família nasceu (o padrão de pilha do JINV-020 já existia
# correto em quatro varreduras; a quinta ficou de fora porque não havia gate).
#
# ──────────────────────────────────────────────────────────────────────────
# 2026-07-28 — POR QUE O TETO SUBIU DE 11 PARA 75 (JINV-091)
# ──────────────────────────────────────────────────────────────────────────
#
# Isto NÃO é uma regressão. É a primeira medição correta.
#
# De 2026-07-27 até hoje este lint rodou VERDE com um olho cego. A alternativa
# `sizeof(...)` do padrão estava escrita assim:
#
#     sizeof\([a-zA-Z_>.\[\]-]+\)
#
# e dentro de uma classe POSIX a barra invertida NÃO escapa: o `\]` FECHA a
# classe. O que sobrava depois dela (`-]+\)`) exigia um literal `-]` dentro do
# `sizeof(...)`. Esse ramo, portanto, **nunca casou com nada** — em nenhuma
# árvore, em nenhum commit, desde o dia em que foi escrito.
#
# O ramo consertado (`sizeof\([^);]*\)`) revela 79 sítios que o lint nunca
# reportou, entre eles `bl = sizeof(cp_body) - 1` e `cl = sizeof(vbuf) - 1` —
# cortes de 1023 e 2047 bytes em corpo de comando e em valor de variável, ou
# seja, exatamente a família que este arquivo existe para impedir.
#
# Números da virada (medidos, `bash tools/lint-no-silent-truncation.sh`):
#
#     antes (regex cego):   20 sítios ·  15 entradas ·  4 SEGURO · 11 DÍVIDA
#     depois (regex honesto): 99 sítios ·  83 entradas ·  5 SEGURO · 3 MORTO · 75 DÍVIDA
#
# A catraca continua valendo com o mesmo rigor de antes: 75 é o novo teto e ele
# **só desce**. Um teto que sobe sem explicação é sabotagem; este subiu porque o
# instrumento passou a enxergar, e a explicação está escrita aqui, com a data e
# o número anterior, para que ninguém possa repetir o truque em silêncio.
#
# A lição operacional está na CALIBRAGEM abaixo: um lint que não prova a cada
# execução que consegue ver o que procura é indistinguível de um lint que passou.
#
# Uso:
#   bash tools/lint-no-silent-truncation.sh              # varredura + catraca
#   bash tools/lint-no-silent-truncation.sh --check A.c  # veredito por arquivo
#                                                        # (rc=1 se acusa, 0 se absolve)
# Saída: 0 se nada novo apareceu; 1 e a lista se apareceu.

set -u
cd "$(dirname "$0")/.." || exit 2

# A lista de exceções é sobreponível SÓ para que o gate consiga provar que a
# catraca reprova (JINV029_ALLOW aponta para uma cópia adulterada). Nenhum
# caminho de produção passa essa variável — se ela aparecer num Makefile ou num
# CI, é contorno, não configuração. Mesma regra do lint-pointer-advance.sh.
ALLOW="${JINV029_ALLOW:-tools/lint-no-silent-truncation.allow}"
SCOPE="c/parser c/core c/builtins"

# O padrão perigoso: uma atribuição que ENCOLHE um comprimento para caber,
# dentro de (ou logo após) um teste de estouro. `\bslen =`, `len =`, `n =` etc.
# seguidos de `TETO - 1`, onde TETO é `sizeof(...)`, um macro em CAIXA ALTA ou
# um literal numérico.
#
# O ramo do `sizeof` usa `[^);]*` de propósito: a versão anterior tentou listar
# os caracteres permitidos numa classe, errou o escape do `]` e ficou cega por
# semanas (ver o cabeçalho). Uma negação simples não tem como ser escrita
# errada em silêncio — e o `)` que ela exclui é o próprio delimitador.
#
# O padrão TAMBÉM perdeu a âncora `^[^*/]*` que tinha antes. Ela existia para
# pular linha de continuação de comentário de bloco (` * len = X - 1;`), mas
# cegava de quebra QUALQUER linha com `*` ou `/` antes da atribuição — um
# `if (*p) n = sizeof(b) - 1;` era invisível pelo mesmo motivo que o ramo do
# sizeof era: um pedaço do padrão que não casa e não avisa. Hoje a árvore não
# tem nenhum caso desses (medido: 99 sítios com e sem a âncora), então fechar o
# buraco custou zero — e o comentário de bloco virou um filtro explícito
# abaixo, que é onde essa regra devia estar desde o começo.
PAT='\b[a-z_]+ *= *(sizeof\([^);]*\)|[A-Z_]{3,}|[0-9]+) *- *1 *;'

# ──── CALIBRAGEM: o lint prova, a cada execução, que ENXERGA ────
# JINV-091. O defeito que este bloco impede não é hipotético: foi assim que a
# dívida real (99 sítios) passou por 11 durante semanas. Um instrumento que não
# se calibra não distingue "a árvore está limpa" de "eu estou cego".
# Três formas canônicas que TÊM que casar, uma por ramo do padrão:
cal_ok=1
for cal in \
    '    if (n >= sizeof(cal_buf)) n = sizeof(cal_buf) - 1;' \
    '    if (n >= sizeof(s->cal[i].k)) n = sizeof(s->cal[i].k) - 1;' \
    '    if (n >= CAL_MAX) n = CAL_MAX - 1;' \
    '    if (n >= 4096) n = 4096 - 1;' \
    '    if (*p && n >= sizeof(cal_buf)) n = sizeof(cal_buf) - 1;'
do
    printf '%s\n' "$cal" | grep -qE "$PAT" || {
        echo "✗ o lint está CEGO: a forma canônica abaixo não casa com o padrão."
        echo "    $cal"
        cal_ok=0
    }
done
# E uma que NÃO pode casar — senão "consertar" o padrão vira casar com tudo,
# que é a outra maneira de um lint não servir para nada.
if printf '%s\n' '    total = base + 1;' | grep -qE "$PAT"; then
    echo "✗ o lint virou ruído: casou com uma linha que não trunca nada."
    cal_ok=0
fi
[ "$cal_ok" = "1" ] || { echo "  (conserte tools/lint-no-silent-truncation.sh — ver JINV-091)"; exit 2; }

# `scan ARQUIVOS...` — a varredura, com os mesmos filtros em qualquer modo.
# Linhas comentadas não contam, e uma linha que cita o próprio invariante
# (JINV-021..039) é documentação, não código novo.
#
# O terceiro filtro (` * texto`) é a antiga âncora `^[^*/]*` do padrão, agora
# explícita: continuação de comentário de bloco não tem `//` nem `/*` na linha,
# então precisa de uma regra própria — mas ela olha só o INÍCIO da linha, e não
# cega o resto dela.
scan() {
    grep -HrnE "$PAT" --include='*.c' "$@" 2>/dev/null \
      | grep -vE '//|/\*' \
      | grep -vE '^[^:]+:[0-9]+:[[:space:]]*\*' \
      | grep -viE 'JINV-0(2[1-9]|3[0-9])' \
      | sed 's/:[[:space:]]*/:/' \
      | sort
}

# ──── modo --check: veredito por arquivo, sem catraca e sem lista de exceções ────
# É o que o gate usa nas fixtures: rc=1 quando o arquivo tem truncamento, rc=0
# quando não tem. Sem isto o gate só provaria que o lint RODA; com isto ele
# prova que o lint DISCRIMINA.
if [ "${1:-}" = "--check" ]; then
    shift
    [ $# -gt 0 ] || { echo "uso: bash $0 --check ARQUIVO..." >&2; exit 2; }
    saida=$(scan "$@")
    if [ -n "$saida" ]; then
        printf '%s\n' "$saida"
        exit 1
    fi
    echo "✓ nenhum truncamento silencioso"
    exit 0
fi

hits=$(scan $SCOPE)

[ -f "$ALLOW" ] || : > "$ALLOW"

# A lista de exceções guarda "arquivo:trecho" (sem número de linha, que muda a
# cada edição). Cada entrada precisa de um comentário `# porque:` na linha
# anterior — uma exceção sem justificativa é dívida escondida.
#
# Uma entrada pode cobrir MAIS DE UM sítio (mesmo texto, linhas diferentes): as
# 83 entradas correspondem a 99 sítios. O número de sítios de cada entrada está
# escrito na justificativa quando é maior que um — a chave é grossa de
# propósito (imune a renumeração), mas o custo real fica visível.
norm() { sed -E 's/^([^:]+):[0-9]+:/\1: /' ; }

atual=$(printf '%s\n' "$hits" | norm | sed '/^$/d' | sort -u)
permitido=$(grep -vE '^\s*#|^\s*$' "$ALLOW" 2>/dev/null | sort -u)

novos=$(comm -23 <(printf '%s\n' "$atual") <(printf '%s\n' "$permitido"))
mortos=$(comm -13 <(printf '%s\n' "$atual") <(printf '%s\n' "$permitido"))

rc=0
if [ -n "$novos" ]; then
    echo "✗ truncamento silencioso NOVO (cresça ou recuse alto — ver ACHADO-buffers-fixos.md):"
    printf '%s\n' "$novos" | sed 's/^/    /'
    echo "  Se truncar ali for mesmo seguro, acrescente a linha em $ALLOW"
    echo "  precedida de um comentário '# porque: <razão>'."
    rc=1
fi
if [ -n "$mortos" ]; then
    echo "✗ entradas obsoletas em $ALLOW (o código sumiu — remova-as):"
    printf '%s\n' "$mortos" | sed 's/^/    /'
    rc=1
fi

# ──── catraca: a seção DÍVIDA só encolhe ────
# Uma exceção SEGURO é permanente (clampar índice não perde dado). Uma exceção
# MORTO é inalcançável (candidata a remoção, não a conserto). Uma exceção
# DÍVIDA perde dado e está ali só até alguém consertar. Se o número subir,
# alguém acrescentou perda silenciosa e a DECLAROU em vez de corrigir.
#
# 2026-07-28: 11 → 75. NÃO é regressão — é a primeira medição com o regex
# enxergando (o ramo `sizeof(...)` nunca casou; ver o cabeçalho e JINV-091).
# A partir daqui vale a mesma regra de sempre: este número só desce.
#
# 2026-08-04: 75 → 72. JINV-082/090 trocou os buffers fixos de argumento
# expandido em pipeline.c por `jash_param_expand` (aloca), então três entradas
# ficaram órfãs. A catraca as recusou e cobrou este número — foi o lint que
# descobriu que a dívida tinha sido paga, não o autor do conserto.
# 2026-08-11: 72 → 66. JINV-103/buffer fixos do jash_brain usam heap para val/tag
# e recusam alto para key; os padrões expr_buf de eval/arithmetic/for também foram
# movidos para heap. A catraca exigiu a baixa do teto — foi o lint que descobriu.
# 2026-08-16: 66 → 64. Globstar e a lista de palavras do `for` agora crescem
# dinamicamente; a catraca recusou as duas exceções que ficaram órfãs.
# 2026-08-16: 64 → 61. Os três caminhos de argumentos de função agora usam a
# mesma tokenização dinâmica, sem fragmentar valores em 8/4 KiB.
# 2026-09-14: 61 → 60. Removida exceção obsoleta de root_buf em digest;
# o trecho já não existia em HEAD ao qualificar os mutadores W03.
# 2026-09-19: 60 → 59. O `cmd &` acima de 1023 bytes não trunca mais em
# silêncio (buffer de tamanho exato no bloco background de eval.c);
# a exceção DÍVIDA #7 ficou órfã e foi removida com ela.
# 2026-10-20: 59 → 58. json now uses the strict bounded AST; its obsolete line-clipping
# allowance is removed. The length-aware number writer preserves every byte.
# 2026-10-20: 58 → 57. subshell bodies now retain their full allocation and use the
# ordinary evaluator; the obsolete 2047-byte clipping allowance is removed.
DEBT_CEILING=57
# A seção DÍVIDA é a ÚLTIMA do arquivo de propósito: a contagem vai daqui até o
# fim, então SEGURO e MORTO precisam vir antes para não serem contados.
#
# O marcador é o token fixo `# @@DIVIDA@@` sozinho numa linha, não o título em
# prosa. A lição é emprestada do JINV-061, que já pagou por ela: um contador
# ancorado na prosa devolve ZERO tanto quando a seção está vazia quanto quando
# alguém trocou um travessão — e as duas respostas são idênticas na saída.
# Medido neste arquivo: com o título em prosa, `awk` casava o marcador e
# `grep -E` NÃO casava, no MESMO arquivo. Por isso o token, e por isso a falta
# dele é um erro barulhento em vez de um zero silencioso.
if ! grep -q '^# @@DIVIDA@@$' "$ALLOW"; then
    echo "✗ o marcador '# @@DIVIDA@@' sumiu de $ALLOW"
    echo "  Sem ele a dívida conta 0 e a catraca vira decoração — que é"
    echo "  exatamente como um instrumento cego parece um instrumento limpo."
    exit 2
fi
n_debt=$(awk '/^# @@DIVIDA@@$/{f=1;next} f && $0 !~ /^[[:space:]]*#/ && NF' "$ALLOW" | wc -l | tr -d ' ')
if [ "$n_debt" -gt "$DEBT_CEILING" ]; then
    echo "✗ a dívida de truncamento SUBIU: $n_debt (teto $DEBT_CEILING)"
    echo "  Esta seção só encolhe. Conserte em vez de declarar."
    rc=1
elif [ "$n_debt" -lt "$DEBT_CEILING" ]; then
    echo "✗ a dívida caiu para $n_debt — abaixe DEBT_CEILING para $n_debt neste mesmo commit"
    rc=1
fi

# ──── toda exceção precisa de justificativa escrita ────
sem_porque=$(awk '/^[[:space:]]*#/ { if ($0 ~ /porque:/) seen=1; next } /^[[:space:]]*$/ { seen=0; next } { if (!seen) print "    " $0; seen=0 }' "$ALLOW")
if [ -n "$sem_porque" ]; then
    echo "✗ exceção sem '# porque:' na linha anterior (dívida escondida):"
    printf '%s\n' "$sem_porque"
    rc=1
fi

n_atual=$(printf '%s\n' "$atual" | sed '/^$/d' | wc -l | tr -d ' ')
n_sitios=$(printf '%s\n' "$hits" | sed '/^$/d' | wc -l | tr -d ' ')
n_morto=$(awk '/^# @@MORTO@@$/{f=1;next} /^# @@DIVIDA@@$/{f=0} f && $0 !~ /^[[:space:]]*#/ && NF' "$ALLOW" | wc -l | tr -d ' ')
if [ "$rc" -eq 0 ]; then
    echo "✓ nenhum truncamento silencioso novo ($n_sitios sítios · $n_atual entradas · seguro: $((n_atual - n_debt - n_morto)) · morto: $n_morto · dívida: $n_debt)"
fi
exit $rc
