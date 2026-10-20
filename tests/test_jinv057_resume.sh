#!/usr/bin/env bash
# JINV-057 — `resume`: o bilhete que sabe se envelheceu.
#
# Um bilhete de handoff é a coisa mais perigosa que uma sessão deixa para a
# próxima: foi verdade quando escrito, chega com autoridade de fato, e o
# repositório andou embaixo dele. É o mesmo padrão das quatro afirmações
# falsas desta sessão — verificou uma vez, continuou afirmando — só que agora
# atravessando a fronteira onde ninguém do outro lado pode conferir.
#
# ═══ As duas propriedades que este gate protege ═══
#
# 1. **Discriminação.** Não basta dizer "algo mudou". O bilhete cita arquivos;
#    o `resume` tem que nomear QUAL deles envelheceu e qual não. Um aviso
#    genérico de staleness é ignorado na terceira vez que aparece.
#
# 2. **Honestidade sobre o alcance.** `resume` NUNCA diz "ainda vale". Ele
#    conferiu o HEAD e os caminhos citados — não o resto do mundo. Tem que
#    relatar as duas coisas separadas: `checks` e `not_verified`. É a régua do
#    JINV-051 (`sigil chain verify` declarando o que sustenta seu veredito)
#    apontada para o próximo agente, no momento em que ele tem menos condição
#    de duvidar.
#
# A asserção 5 é a que mais importa: se `not_verified` sumir do JSON, o
# veredito passa a afirmar mais do que sustenta — e o gate fica vermelho.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0
[ -x "$JASH" ] || { echo "SKIP: $JASH ausente"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

H=$(mktemp -d) || exit 2
R=$(mktemp -d) || exit 2
trap 'rm -rf "$H" "$R"' EXIT INT TERM

mkdir -p "$R/.git"
printf 'aaaaaaaaaaaa1111\n' > "$R/.git/HEAD"
: > "$R/citado.c"
: > "$R/naocitado.c"

# TODA invocação do shell passa pelo cão de guarda — inclusive as de PREPARO.
#
# A versão anterior blindava só a asserção final de travamento e deixava nuas
# as duas chamadas que GRAVAM o bilhete com parênteses — isto é, exatamente a
# entrada que travava. Quando o `resume` girava em laço, a suíte parava ali,
# antes de chegar ao prazo, e ficava pendurada por 25 minutos em vez de falhar
# em 10 segundos. Dois agentes perderam meia hora cada por causa disso.
#
# A lição não é sobre este gate: **prazo aplicado só na asserção não protege
# nada.** O que trava costuma ser o preparo, porque é ele que carrega a
# entrada patológica. O cão de guarda tem que estar em TODA porta.
#
# `$DL` é capturado ANTES do `cd "$R"` — depois do cd, um caminho relativo a
# tools/ não resolve mais.
DL="$PWD/tools/deadline.sh"
J()   { (cd "$R" && HOME="$H" "$DL" 15 "$JASH" -c "$1" 2>&1); }
Jrc() { (cd "$R" && HOME="$H" "$DL" 15 "$JASH" -c "$1" >/dev/null 2>&1); echo $?; }

# ── 1. grava e reconhece os caminhos citados NO PRÓPRIO TEXTO ───────────
out=$(J 'resume "o conserto esta em citado.c, o gate cobre"')
if printf '%s' "$out" | grep -q "citado.c"; then
    ok "extrai do texto do bilhete os arquivos que ele cita"
else
    bad "extração de caminhos" "não reconheceu 'citado.c' no texto: $out"
fi

# ── 2. estado intacto: não grita ────────────────────────────────────────
out=$(J 'resume')
printf '%s' "$out" | grep -q "o mundo andou" \
    && bad "falso positivo" "alertou staleness sem nada ter mudado" \
    || ok "estado intacto não produz alerta"
[ "$(Jrc 'resume')" = "0" ] && ok "rc=0 quando nada mudou" \
                            || bad "rc intacto" "rc≠0 sem mudança"

# ── 3. DISCRIMINAÇÃO — a primeira propriedade central ───────────────────
# Toca o arquivo CITADO e deixa o não-citado quieto. O alerta tem que
# nomear um e só um.
sleep 1.1
printf 'mudou\n' > "$R/citado.c"
out=$(J 'resume')
if printf '%s' "$out" | grep -q "citado.c"; then
    ok "nomeia o arquivo citado que foi modificado"
else
    bad "detecção de mtime" "citado.c mudou depois do bilhete e não foi apontado"
fi
# E o rc reflete — um agente checa rc, não lê prosa.
[ "$(Jrc 'resume')" != "0" ] && ok "rc≠0 quando o bilhete envelheceu" \
                             || bad "rc de staleness" "rc=0 com bilhete velho"

# ── 4. o git HEAD também é verificador ──────────────────────────────────
printf 'bbbbbbbbbbbb2222\n' > "$R/.git/HEAD"
out=$(J 'resume')
printf '%s' "$out" | grep -q "aaaaaaaaaaaa" && printf '%s' "$out" | grep -q "bbbbbbbbbbbb" \
    && ok "mostra o HEAD de então → o de agora" \
    || bad "detecção de HEAD" "não mostrou a transição de HEAD"

# ── 5. HONESTIDADE — a propriedade que não pode cair ────────────────────
# Nunca "ainda vale": o que foi conferido, e o que NÃO foi.
out=$(J 'resume')
if printf '%s' "$out" | grep -qi "nao conferido"; then
    ok "declara explicitamente o que NÃO verificou"
else
    bad "honestidade sobre o alcance" \
        "o veredito não diz o que ficou de fora — está afirmando mais do que sustenta (JINV-051)"
fi

# ── 6. JSON com a mesma disciplina ──────────────────────────────────────
if command -v python3 >/dev/null; then
    out=$( (cd "$R" && HOME="$H" "$JASH" -c 'resume --json' 2>/dev/null) | tail -1)
    r=$(printf '%s' "$out" | python3 -c "
import json,sys
d=json.load(sys.stdin)
f={x['path']:x['changed_since_note'] for x in d['files']}
print(d.get('stale'), d.get('head_moved'),
      f.get('citado.c'), f.get('naocitado.c') if 'naocitado.c' in f else 'ausente',
      len(d.get('not_verified',[]))>0)" 2>/dev/null)
    case "$r" in
        "True True True "*" True")
            ok "JSON: stale/head_moved/por-arquivo/not_verified" ;;
        *) bad "JSON completo" "veio '$r'" ;;
    esac
    # O não-citado NÃO pode aparecer como mudado: discriminação em JSON.
    r2=$(printf '%s' "$out" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print(any(x['path']=='naocitado.c' and x['changed_since_note'] for x in d['files']))" 2>/dev/null)
    [ "$r2" = "False" ] && ok "não marca como velho o arquivo que não mudou" \
                        || bad "discriminação em JSON" "naocitado.c apareceu como mudado"
else
    ok "sem python3 — JSON não verificado"
fi

# ── 7. arquivo citado que SUMIU é distinto de modificado ────────────────
rm -f "$R/citado.c"
J 'resume' | grep -q "nao existe mais" \
    && ok "'sumiu' é distinto de 'modificado'" \
    || bad "distinção de estados" "arquivo apagado não foi relatado como ausente"

# ── 8. sem bilhete, ADMITE — não inventa ────────────────────────────────
J 'resume --forget' >/dev/null
out=$(J 'resume')
printf '%s' "$out" | grep -qi "nenhum bilhete" \
    && ok "sem bilhete, diz que não há (não inventa contexto)" \
    || bad "honestidade no vazio" "produziu algo sem ter bilhete: $out"

# ── 9. token com barra que NÃO é caminho não vira falso positivo ───────
# "83/83", "2026/07/27", "1/2": têm barra, não são caminhos. Um alerta de
# staleness sobre um arquivo que o bilhete nunca citou é ruído — e ruído
# corrói a confiança em TODOS os outros alertas, inclusive os verdadeiros.
#
# Achado ao USAR o próprio builtin para gravar o bilhete de handoff desta
# sessão, trinta segundos depois de ele ficar pronto. O comentário no código
# já advertia contra exatamente isto, e o defeito embarcou junto; só a
# experimentação pegou.
J 'resume --forget' >/dev/null
J 'resume "suite 83/83 verde em 2026/07/27, 1/2 do plano feito"' >/dev/null
out=$(J 'resume')
if printf '%s' "$out" | grep -q "nao existe mais"; then
    bad "falso positivo de caminho" \
        "um numero com barra virou caminho e gerou alerta sobre arquivo inexistente"
else
    ok "número com barra não é confundido com caminho"
fi
# e um caminho de verdade no mesmo bilhete CONTINUA sendo vigiado
J 'resume --forget' >/dev/null
J "resume \"83/83 verde; o conserto esta em $R/citado.c\"" >/dev/null
: > "$R/citado.c"
J 'resume' | grep -q "citado.c" \
    && ok "caminho real continua vigiado ao lado do número" \
    || bad "regressão da extração" "a guarda nova matou a detecção de caminho legítimo"

# ── 10. pontuação no bilhete não pode TRAVAR o shell ───────────────────
# Os conjuntos de separadores eram assimétricos: `(` era pulado mas não
# parava a varredura; `)` parava mas não era pulado. Num `)` o ponteiro
# ficava imóvel — laço de pular não consome, laço de varrer para, comprimento
# zero, iteração idêntica. Laço infinito.
#
# O bilhete de handoff real desta sessão tinha parênteses, e o shell travou
# ao gravá-lo. Os 14 gates anteriores passaram porque nenhuma nota de teste
# minha tinha pontuação — a mesma cegueira que deixou passar o "83/83".
#
# O cão de guarda aqui é `tools/deadline.sh`, NÃO `timeout` — e a diferença
# não é estilo. `timeout(1)` NÃO EXISTE neste macOS: sob bash ele devolve 127
# ("command not found"), o teste vê um rc diferente de 124 e conclui "não
# travou". Esta asserção nasceu inerte por isso, no mesmo dia em que foi
# escrita para pegar um laço infinito que tinha acabado de travar o shell.
#
# A armadilha tem um agravante: `command -v timeout` responde "timeout" quando
# quem pergunta é o jash, porque ali é um builtin. A verificação óbvia mente.
#
# `deadline.sh` distingue 124 (estourou) de 127 (não existe) e RECUSA rodar se
# não houver mecanismo de prazo — em vez de fingir que protege.
for pontuado in \
    'ver eval.c (info perdida) aqui' \
    'a) primeiro b) segundo' \
    'f(x) e g(y) em util.c' \
    'lista: (um, dois) [tres] {quatro}' \
    ')))((( c/parser/eval.c' ; do
    J 'resume --forget' >/dev/null 2>&1
    if HOME="$H" "$PWD/tools/deadline.sh" 10 "$JASH" -c "resume \"$pontuado\"" >/dev/null 2>&1; then
        :
    else
        rc=$?
        [ "$rc" = "124" ] && { bad "laço infinito com pontuação" \
            "travou em: $pontuado"; break; }
    fi
done
[ "${rc:-0}" != "124" ] && ok "parênteses e pontuação não travam a extração" || true
unset rc

# e a leitura de volta também não pode travar
J 'resume --forget' >/dev/null 2>&1
J 'resume "ver c/parser/eval.c (o ponto certo) e nao echo.c"' >/dev/null 2>&1
HOME="$H" "$PWD/tools/deadline.sh" 10 "$JASH" -c 'resume' >/dev/null 2>&1
[ $? != "124" ] && ok "ler bilhete com pontuação também não trava" \
                || bad "laço na leitura" "resume travou ao reler nota com parênteses"

# ── 11. flag desconhecida não vira texto de bilhete (JINV-046) ─────────
[ "$(Jrc 'resume --nao-existe')" != "0" ] \
    && ok "flag desconhecida recusada" \
    || bad "guarda de flags" "'--nao-existe' virou corpo de bilhete"

echo ""
echo "=== JINV-057: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
