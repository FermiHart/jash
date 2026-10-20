#!/usr/bin/env bash
# tools/deadline.sh — o cão de guarda dos gates. Use SEMPRE este, nunca `timeout`.
#
# ── Por que este arquivo existe ────────────────────────────────────────────
#
# `timeout(1)` NÃO EXISTE neste macOS. Um gate que escreve
#
#     timeout 10 "$JASH" -c '...'
#
# não protege nada: o bash devolve 127 ("command not found"), o teste vê um rc
# diferente de 124 e conclui "não travou". O cão de guarda late para sempre
# porque nunca acordou.
#
# Isto não é hipótese. O gate JINV-057 nasceu hoje com a asserção
# "parênteses e pontuação não travam a extração" — escrita EXATAMENTE para
# pegar um laço infinito que tinha acabado de travar o shell instalado — e
# ela era inerte desde a primeira execução. Passou verde por 127.
#
# A confusão tem um agravante cruel: `command -v timeout` responde "timeout"
# quando quem pergunta é o jash, porque ali `timeout` É um builtin. Só que os
# gates rodam sob `bash`, onde não é. A verificação mais óbvia mente.
#
# ── Contrato ───────────────────────────────────────────────────────────────
#
#     deadline.sh <segundos> <comando> [args...]
#
# rc 124 = estourou o prazo (mesma convenção do timeout(1), para que gates
#          escritos contra ela continuem legíveis)
# rc 127 = o comando não existe
# senão  = o rc do próprio comando
#
# Mata o GRUPO de processos, não só o filho direto — um shell que trava
# costuma ter descendentes, e matar só o pai deixa o laço girando.
#
# F E R M I ∞ H A R T

set -u

if [ "$#" -lt 2 ]; then
    printf 'uso: deadline.sh <segundos> <comando> [args...]\n' >&2
    exit 2
fi

secs="$1"; shift

# Preferimos python3 (presente em toda máquina onde este projeto compila):
# ele mata o grupo inteiro e distingue "não existe" de "estourou".
if command -v python3 >/dev/null 2>&1; then
    exec python3 "$(dirname "$0")/deadline.py" "$secs" "$@"
fi

# Sem python3: perl com alarm. Não mata o grupo, mas é melhor que nada —
# e o gate que usar isto ainda detecta o estouro.
if command -v perl >/dev/null 2>&1; then
    perl -e '
        my $s = shift;
        my $pid = fork();
        die "fork: $!" unless defined $pid;
        if ($pid == 0) { setpgrp(0,0); exec @ARGV or exit 127; }
        my $rc = 0;
        eval {
            local $SIG{ALRM} = sub { die "timeout\n" };
            alarm $s; waitpid($pid, 0); alarm 0;
            $rc = $? >> 8;
        };
        if ($@) { kill(-9, $pid); waitpid($pid, 0); exit 124; }
        exit $rc;
    ' "$secs" "$@"
    exit $?
fi

# Último recurso: se existir um timeout(1) DE VERDADE, use-o.
if command -v /usr/bin/timeout >/dev/null 2>&1; then
    /usr/bin/timeout "$secs" "$@"
    exit $?
fi

# Nada disponível: FALHAR ALTO. Rodar sem cão de guarda e fingir que há um é
# a falha que este arquivo existe para matar.
printf 'deadline.sh: nenhum mecanismo de prazo disponivel (python3, perl, /usr/bin/timeout)\n' >&2
printf 'deadline.sh: RECUSANDO rodar sem protecao — um gate sem prazo pendura a suite\n' >&2
exit 2
