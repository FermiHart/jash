#!/usr/bin/env bash
# JINV-069 — `-n` não pode devolver 0 sem ter checado nada.
#
# ═══ A PROPRIEDADE PROTEGIDA ═══
#
# `-n` (noexec) existe para EMITIR UM VEREDITO: "este arquivo é sintaticamente
# válido". Um veredito só vale se puder ser negativo. Enquanto o jash respondia
# 0 para tudo — inclusive para arquivos que não existem — ele não estava
# aprovando scripts, estava produzindo um carimbo constante.
#
#   $ jash -n /tmp/if_sem_fi.sh   ; echo $?     # sintaticamente inválido
#   0
#   $ jash -n /tmp/NAO_EXISTE.sh  ; echo $?     # o arquivo nunca foi aberto
#   0
#
# Um arranjo de teste construído sobre `jash -n` reportaria **100 % de
# aprovação em qualquer corpus**. Alguém quase construiu um, hoje.
#
# ═══ POR QUE A CORREÇÃO É RECUSAR, E NÃO IMPLEMENTAR ═══
#
# Decidido com medição, não por preguiça:
#
#  1. **O jash não separa análise de execução.** Não existe passe de parse:
#     os avaliadores de if/for/while/case leem e executam no MESMO passo
#     (c/parser/{if_block,for_loop,while_loop,case_stmt}.c). Não há estágio
#     onde perguntar "isto é válido?" sem rodar.
#
#  2. **A envelopagem que pareceria salvar já foi medida — e EXECUTA.** A
#     auditoria de 2 176 scripts reais envelopou cada um em
#     `if false; then <script>; fi` justamente para forçar análise sem
#     execução. Em **773 de 2 158** casos o jash perdeu a sincronia dentro do
#     bloco e passou a rodar o texto como comando — houve tentativa real de
#     executar `python3`, `qemu-system-x86_64`, `docker … sudo`
#     (audits/BENCHMARK-scripts-reais.md §3.4). Um `-n` construído assim seria
#     uma ferramenta de checagem que executa o que deveria só ler.
#
#  3. **Mesmo um verificador de sintaxe perfeito prometeria demais.** O jash
#     ainda executa ERRADO scripts perfeitamente válidos (comentário no fim da
#     linha dentro de bloco, §4.1 da mesma auditoria: 208 scripts). Um rc 0 de
#     `-n` seria lido como "o jash roda isto", que é falso para boa parte do
#     corpus.
#
# Então `-n` RECUSA, alto, com rc 2 e uma mensagem que diz o porquê e aponta
# `bash -n`. Uma recusa alta custa um segundo; um carimbo constante custa toda
# a confiança que for construída sobre ele.
#
# ═══ COMO ESTE GATE MEDE ═══
#
# `/bin/bash` é o ORÁCULO — mas de um jeito específico, porque aqui o jash
# DIVERGE dele de propósito. O bash é usado para provar que cada arquivo de
# teste é o que diz ser (inválido é de fato inválido, válido é de fato
# válido); o jash é então medido contra a propriedade: **nunca rc 0, e nunca
# executar**. As duas metades são falsificáveis.
#
# Gate: bash tests/test_jinv069_noexec_refusal.sh

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

J() { HOME="$H" JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off \
      "$DL" 15 "$JASH" "$@" >"$W/j.out" 2>"$W/j.err" </dev/null; echo $?; }
B() { HOME="$H" "$DL" 15 "$BASH_BIN" "$@" >"$W/b.out" 2>"$W/b.err" </dev/null; echo $?; }

# ── os arquivos de prova, e o oráculo confirmando o que cada um é ────────
printf 'echo hi\nif true\n'        > "$W/invalido.sh"   # if sem fi
printf 'echo )\n'                  > "$W/invalido2.sh"  # parêntese órfão
printf 'fi\n'                      > "$W/invalido3.sh"  # fechador solto
printf 'echo VALIDO\n'             > "$W/valido.sh"
AUSENTE="$W/NAO_EXISTE_JINV069.sh"

brc=$(B -n "$W/invalido.sh")
[ "$brc" != "0" ] && ok "oráculo: bash -n reprova 'if' sem 'fi' (rc=$brc)" \
                  || bad "oráculo do inválido" "bash -n aceitou um script inválido (rc=$brc)"
brc_ok=$(B -n "$W/valido.sh")
[ "$brc_ok" = "0" ] && ok "oráculo: bash -n aprova o script válido (rc=0)" \
                    || bad "oráculo do válido" "bash -n reprovou script válido (rc=$brc_ok)"
brc_abs=$(B -n "$AUSENTE")
[ "$brc_abs" != "0" ] && ok "oráculo: bash -n reprova arquivo inexistente (rc=$brc_abs)" \
                      || bad "oráculo do ausente" "bash -n aceitou arquivo inexistente"

# ── 1. O PIOR CASO: script inválido NÃO pode sair 0 ─────────────────────
for f in invalido invalido2 invalido3; do
    jrc=$(J -n "$W/$f.sh")
    if [ "$jrc" = "0" ]; then
        bad "'-n $f.sh' não sai 0" \
            "rc=0 num arquivo que o bash reprova — carimbo constante, o defeito"
    else
        ok "'-n $f.sh' reprova (rc=$jrc), como o bash"
    fi
done

# ── 2. E o arquivo INEXISTENTE também não ───────────────────────────────
jrc=$(J -n "$AUSENTE")
[ "$jrc" != "0" ] && ok "'-n ARQUIVO_INEXISTENTE' reprova (rc=$jrc)" \
                  || bad "'-n' em arquivo ausente" "rc=0 — o arquivo nunca foi sequer aberto"

# ── 3. A RECUSA É EXPLÍCITA — e diz o que fazer ─────────────────────────
# Reprovar sem explicar produz um segundo mistério no lugar do primeiro.
jrc=$(J -n "$W/valido.sh"); jerr=$(cat "$W/j.err")
if [ "$jrc" = "0" ]; then
    bad "'-n' nunca sai 0 calado" \
        "rc=0 para um script válido — é exatamente a aprovação que este shell não pode sustentar"
else
    ok "'-n' em script VÁLIDO também recusa (rc=$jrc): não finge um veredito"
fi
if printf '%s' "$jerr" | grep -qi "noexec"; then
    ok "a mensagem nomeia o recurso (noexec)"
else
    bad "mensagem nomeia o recurso" "stderr não cita noexec: $(printf '%s' "$jerr" | head -c 90)"
fi
if printf '%s' "$jerr" | grep -q "bash -n"; then
    ok "a mensagem aponta a ferramenta que faz o serviço (bash -n)"
else
    bad "mensagem aponta saída" "stderr não sugere 'bash -n': $(printf '%s' "$jerr" | head -c 90)"
fi

# ── 4. RECUSAR NÃO PODE VIRAR EXECUTAR ──────────────────────────────────
# Se `-n` rodasse o script, a "checagem" teria efeito colateral — o mesmo
# desastre da envelopagem `if false`. O arquivo abaixo deixa rastro no disco.
printf 'echo EFEITO > "%s/marca"\n' "$W" > "$W/efeito.sh"
rm -f "$W/marca"
jrc=$(J -n "$W/efeito.sh")
if [ -e "$W/marca" ]; then
    bad "'-n' não executa" "o script rodou sob -n e criou $W/marca (rc=$jrc)"
else
    ok "'-n' não executou o script (nenhum efeito colateral)"
fi
# controle: sem `-n` o mesmo script DEIXA o rastro — senão a asserção acima
# passaria por o script ser inerte, não por o -n ter recusado.
rm -f "$W/marca"
jrc2=$(J "$W/efeito.sh")
if [ -e "$W/marca" ]; then
    ok "controle: sem '-n' o mesmo script executa e deixa rastro (rc=$jrc2)"
else
    bad "controle do efeito" "o script não deixa rastro nem sem -n (rc=$jrc2) — asserção acima é vazia"
fi

# ── 5. `set -n` (a forma POSIX) tem a MESMA resposta ────────────────────
# Antes: `jash: set: unknown option: -n` … e o script continuava rodando.
# Executar MAIS do que o bash executaria é o lado perigoso da divergência.
printf 'set -n\necho NAO_DEVE_SAIR\n' > "$W/setn.sh"
jrc=$(J "$W/setn.sh"); jout=$(cat "$W/j.out"); jerr=$(cat "$W/j.err")
if [ "$jrc" = "0" ]; then
    bad "'set -n' reprova" "rc=0 — a opção foi aceita ou ignorada"
else
    ok "'set -n' reprova (rc=$jrc)"
fi
if printf '%s' "$jout" | grep -q "NAO_DEVE_SAIR"; then
    bad "'set -n' interrompe" \
        "o comando DEPOIS de 'set -n' executou — o bash não executaria nada dali em diante"
else
    ok "nada depois de 'set -n' executa (como no bash, que também não executaria)"
fi
if printf '%s' "$jerr" | grep -qi "noexec"; then
    ok "'set -n' dá a MESMA mensagem que '-n' (uma porta só)"
else
    bad "mensagem única" "'set -n' não cita noexec: $(printf '%s' "$jerr" | head -c 90)"
fi
# `set -o noexec`, a forma longa, idem.
jrc=$(J -c 'set -o noexec'); jerr=$(cat "$W/j.err")
if [ "$jrc" != "0" ] && printf '%s' "$jerr" | grep -qi "noexec"; then
    ok "'set -o noexec' recusa com a mesma mensagem (rc=$jrc)"
else
    bad "'set -o noexec'" "rc=$jrc / stderr=$(printf '%s' "$jerr" | head -c 60)"
fi
# e `-o noexec` na invocação, idem.
jrc=$(J -o noexec -c 'echo NAO'); jout=$(cat "$W/j.out")
if [ "$jrc" != "0" ] && [ -z "$jout" ]; then
    ok "'-o noexec' na invocação recusa e não executa o -c (rc=$jrc)"
else
    bad "'-o noexec' na invocação" "rc=$jrc out='$jout'"
fi

# ── 6. A RECUSA É CIRÚRGICA: as vizinhas continuam válidas ──────────────
# Uma guarda que barra o uso legítimo troca um bug por outro (JINV-046).
# `-m` e `-b` são letras vizinhas no cluster do bash; nenhuma pode cair junto.
for f in -e -u -x -v -f -a -b -C -m -h -k -p -t; do
    jrc=$(J "$f" -c 'echo OK'); jout=$(cat "$W/j.out")
    brc=$(B "$f" -c 'echo OK'); bout=$(cat "$W/b.out")
    if [ "$bout" != "OK" ]; then
        bad "oráculo de '$f'" "bash não imprimiu OK — caso inválido"
    elif [ "$jout" = "$bout" ] && [ "$jrc" = "$brc" ]; then
        : # silencioso: o JINV-067 já conta estas; aqui só não podem quebrar
    else
        bad "recusa cirúrgica" "'$f' caiu junto com o -n: jash out='$jout' rc=$jrc ≠ bash '$bout' rc=$brc"
    fi
done
ok "nenhuma das outras 13 letras foi derrubada junto com o -n"

echo ""
echo "=== JINV-069: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
