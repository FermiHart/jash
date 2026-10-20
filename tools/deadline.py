#!/usr/bin/env python3
"""Cão de guarda com prazo, matando o GRUPO de processos.

Chamado por tools/deadline.sh. Veja o cabeçalho daquele arquivo para o porquê
deste mecanismo existir em vez de `timeout(1)` (que não existe neste macOS, e
cuja ausência tornou inerte o watchdog do gate JINV-057).

Convenções de saída, iguais às do timeout(1) para que gates continuem legíveis:
    124 = estourou o prazo
    127 = comando não encontrado
    outro = o rc do próprio comando
"""
import os
import signal
import subprocess
import sys

if len(sys.argv) < 3:
    sys.stderr.write("uso: deadline.py <segundos> <comando> [args...]\n")
    sys.exit(2)

try:
    secs = float(sys.argv[1])
except ValueError:
    sys.stderr.write("deadline.py: prazo invalido: %s\n" % sys.argv[1])
    sys.exit(2)

cmd = sys.argv[2:]

try:
    # start_new_session põe o filho num grupo próprio, para que o kill alcance
    # os descendentes. Um shell travado costuma ter filhos; matar só o pai
    # deixa o laço girando e o gate mentindo sobre ter contido o estouro.
    p = subprocess.Popen(cmd, start_new_session=True)
except FileNotFoundError:
    sys.exit(127)
except PermissionError:
    sys.exit(126)

try:
    sys.exit(p.wait(timeout=secs))
except subprocess.TimeoutExpired:
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        try:
            p.kill()
        except ProcessLookupError:
            pass
    try:
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass
    sys.exit(124)
