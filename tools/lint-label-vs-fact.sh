#!/usr/bin/env bash
# lint-label-vs-fact.sh — W6
#
# Recusa o padrao que mais custou a este projeto: confiar no ROTULO em vez do
# FATO. Toda regra aqui nasceu de um incidente registrado, nao de gosto — um
# lint que so proibe o que ja mordeu e um lint que ninguem desativa.
#
# Uso:  bash tools/lint-label-vs-fact.sh [--all]
#       sem --all, olha so o que mudou contra origin/main (ou o ultimo commit)
#
# F E R M I ∞ H A R T

cd "$(dirname "$0")/.." || exit 2
ALLOW=tools/lint-label-vs-fact.allow
fail=0
report() {  # report <regra> <arquivo:linha> <trecho> <por que>
    grep -qF -- "$1: $3" "$ALLOW" 2>/dev/null && return
    fail=$((fail + 1))
    printf '\033[31m✗\033[0m %s\n  %s\n  %s\n  POR QUE: %s\n\n' "$1" "$2" "$3" "$4"
}

if [ "${1:-}" = "--all" ]; then
    FILES=$(git ls-files '*.sh' '*.c' '*.h' 'CMakeLists.txt' '**/CMakeLists.txt' 2>/dev/null)
else
    BASE=$(git merge-base HEAD origin/main 2>/dev/null || echo HEAD~1)
    FILES=$(git diff --name-only --diff-filter=ACM "$BASE" 2>/dev/null \
            | grep -E '\.(sh|c|h)$|CMakeLists\.txt$')
fi
[ -z "$FILES" ] && { echo "ok  (nada a checar)"; exit 0; }

for f in $FILES; do
    [ -f "$f" ] || continue
    case "$f" in
        tools/lint-label-vs-fact.sh|*/lint-label-vs-fact.allow) continue ;;
    esac

    # ── R1: `[ -x ]` como prova de que um binario RODA ────────────────────
    # Incidente: build-speed/jash-release e um Mach-O de macOS com o bit +x.
    # Passou em tres gates (rc 126) e o install_verify.sh chegou a um passo de
    # copia-lo por cima de /usr/local/bin/jash — trocando o shell do sistema
    # por um que o kernel recusa. `[ -x ]` responde "tem o bit", nunca "roda".
    while IFS=: read -r n line; do
        [ -z "$n" ] && continue
        report "R1 [-x] como prova de executabilidade" "$f:$n" "$(echo "$line" | cut -c1-72)" \
            "o bit de execucao nao distingue arquitetura. Execute o candidato: \"\$BIN\" -c 'exit 0'"
    # Restrito a candidatos a BINARIO (jash, jash-release, .modjash). O
    # incidente foi um Mach-O passando por `[ -x ]`; um `[ -x algum_script.sh ]`
    # nao tem esse risco e sinaliza-lo so ensinaria a desligar o lint.
    done < <(grep -nE '\[\[? *-x +"?\$\{?[A-Za-z_]*(JASH|BIN|TREE|DEST)' "$f" 2>/dev/null \
             | grep -vE "exit 0|runs |can_run" || true)

    # ── R2: variavel de ambiente como prova de identidade de processo ─────
    # Incidente: JASH_VERSION estava no ambiente herdado de um jash ancestral
    # enquanto o interpretador corrente era bash. O teste ingenuo respondeu
    # "sim, sou jash" — errado. /proc/self/exe responde o fato.
    while IFS=: read -r n line; do
        [ -z "$n" ] && continue
        report "R2 env var como prova de identidade" "$f:$n" "$(echo "$line" | cut -c1-72)" \
            "env e HERDADA: prova que um jash existiu num ancestral, nao que ele executa isto. Use readlink -f /proc/\$\$/exe"
    done < <(grep -nE '\[ +-n +"?\$\{?JASH_(VERSION|SESSION)' "$f" 2>/dev/null || true)

    # ── R3: valor de configure-time servindo de build id ──────────────────
    # Incidente: o hash git era capturado por execute_process() no CMakeLists,
    # entao congelava no `cmake -B build`. Um binario cinco commits depois
    # reportava o commit antigo — e `jash version`/`--whoami`/o envelope JSON
    # sao a deteccao de drift do projeto (JINV-119). Errar com confianca ali e
    # pior que nao responder.
    if [ "$(basename "$f")" = "CMakeLists.txt" ]; then
        while IFS=: read -r n line; do
            [ -z "$n" ] && continue
            report "R3 build id capturado em configure-time" "$f:$n" "$(echo "$line" | cut -c1-72)" \
                "execute_process roda no configure e congela. Gere no BUILD (ver cmake/GitVersion.cmake)"
        done < <(grep -nE 'execute_process.*(rev-parse|git .*describe)' "$f" 2>/dev/null || true)
    fi

    # ── R4: `stat -f … || stat -c …` (e irmaos BSD‖GNU) ───────────────────
    # Incidente: o GNU stat le -f como "filesystem", trata o formato como nome
    # de arquivo, e imprime no STDOUT antes de sair 1 — entao o fallback roda e
    # sua saida e CONCATENADA com o lixo. Falha "com sucesso".
    while IFS=: read -r n line; do
        [ -z "$n" ] && continue
        report "R4 encadeamento BSD||GNU" "$f:$n" "$(echo "$line" | cut -c1-72)" \
            "o comando BSD nao falha limpo no GNU: imprime no stdout e SO ENTAO sai 1. Selecione o dialeto uma vez"
    done < <(grep -nE 'stat -f .*\|\| *stat -c|stat -c .*\|\| *stat -f' "$f" 2>/dev/null || true)

    # ── R5: `2>&1` alimentando comparacao por igualdade ───────────────────
    # Incidente: quatro gates capturavam com 2>&1 e comparavam por igualdade.
    # UMA linha de aviso no stderr (o bloqueio do ratchet) transformou 26
    # resultados corretos em falhas. O gate media o estado da maquina.
    # So dispara se a variavel capturada e COMPARADA por igualdade logo
    # abaixo. Capturar com 2>&1 para exibir num diagnostico e legitimo; o
    # incidente foi captura + `[ "$a" = "$b" ]`, em que uma linha de aviso no
    # stderr decidia o veredito.
    while IFS=: read -r n line; do
        [ -z "$n" ] && continue
        var=$(echo "$line" | sed -nE 's/^[[:space:]]*([a-z_]+)=.*/\1/p')
        [ -z "$var" ] && continue
        sed -n "$((n+1)),$((n+4))p" "$f" 2>/dev/null \
            | grep -qE "(\[|\[\[|if).*\\\$\{?$var\}?\"? *(=|==|!=)" || continue
        report "R5 2>&1 em comparacao por igualdade" "$f:$n" "$(echo "$line" | cut -c1-72)" \
            "qualquer aviso no stderr decide o veredito. Compare stdout, ou filtre o que e ruido"
    done < <(grep -nE '^\s*[a-z_]+=\$\(.*2>&1\)' "$f" 2>/dev/null || true)

    # ── R6: `mktemp -t NOME` sem XXXXXX ───────────────────────────────────
    # Incidente: BSD acrescenta os X; o GNU recusa ("too few X's in template")
    # e o gate inteiro abortava antes da primeira asercao.
    while IFS=: read -r n line; do
        [ -z "$n" ] && continue
        echo "$line" | grep -q 'XXX' && continue
        report "R6 mktemp -t sem XXXXXX" "$f:$n" "$(echo "$line" | cut -c1-72)" \
            "GNU coreutils recusa. Escreva o template: mktemp \"\${TMPDIR:-/tmp}/nome.XXXXXX\""
    done < <(grep -nE 'mktemp +(-d +)?-t +' "$f" 2>/dev/null || true)
done

echo
if [ "$fail" -eq 0 ]; then
    echo -e "\033[32mok\033[0m  nenhum rotulo-como-prova novo"
    exit 0
fi
echo -e "\033[31mFAIL\033[0m  $fail ocorrencia(s)."
echo "Se alguma for legitima, acrescente a linha em $ALLOW"
echo "precedida de um comentario '# porque: <razao>'."
exit 1
