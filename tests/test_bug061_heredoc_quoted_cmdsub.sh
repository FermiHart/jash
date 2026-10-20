#!/usr/bin/env bash
# BUG-061 — heredoc com delimitador CITADO executava crases no corpo.
#
# ═══ A PROPRIEDADE ══════════════════════════════════════════════════════
#
# POSIX XCU 2.7.4: se QUALQUER caractere da palavra delimitadora estiver
# citado, o corpo do heredoc NÃO sofre expansão — nem $VAR, nem $( ), nem
# CRASE. As três formas valem:
#
#     <<'EOF'    <<"EOF"    <<\EOF
#
# Um corpo assim é DADO. Se uma crase nele executa, o shell rodou um comando
# arbitrário que o autor escreveu como texto — classe INJEÇÃO (mensagens de
# commit via `git commit -F - <<'MSG'` com trechos `assim` eram corrompidas
# e os comandos dentro das crases EXECUTAVAM).
#
# ═══ AS DUAS PONTAS ═════════════════════════════════════════════════════
#
#   ponta do bug:     <<'EOF' / <<"EOF" / <<\EOF com `cmd` no corpo
#                     → corpo LITERAL, `cmd` NUNCA executa, e a linha AFTER
#                     roda (prova de que o corpo não vazou para o parser);
#
#   ponta que segura: <<EOF sem citação com `cmd` no corpo
#                     → `cmd` EXECUTA e a saída entra no lugar (um conserto
#                     que simplesmente desligasse cmdsub em todo heredoc
#                     passaria na primeira ponta e quebraria esta).
#
# ═══ DETECTORES ═════════════════════════════════════════════════════════
#
# Dois, independentes:
#
#   1. ORÁCULO — /bin/bash roda o MESMO arquivo; as saídas têm que ser
#      idênticas (como em test_jinv092_heredoc_delim.sh);
#
#   2. VENENO — o corpo contém `` `touch <arquivo>` ``: se a crase executar,
#      o arquivo aparece. Execução arbitrária detectada sem depender de
#      texto nenhum. Na ponta que segura o arquivo TEM que aparecer.
#
# ═══ A PONTA INTERATIVA ═════════════════════════════════════════════════
#
# O REPL (c/main.c) tinha uma TERCEIRA leitura do delimitador, que copiava as
# aspas JUNTO ('EOF'): com o fd em O_NONBLOCK (o readline deixa), a captura
# via EOF imediato devolvia NULL e o corpo voltava ao readline como COMANDO
# (a mesma família do JINV-016/020/022) — e a crase nele EXECUTAVA. O
# conserto faz o REPL usar a leitura única, `jash_heredoc_delim` (JINV-092).
# A seção 4 dirige um pty com as linhas ESPAÇADAS (0,6 s) — o desenho medido
# que cai no caminho do defeito TODA vez: 5/5 "command not found" no binário
# com o bug. Linhas enviadas JUNTAS caem na continuation do readline e até o
# binário defeituoso passa — por isso o driver escreve uma linha de cada vez.
#
# O binário sob teste vem de JASH_UNDER_TEST — nunca de JASH_BIN (o jash a
# exporta no ambiente dos agentes; lê-la mediria o binário instalado).
#
# PROVA NEGATIVA (medida em 2026-08-01): contra o binário de 2026-07-28 o
# caso `<<\EOF` deste gate sai VERMELHO com a assinatura exata do BUG-061 —
# `valor = PWNED` (crase executada) e o corpo vazando como comando.

set -u
cd "$(dirname "$0")/.." || exit 2
ROOT="$PWD"
JASH="${JASH_UNDER_TEST:-$ROOT/build/jash}"
DEADLINE="$ROOT/tools/deadline.sh"
ORACLE=/bin/bash
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off JASH_SIGIL=0 JASH_MEMO=0
export JASH_ABSORB=0
[ -x "$JASH" ]     || { echo "SKIP: $JASH ausente"; exit 0; }
[ -x "$DEADLINE" ] || { echo "SKIP: $DEADLINE ausente"; exit 0; }
[ -x "$ORACLE" ]   || { echo "SKIP: $ORACLE ausente — sem oráculo não há gate"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

W=$(mktemp -d) || exit 2
trap 'rm -rf "$W"' EXIT INT TERM

export HOME=/CASA_DE_TESTE

# ── o juiz-oráculo (mesmo desenho do JINV-092) ─────────────────────────
oracle() { # rotulo  programa
    local label="$1" prog="$2" jo bo jr br
    printf '%s\n' "$prog" > "$W/p.sh"
    jo=$( (cd "$W" && LC_ALL=C LANG=C HOME=/CASA_DE_TESTE "$DEADLINE" 10 "$JASH"   ./p.sh 2>/dev/null) ); jr=$?
    bo=$( (cd "$W" && LC_ALL=C LANG=C HOME=/CASA_DE_TESTE "$DEADLINE" 10 "$ORACLE" ./p.sh 2>/dev/null) ); br=$?
    if [ "$jr" = 124 ]; then
        bad "$label" "o jash TRAVOU (10 s) prog: $(printf '%s' "$prog" | tr '\n' '|')"
    elif [ "$br" = 124 ]; then
        ok "$label (o próprio bash travou — fora de alcance)"
    elif [ "$jo" = "$bo" ] && [ "$jr" = "$br" ]; then
        ok "$label"
    else
        bad "$label" "prog: $(printf '%s' "$prog" | tr '\n' '|')
        jash=[$(printf '%s' "$jo" | tr '\n' '|')] rc=$jr  ≠  bash=[$(printf '%s' "$bo" | tr '\n' '|')] rc=$br"
    fi
}

# ── o juiz-veneno: a crase no corpo vira arquivo se executar ───────────
# rotulo  programa  espera_veneno(0=não pode existir, 1=tem que existir)
poison() {
    local label="$1" prog="$2" want="$3" out rc
    rm -f "$W/PWNED"
    printf '%s\n' "$prog" > "$W/p.sh"
    out=$( (cd "$W" && LC_ALL=C LANG=C HOME=/CASA_DE_TESTE "$DEADLINE" 10 "$JASH" ./p.sh 2>/dev/null) ); rc=$?
    if [ "$rc" = 124 ]; then
        bad "$label" "o jash TRAVOU (10 s) prog: $(printf '%s' "$prog" | tr '\n' '|')"
        return
    fi
    if [ "$want" = 0 ] && [ -e "$W/PWNED" ]; then
        bad "$label" "INJEÇÃO: a crase no corpo CITADO executou (PWNED existe). saída=[$(printf '%s' "$out" | tr '\n' '|')]"
    elif [ "$want" = 1 ] && [ ! -e "$W/PWNED" ]; then
        bad "$label" "REGRESSÃO: a crase no corpo NÃO citado deixou de executar. saída=[$(printf '%s' "$out" | tr '\n' '|')]"
    else
        ok "$label"
    fi
}

# ── 0. ANTI-INÉRCIA: o cão de guarda está vivo? ────────────────────────
"$DEADLINE" 2 sleep 30 >/dev/null 2>&1
if [ $? = 124 ]; then
    ok "cão de guarda late: travamento real devolve 124 (não 127)"
else
    bad "watchdog inerte" \
        "deadline.sh 2 sleep 30 não devolveu 124 — as asserções de prazo estão sem valor"
fi

# ── 1. A PONTA DO BUG: delimitador citado ⇒ corpo literal, crase inerte ─
oracle "<<'EOF' com crases: corpo literal, nada executa, AFTER roda" \
'cat <<'"'"'EOF'"'"'
valor = `echo PWNED`
EOF
echo AFTER'
oracle '<<"EOF" com crases: corpo literal, nada executa, AFTER roda' \
'cat <<"EOF"
valor = `echo PWNED`
EOF
echo AFTER'
oracle "<<\EOF com crases: corpo literal, nada executa, AFTER roda" \
'cat <<\EOF
valor = `echo PWNED`
EOF
echo AFTER'
oracle "<<'EOF' crase + \$HOME mistos: os dois literais" \
'cat <<'"'"'EOF'"'"'
diag (`--dump-succ <path>`, engine `dump_successor_histogram`) em $HOME
EOF
echo AFTER'

poison "<<'EOF' com \`touch\` no corpo NÃO cria arquivo" 0 \
'cat <<'"'"'EOF'"'"'
valor = `touch PWNED`
EOF
echo AFTER'
poison '<<"EOF" com `touch` no corpo NÃO cria arquivo' 0 \
'cat <<"EOF"
valor = `touch PWNED`
EOF
echo AFTER'
poison "<<\EOF com \`touch\` no corpo NÃO cria arquivo" 0 \
'cat <<\EOF
valor = `touch PWNED`
EOF
echo AFTER'

# ── 2. A PONTA QUE SEGURA: sem citação, a crase EXECUTA ────────────────
oracle "<<EOF sem citação EXPANDE a crase do corpo" \
'cat <<EOF
valor = `echo EXPANDIU`
EOF
echo AFTER'
poison "<<EOF sem citação com \`touch\` no corpo CRIA arquivo" 1 \
'cat <<EOF
valor = `touch PWNED`
EOF
echo AFTER'

# ── 3. O CENÁRIO DO RELATO: git commit -F - <<'MSG' ─────────────────────
# A mensagem tem que sair INTEIRA e as crases não podem ter executado. O
# `git` aqui é um impostor no PATH que despeja o stdin — o que está sob
# teste é o shell, não o git.
mkdir -p "$W/bin"
cat > "$W/bin/git" <<'IMPOSTOR'
#!/bin/sh
if [ "$1" = commit ]; then cat; fi
IMPOSTOR
chmod +x "$W/bin/git"
printf '%s\n' 'git commit -F - <<'"'"'MSG'"'"'
diag (`--dump-succ <path>`, engine `dump_successor_histogram`): texto
MSG' > "$W/p2.sh"
jo=$( (cd "$W" && PATH="$W/bin:$PATH" LC_ALL=C LANG=C HOME=/CASA_DE_TESTE "$DEADLINE" 10 "$JASH" ./p2.sh 2>/dev/null) ); jr=$?
if [ "$jr" = 124 ]; then
    bad "git commit -F - <<'MSG'" "o jash TRAVOU (10 s)"
elif [ "$jo" = 'diag (`--dump-succ <path>`, engine `dump_successor_histogram`): texto' ]; then
    ok "git commit -F - <<'MSG' grava a mensagem com as crases INTEIRAS"
else
    bad "git commit -F - <<'MSG'" "mensagem=[$(printf '%s' "$jo" | tr '\n' '|')] rc=$jr — esperado: crases literais"
fi

# ── 4. A PONTA INTERATIVA: o REPL fecha o citado com delimitador NU ────
# O bug MORAVA aqui: o REPL (c/main.c) tinha uma terceira leitura do
# delimitador, que copiava as aspas JUNTO ('Q'). Com stdin em O_NONBLOCK (o
# readline deixa), o laço de captura via EOF imediato devolvia NULL e a linha
# `cat <<'Q'` ia ao avaliador SEM corpo — as linhas seguintes, que o autor
# escreveu como DADO, voltavam ao readline como COMANDO, e a crase nelas
# EXECUTAVA.
#
# A disputa readline × captura-raw no mesmo fd é sensível a temporização
# (medido): linhas chegando JUNTAS caem na continuation do readline e até o
# binário com o bug sai certinho; linhas chegando ESPAÇADAS (1 s) caem no
# caminho do defeito. E o defeito tem DOIS disfarces (medidos):
#
#   VAZA   — a captura via EAGAIN no fd O_NONBLOCK do readline, devolvia
#            NULL, e o corpo voltava ao readline como COMANDO: "command not
#            found" na saída e a crase EXECUTADA (PWNEDREPL criado);
#
#   ENGOLiu — a captura lia o corpo cru e nunca fechava (delimitador
#            guardado COM aspas): o texto aparece só no ECO do tty e o
#            shell morre esperando. Sem "command not found", sem PWNEDREPL
#            — só se delata porque NADA roda depois do delimitador.
#
# Os detectores, na ordem, cobrem os dois disfarces e o shell são:
#   (a) PWNEDREPL existe          → VAZA com injeção;
#   (b) "command not found"       → VAZA;
#   (c) REPL_AFTER_<dígito> some  → ENGOLIU (o `echo REPL_AFTER_$?` é mudo
#       no eco — `$?` só vira dígito se o echo EXECUTAR);
#   (d) o corpo LITERAL tem que sair (foi entregue ao cat, não perdido).
if command -v python3 >/dev/null 2>&1; then
    cat > "$W/pty_drive.py" <<'PYEOF'
import os, pty, select, sys, time
jash, marker = sys.argv[1], sys.argv[2]
lines = [
    "cat <<'Q'\n",
    "antes `touch %s` depois\n" % marker,
    "Q\n",
    "echo REPL_AFTER_$?\n",
]
pid, fd = pty.fork()
if pid == 0:
    os.environ["JASH_QUIET"] = "1"
    os.environ["JASH_SIGIL"] = "0"
    os.execv(jash, ["jash"])
def drain(t):
    out = b""; end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try: out += os.read(fd, 65536)
            except OSError: break
    return out
drain(1.0)
out = b""
for line in lines:
    os.write(fd, line.encode())
    out += drain(1.0)
# Espera ADAPTATIVA pelo REPL_AFTER_<dígito>: o reprint do readline é lento
# sob carga, e um prazo fixo curto acusa "engoliu" num shell são (medido:
# falso-vermelho com build concorrente na máquina).
import re
end = time.time() + 10
while time.time() < end and not re.search(rb"REPL_AFTER_[0-9]", out):
    out += drain(0.5)
try: os.kill(pid, 9); os.waitpid(pid, 0)
except OSError: pass
sys.stdout.buffer.write(out)
PYEOF
    rm -f "$W/PWNEDREPL"
    iout=$( "$DEADLINE" 20 python3 "$W/pty_drive.py" "$JASH" "$W/PWNEDREPL" 2>/dev/null ); irc=$?
    if [ "$irc" = 124 ]; then
        bad "REPL: <<'Q' fecha no delimitador nu" "a sessão interativa TRAVOU (20 s) — delimitador guardado com aspas?"
    elif [ -e "$W/PWNEDREPL" ]; then
        bad "REPL: <<'Q' fecha no delimitador nu" "INJEÇÃO: a crase no corpo CITADO executou (PWNEDREPL existe). saída=[$(printf '%s' "$iout" | tr '\n' '|')]"
    elif printf '%s' "$iout" | grep -q 'command not found'; then
        bad "REPL: <<'Q' fecha no delimitador nu" "DADO VIROU COMANDO: o corpo vazou para o parser. saída=[$(printf '%s' "$iout" | tr '\n' '|')]"
    elif ! printf '%s' "$iout" | grep -qE 'REPL_AFTER_[0-9]'; then
        bad "REPL: <<'Q' fecha no delimitador nu" "ENGOLIU: nada rodou depois do delimitador (REPL_AFTER_<n> ausente). saída=[$(printf '%s' "$iout" | tr '\n' '|')]"
    elif printf '%s' "$iout" | grep -qF 'antes `touch '"$W/PWNEDREPL"'` depois'; then
        ok "REPL: <<'Q' fecha no delimitador nu, corpo literal, crase inerte"
    else
        bad "REPL: <<'Q' fecha no delimitador nu" "saída=[$(printf '%s' "$iout" | tr '\n' '|')] — corpo não saiu literal"
    fi
else
    echo "skip  REPL interativo (python3 ausente)"
fi

echo ""
echo "=== BUG-061: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
