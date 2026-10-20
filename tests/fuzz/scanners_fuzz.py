#!/usr/bin/env python3
# tests/fuzz/scanners_fuzz.py — motor determinístico do gate JINV-060.
#
# Gera entradas com semente FIXA (nada de aleatoriedade real: o gate tem que
# dar o mesmo veredito hoje e daqui a um ano) e as joga contra as funções de
# varredura de texto do jash, sempre através do binário real.
#
# O que ele procura NÃO é saída errada — é ausência de resposta:
#   HANG    o processo não terminou dentro do prazo (laço infinito ou custo
#           exponencial: para quem espera, é a mesma coisa)
#   CRASH   o processo morreu de sinal (SIGSEGV/SIGABRT/SIGBUS/...)
#   SHARED  travou, mas o /bin/bash trava na MESMA linha — é propriedade da
#           classe de problema, não defeito do jash. Informativo, não falha.
#
# O oráculo é o que separa acusação de fofoca: todo achado é reconferido
# contra o bash antes de virar veredito. Um builtin que só o jash tem faz o
# bash sair na hora com "command not found" — o que atribui o travamento ao
# jash, corretamente.
#
# Por que o watchdog é do python e não `timeout(1)`: este mac não tem
# `timeout` no PATH (só o builtin do próprio jash — o suspeito vigiando a si
# mesmo). `subprocess(timeout=)` + `killpg` é o equivalente portátil, e mata o
# GRUPO, não só o filho direto.
#
# Protocolo de saída (consumido por tests/test_jinv060_fuzz_scanners.sh):
#   CASES n / INVOCATIONS n / ELAPSED s / SELFTEST hang_detected=yes|no
#   FAMILY <nome> cases=<n> hangs=<n> crashes=<n> shared=<n> skipped=<n>
#   HANG   <familia> <hex> <repr-entrada> :: <repr-linha>
#   CRASH  <familia> sig=<n> <hex> <repr-entrada> :: <repr-linha>
#   SHARED <familia> <hex> <repr-entrada> :: <repr-linha>
#   TOTAL hangs=<n> crashes=<n> shared=<n>
#
# F E R M I ∞ H A R T

import os
import random
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SEED = 20260727          # semente fixa: reprodutibilidade > cobertura
TIMEOUT = 5.0            # prazo por invocação
MAX_PER_FAMILY = 1       # disjuntor: provada a classe, para de pagar 5s por caso
MAX_FINDINGS = 5         # teto global de achados — o gate não pode virar o lento

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
JASH = os.environ.get("JASH", os.path.join(REPO, "build", "jash"))
BASH = os.environ.get("ORACLE_SHELL", "/bin/bash")
N_CASES = int(os.environ.get("JASH_FUZZ_CASES", "480"))

# ── alfabeto ────────────────────────────────────────────────────────────────
ALNUM = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
# a pontuação pesa mais que letra na mistura: é onde a classe de bug mora
# (separador assimétrico, ponteiro que não avança)
PUNCT = "/.()[]{}'\"`,;: \t\\$|&<>*?~#!-=+"
UTF8 = ["á", "ç", "ã", "É", "ü", "∞", "→", "日", "本", "💥", "«", "»"]

# formas que imitam texto de bilhete real — onde o defeito original nasceu
SHAPES = [
    "{w}({w}) {w}",
    "{w}) {w}",
    "({w}",
    "{w}/{w}.c ({w})",
    "{p}{p}{p}{p}",
    "{w}: [{w}] {{{w}}}",
    "{w} {p}{p} {w}",
    "'{w}\" {w}",
    "{w}/{w}/{w}.{w}",
    "{n}/{n} {w} {n}/{n}/{n}",
    "{p}",
    "{w}{p}{w}{p}",
]

# Corpus de regressão: entradas que JÁ quebraram algo, ou que exercem a forma
# exata do defeito conhecido. Vêm primeiro para que uma regressão clássica
# falhe cedo e barato.
SEEDS = [
    ")", "(", ")))(((",
    "ver eval.c (info perdida) aqui",
    "a) primeiro b) segundo",
    "f(x) e g(y) em util.c",
    "lista: (um, dois) [tres] {quatro}",
    "83/83 verde em 2026/07/27",
    "c/parser/eval.c (o ponto certo)",
    "`", "'", '"', "\\", "\t", " ", "", "a\\",
    "$(", "${", "((", "|", "&", "<", ">", "*", "~", "#", "!",
    "a" * 200, "/" * 64, "(" * 32 + ")" * 32,
    "∞", "日本語 c/parser/eval.c", "💥)(💥",
]


def gen_inputs(rng, n):
    out = list(SEEDS)
    while len(out) < n:
        kind = rng.random()
        if kind < 0.35:
            out.append(_shape(rng))
        elif kind < 0.55:
            out.append(_run(rng, PUNCT, rng.randint(1, 24)))
        else:
            out.append(_mixed(rng, rng.randint(1, 28)))
    return out[:n]


def _word(rng):
    return "".join(rng.choice(ALNUM) for _ in range(rng.randint(1, 6)))


def _shape(rng):
    s = rng.choice(SHAPES)
    while "{w}" in s or "{p}" in s or "{n}" in s:
        s = (s.replace("{w}", _word(rng), 1)
              .replace("{p}", rng.choice(PUNCT), 1)
              .replace("{n}", str(rng.randint(0, 999)), 1))
    return s


def _run(rng, pool, n):
    return "".join(rng.choice(pool) for _ in range(n))


def _mixed(rng, n):
    out = []
    for _ in range(n):
        r = rng.random()
        if r < 0.45:
            out.append(rng.choice(ALNUM))
        elif r < 0.90:
            out.append(rng.choice(PUNCT))
        else:
            out.append(rng.choice(UTF8))
    return "".join(out)


# ── alvos ───────────────────────────────────────────────────────────────────
# Cada família devolve a LISTA de linhas `-c` a rodar, na ordem. Famílias com
# duas linhas são as que gravam e releem: o defeito pode estar na escrita, na
# leitura, ou só na combinação — foi exatamente o caso do `resume`.
def fam_resume(t):   return ['resume ' + t, 'resume']
def fam_assert(t):   return ['assert ' + t + " --check 'true'", 'assert']
def fam_echo(t):     return ['echo ' + t]
def fam_why(t):      return ['cd ' + t, 'why']
def fam_intent(t):   return ['intent ' + t]
def fam_explain(t):  return ['explain ' + t]
def fam_lint(t):     return ['lint -c ' + t]
def fam_token(t):    return ['token count ' + t]
def fam_prompt(t):   return ['prompt analyze ' + t]
def fam_mistake(t):  return ['mistake log ' + t]
def fam_parse(t):    return ['@RAW@']   # a linha crua, sem embrulho


# ── família glob ────────────────────────────────────────────────────────────
# Casamento de padrão é varredura de texto com backtracking, e é a forma da
# varredura onde "não travar" custa uma linha (colapsar a corrida de `*`) que é
# fácil de esquecer numa das cópias. Aqui SÓ se geram corridas de `*` seguidas
# de literal — a classe em que um matcher correto é LINEAR, e portanto em que
# um travamento é defeito e não propriedade do problema. Padrões alternados
# (`*a*a*a...`) fazem o bash sofrer igual e por isso ficam de fora; se algum
# escapar, o oráculo os classifica como SHARED em vez de acusar.
GLOB_FORMS = [
    lambda s, p: "case '%s' in %s) :;; esac" % (s, p),
    lambda s, p: "x='%s'; echo ${x##%s}" % (s, p),
    lambda s, p: "x='%s'; echo ${x%%%%%s}" % (s, p),
    lambda s, p: "[[ '%s' == %s ]]" % (s, p),
    lambda s, p: "x='%s'; echo ${x#%s}" % (s, p),
    lambda s, p: "x='%s'; echo ${x%%%s}" % (s, p),
]


def glob_case(rng):
    body = rng.choice("ab01x/")
    n = rng.randint(16, 48)
    subject = body * n
    stars = "*" * rng.randint(2, 14)
    tail = rng.choice(["z", "zz", "Q9", "..", "%%"])   # literal que nunca casa
    pat = stars + tail
    return GLOB_FORMS[rng.randrange(len(GLOB_FORMS))](subject, pat), pat + " vs " + subject[:12] + "…"


FAMILIES = [
    ("resume", fam_resume),
    ("assert", fam_assert),
    ("echo", fam_echo),
    ("why", fam_why),
    ("intent", fam_intent),
    ("explain", fam_explain),
    ("lint", fam_lint),
    ("token", fam_token),
    ("prompt", fam_prompt),
    ("mistake", fam_mistake),
    ("parse", fam_parse),
    ("glob", None),          # gera a própria linha, não recebe texto livre
]


def wrap(mode, s):
    """Dois modos, porque atacam camadas diferentes:
       exato — aspas simples POSIX: o builtin recebe os BYTES exatos, e quem é
               fuzzado é a varredura dele (foi assim que o `)` travou).
       cru   — o texto entra sem escape dentro de aspas duplas: quem é fuzzado
               é o PARSER, que pode nem chegar ao builtin."""
    return shlex.quote(s) if mode == 0 else '"' + s + '"'


def run_one(binpath, cmd, workdir, timeout=None):
    """Uma invocação sob prazo. Devolve (status, rc) com status em
    {'ok','hang','signal'}."""
    env = dict(os.environ)
    env.update(HOME=workdir, JASH_NUDGE="0", JASH_BRIEF="0",
               JASH_LOOPGUARD="off", JASH_SIGIL="0")
    try:
        p = subprocess.Popen([binpath, "-c", cmd], cwd=workdir, env=env,
                             stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL,
                             start_new_session=True)
    except OSError as e:
        print("ERROR spawn %s" % e, file=sys.stderr)
        return ("ok", 0)
    try:
        rc = p.wait(timeout=timeout if timeout is not None else TIMEOUT)
    except subprocess.TimeoutExpired:
        _killgroup(p)
        return ("hang", 124)
    if rc < 0:
        return ("signal", -rc)
    return ("ok", rc)


def _killgroup(p):
    """Mata o GRUPO: um shell travado costuma ter filhos, e deixar o neto vivo
    contamina as medições seguintes."""
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


def oracle_also_hangs(cmd):
    """O bash trava na mesma linha? Então não é defeito do jash."""
    if not os.access(BASH, os.X_OK):
        return False
    w = tempfile.mkdtemp(prefix="jash-oracle-")
    try:
        st, _ = run_one(BASH, cmd, w)
        return st == "hang"
    finally:
        shutil.rmtree(w, ignore_errors=True)


def selftest():
    """Um fuzzer que não sabe detectar é pior que nenhum: dá luz verde.
    Aqui um laço infinito de verdade tem que ser reportado como hang."""
    w = tempfile.mkdtemp(prefix="jash-canary-")
    try:
        st, _ = run_one(JASH, "while :; do :; done", w, timeout=2.0)
        return st == "hang"
    finally:
        shutil.rmtree(w, ignore_errors=True)


def main():
    if not os.access(JASH, os.X_OK):
        print("SKIP %s ausente" % JASH)
        return 0

    rng = random.Random(SEED)
    inputs = gen_inputs(rng, N_CASES)
    grng = random.Random(SEED ^ 0x9E3779B9)   # fluxo próprio: a família glob não
                                              # desloca o corpus das outras

    stats = {name: {"cases": 0, "hangs": 0, "crashes": 0, "shared": 0, "skipped": 0}
             for name, _ in FAMILIES}
    findings = []
    invocations = 0
    t0 = time.time()

    st_ok = selftest()

    parent = tempfile.mkdtemp(prefix="jash-fuzz-")
    try:
        for i, s in enumerate(inputs):
            name, fam = FAMILIES[i % len(FAMILIES)]
            st = stats[name]

            if name == "glob":
                lines, shown = [glob_case(grng)[0]], None
                shown = lines[0]
            else:
                mode = (i // len(FAMILIES)) % 2
                quoted = wrap(mode, s)
                lines = [s if ln == '@RAW@' else ln for ln in fam(quoted)]
                shown = s

            # Disjuntor: provada a classe numa família, os casos seguintes dela
            # só pagariam 5s de watchdog para repetir o mesmo veredito.
            if st["hangs"] + st["crashes"] >= MAX_PER_FAMILY or len(findings) >= MAX_FINDINGS:
                st["skipped"] += 1
                continue

            # HOME + cwd por caso: estado nunca acumula, então o caso N não
            # depende do caso N-1 e o repro impresso vale sozinho.
            work = tempfile.mkdtemp(dir=parent)
            try:
                for ln in lines:
                    invocations += 1
                    status, rc = run_one(JASH, ln, work)
                    if status == "hang":
                        if oracle_also_hangs(ln):
                            st["shared"] += 1
                            findings.append(("SHARED", name, shown, ln, 0))
                        else:
                            st["hangs"] += 1
                            findings.append(("HANG", name, shown, ln, 0))
                        break
                    if status == "signal":
                        st["crashes"] += 1
                        findings.append(("CRASH", name, shown, ln, rc))
                        break
            finally:
                shutil.rmtree(work, ignore_errors=True)
            st["cases"] += 1
    finally:
        shutil.rmtree(parent, ignore_errors=True)

    elapsed = time.time() - t0
    print("CASES %d" % len(inputs))
    print("INVOCATIONS %d" % invocations)
    print("ELAPSED %.1f" % elapsed)
    print("SELFTEST hang_detected=%s" % ("yes" if st_ok else "no"))
    th = tc = ts = 0
    for name, _ in FAMILIES:
        st = stats[name]
        th += st["hangs"]; tc += st["crashes"]; ts += st["shared"]
        print("FAMILY %s cases=%d hangs=%d crashes=%d shared=%d skipped=%d"
              % (name, st["cases"], st["hangs"], st["crashes"],
                 st["shared"], st["skipped"]))
    for kind, name, shown, ln, sig in findings:
        h = shown.encode("utf-8", "surrogatepass").hex()
        if kind == "CRASH":
            print("CRASH %s sig=%d %s %s :: %s" % (name, sig, h, repr(shown), repr(ln)))
        else:
            print("%s %s %s %s :: %s" % (kind, name, h, repr(shown), repr(ln)))
    print("TOTAL hangs=%d crashes=%d shared=%d" % (th, tc, ts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
