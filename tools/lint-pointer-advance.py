#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Analisador do JINV-062 — laco de varredura que pode nao avancar o ponteiro.

Este arquivo e o MOTOR. A porta de entrada e `tools/lint-pointer-advance.sh`,
que faz a catraca (.allow, teto, `# porque:`) no mesmo molde do
`lint-no-silent-truncation.sh`. A analise ficou em Python porque a heuristica
central — comparar os CONJUNTOS DE CARACTERES de dois lacos vizinhos, com
expansao de macro — precisa casar parenteses, decodificar escapes (`'\\t'`,
`'\\''`) e expandir `#define`; grep nao faz nenhuma dessas tres coisas, e um
lint que erra o conjunto e pior que lint nenhum.

═══ A propriedade ═══

    Um `while` que varre caracteres por ponteiro TEM que avancar o ponteiro
    em todo caminho possivel de uma iteracao.

Quando nao avanca, a iteracao seguinte comeca no estado exato da anterior: o
shell trava, sem mensagem, sem log, sem core. Dois bugs desta classe em um
unico dia, escritos pela mesma cabeca com oito horas de intervalo:

  CASO 1 · c/parser/if_block.c, tokenize_if_block — um laco externo cujo corpo
  podia terminar onde comecou. Conserto: `const char* iter_start = p; ... if
  (p == iter_start && *p) p++;`  (JINV-043)

  CASO 2 · c/builtins/builtin_resume.c, rs_collect_paths — dois lacos internos
  com conjuntos de separadores ASSIMETRICOS. `(` estava no conjunto que PULA e
  nao no que PARA; `)` no que PARA e nao no que PULA. Num `)`: o laco de pular
  nao consome, o de varrer para imediatamente, comprimento zero, ponteiro
  imovel. Travou o shell instalado na maquina do dono.  (JINV-057)

═══ Por que a simetria e uma PROVA, e nao um estilo ═══

Considere o par canonico, com o mesmo ponteiro `p`:

    while (*p &&  A(*p)) p++;     /* pular  */
    st = p;
    while (*p && !B(*p)) p++;     /* varrer */

Se A == B, entao para todo caractere c != '\\0': ou A(c) e verdade e o primeiro
laco consome c, ou A(c) e falso — logo B(c) e falso — e o segundo consome c.
Toda iteracao avanca pelo menos um byte: o travamento e IMPOSSIVEL por
construcao. Se A != B, existe um caractere na diferenca simetrica que nenhum
dos dois consome, e ele congela o ponteiro. A diferenca dos conjuntos nao e um
indicio do bug: e o bug, escrito por extenso. Por isso o relatorio imprime
QUAIS caracteres divergem — e essa a informacao que conserta.

Uma guarda de progresso explicita (`if (p == iter_start) ... p++`) e a segunda
condicao suficiente, independente da primeira: mata a classe inteira mesmo com
conjuntos assimetricos. Qualquer uma das duas basta para SEGURO.

═══ Precisao antes de cobertura ═══

Ruido mata a confianca num lint, e um lint em que ninguem confia e pior que
nenhum — vira `|| true` no CI. Um par so vira ACUSACAO depois de sobreviver a
seis absolvicoes, cada uma um jeito legitimo de a volta avancar que este
arquivo aprendeu a enxergar:

  1. simetria — o conjunto que PARA esta contido no que PULA (contando TODOS
     os lacos de pular da volta, e tambem `if (*p == ',') p++;`);
  2. o par nao esta dentro de laco nenhum, logo nao ha volta para repetir;
  3. o ponteiro e redeclarado a cada volta (`const char* fp = linha;`);
  4. guarda de progresso explicita no laco externo;
  5. o laco externo avanca o ponteiro em todo caminho, ou ja avancou ANTES do
     par em todo caminho que chega ate ele;
  6. o laco externo PARA nos mesmos caracteres que travariam o par.

A primeira versao deste lint, sem 1..6, acusava ~55 lacos deste repositorio,
quase todos corretos. Com elas, sobram 4 — e o primeiro deles e um travamento
real, reproduzido. Nao pretendo pegar todo laco que pode travar; pretendo
nunca gritar a toa.

Uso (o .sh chama; direto e para depurar):
    python3 tools/lint-pointer-advance.py --records c/parser c/core
    python3 tools/lint-pointer-advance.py --all arquivo.c      # veredito de cada laco
    python3 tools/lint-pointer-advance.py --counts c
"""

import os
import re
import sys

# Distancia maxima, em linhas, entre o laco que PULA e o laco que VARRE para
# serem considerados um par. O que de fato delimita o par sao as condicoes
# duras (mesmo ponteiro, mesmo laco externo, nenhum avanco entre os dois); este
# numero so evita casar trechos de codigo sem relacao nenhuma.
PAIR_GAP_LINES = 25

# Corpo minimo, em linhas, para um `while (*p)` ser tratado como laco externo
# de tokenizador na heuristica (b). Abaixo disso e um laco de uma tacada so,
# onde o avanco e visivel a olho nu.
OUTER_MIN_LINES = 4


# ───────────────────────────── mascaramento ──────────────────────────────
# Devolve uma copia do fonte com o MESMO comprimento onde comentarios viram
# espaco e o conteudo de literais vira 'X'. Preservar os offsets permite fazer
# a analise ESTRUTURAL na copia (chaves e parenteses dentro de string nao
# atrapalham) e a extracao de CARACTERES no texto original, nos mesmos
# offsets. Sem isso, `case '{':` desalinha todo o casamento de chaves.
MASK_TOKEN = re.compile(r'//[^\n]*'
                        r'|/\*.*?(?:\*/|$)'
                        r'|"(?:\\.|[^"\\\n])*"?'
                        r"|'(?:\\.|[^'\\\n])*'?", re.S)
_MASK_CACHE = {}


def mask(src):
    # As mesmas funcoes reveem os mesmos corpos varias vezes (all_paths_advance
    # chama shallow, que chama mask...). Sem o cache, mascarar sozinho comia
    # dois tercos do tempo do lint — e um lint lento e um lint que alguem tira
    # do caminho.
    hit = _MASK_CACHE.get(src)
    if hit is not None:
        return hit
    out, last = [], 0
    for m in MASK_TOKEN.finditer(src):
        out.append(src[last:m.start()])
        t = m.group(0)
        if t[0] == '/':
            out.append(''.join(c if c == '\n' else ' ' for c in t))
        else:
            q = t[0]
            if len(t) > 1 and t[-1] == q:
                out.append(q + 'X' * (len(t) - 2) + q)
            else:
                out.append(q + 'X' * (len(t) - 1))
        last = m.end()
    out.append(src[last:])
    res = ''.join(out)
    assert len(res) == len(src), (len(res), len(src))
    if len(_MASK_CACHE) > 4000:
        _MASK_CACHE.clear()
    _MASK_CACHE[src] = res
    return res


def match_paren(masked, i):
    """i aponta para '('; devolve o indice do ')' correspondente, ou -1."""
    depth = 0
    for j in range(i, len(masked)):
        if masked[j] == '(':
            depth += 1
        elif masked[j] == ')':
            depth -= 1
            if depth == 0:
                return j
    return -1


def match_brace(masked, i):
    depth = 0
    for j in range(i, len(masked)):
        if masked[j] == '{':
            depth += 1
        elif masked[j] == '}':
            depth -= 1
            if depth == 0:
                return j
    return -1


# ───────────────────────────── conjuntos ─────────────────────────────────
ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', 'a': '\a', 'b': '\b',
           'f': '\f', 'v': '\v', '\\': '\\', "'": "'", '"': '"', '?': '?'}

CHAR_LIT = re.compile(r"'(\\.|[^'\\])'")
STR_LIT = re.compile(r'"((?:\\.|[^"\\])*)"')
SETFN = re.compile(r'\b(strchr|memchr|strpbrk|strcspn|strspn)\s*\(')


def unescape(tok):
    if tok.startswith('\\'):
        return ESCAPES.get(tok[1], tok[1])
    return tok


def decode_str(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            out.append(ESCAPES.get(s[i + 1], s[i + 1]))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return out


def collect_defines(src):
    """#define de uma linha (com continuacao) -> corpo. Objeto e funcao."""
    defs = {}
    for m in re.finditer(r'^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)', src, re.M):
        name = m.group(1)
        j = m.end()
        # o corpo vai ate a primeira quebra de linha nao-continuada
        body = []
        while j < len(src):
            k = src.find('\n', j)
            k = len(src) if k < 0 else k
            line = src[j:k]
            body.append(line)
            if not line.rstrip().endswith('\\'):
                break
            j = k + 1
        defs[name] = ' '.join(body).replace('\\\n', ' ')
    return defs


def expand(text, defs, depth=0):
    """Cola o corpo dos #define citados, para que os caracteres deles entrem
    no conjunto. Sem substituicao de argumentos: so precisamos dos literais."""
    if depth > 3:
        return text
    extra = []
    for name in set(re.findall(r'\b([A-Za-z_]\w*)\b', text)):
        if name in defs:
            extra.append(expand(defs[name], defs, depth + 1))
    return text + ' ' + ' '.join(extra)


def charset(cond, defs):
    """Conjunto de caracteres literais testados na condicao.

    Devolve (conjunto, opaco) — `opaco` lista os predicados cujo conjunto nao
    da para determinar daqui (ex.: `isspace`, funcao de outro arquivo). Um
    predicado opaco NAO e ignorado: dois lacos que usam predicados opacos
    DIFERENTES sao tao indeterminados quanto dois conjuntos diferentes.
    """
    full = expand(cond, defs)
    chars = set(unescape(m.group(1)) for m in CHAR_LIT.finditer(full))
    for m in SETFN.finditer(full):
        tail = full[m.end():m.end() + 200]
        sm = STR_LIT.search(tail)
        if sm:
            chars.update(decode_str(sm.group(1)))
    opaque = set()
    for m in re.finditer(r'\b([A-Za-z_]\w{1,})\s*\(', full):
        name = m.group(1)
        if name in ('if', 'while', 'for', 'switch', 'return', 'sizeof'):
            continue
        if name in defs:
            continue
        if name in ('strchr', 'memchr', 'strpbrk', 'strcspn', 'strspn'):
            continue
        opaque.add(name)
    return chars, opaque


def show(ch):
    named = {' ': "' '", '\t': r"'\t'", '\n': r"'\n'", '\r': r"'\r'",
             '\0': r"'\0'", '\\': r"'\\'"}
    return named.get(ch, "'%s'" % ch)


def showset(s):
    return ' '.join(show(c) for c in sorted(s)) if s else '(vazio)'


# ────────────────────────── extracao de lacos ────────────────────────────
class Loop(object):
    __slots__ = ('kw', 'cond', 'body', 'line', 'start', 'end', 'ptr', 'polarity')


def find_loops(src, masked):
    loops = []
    for m in re.finditer(r'\b(while|for)\s*\(', masked):
        op = m.end() - 1
        cp = match_paren(masked, op)
        if cp < 0:
            continue
        # `} while (...)` de do/while: o corpo vem antes, ignore.
        pre = masked[:m.start()].rstrip()
        if pre.endswith('}') or pre.endswith(';'):
            if re.search(r'\bdo\b[^;]*$', masked[max(0, m.start() - 4000):m.start()], re.S):
                # heuristica barata: um `do` sem `;` no meio, logo atras
                seg = masked[max(0, m.start() - 4000):m.start()]
                if re.search(r'\bdo\s*\{[^{}]*$', seg, re.S) or seg.rstrip().endswith('}'):
                    pass  # nao da para decidir barato; segue e o corpo sai vazio
        k = cp + 1
        while k < len(masked) and masked[k] in ' \t\r\n':
            k += 1
        if k >= len(masked):
            continue
        if masked[k] == '{':
            be = match_brace(masked, k)
            if be < 0:
                continue
            bs, be = k, be + 1
        elif masked[k] == ';':
            bs, be = k, k + 1
        else:
            depth = 0
            j = k
            while j < len(masked):
                if masked[j] in '([':
                    depth += 1
                elif masked[j] in ')]':
                    depth -= 1
                elif masked[j] == ';' and depth == 0:
                    break
                j += 1
            bs, be = k, min(j + 1, len(masked))
        lp = Loop()
        lp.kw = m.group(1)
        lp.cond = src[op + 1:cp]
        lp.body = src[bs:be]
        lp.start, lp.end = m.start(), be
        lp.line = src.count('\n', 0, m.start()) + 1
        lp.ptr = None
        lp.polarity = None
        loops.append(lp)
    return loops


DEREF = re.compile(r'\*\s*([A-Za-z_]\w*)')
_ADV_CACHE = {}


def deref_vars(cond):
    return [m.group(1) for m in DEREF.finditer(cond)]


def advances(text, ptr):
    """O texto contem um avanco PROVADO de `ptr`?

    `p = skip_ws(p)` NAO conta, e essa exclusao e o CASO 1 inteiro: uma funcao
    de pular pode devolver o mesmo ponteiro, e foi assim que `tokenize_if_block`
    girou para sempre. Aceitar reatribuicao por chamada como prova de avanco
    seria cegar o lint justamente para o bug que ele existe para pegar.
    """
    pat = _ADV_CACHE.get(ptr)
    if pat is None:
        p = re.escape(ptr)
        pat = re.compile(r'\b%s\s*\+\+'      # p++
                         r'|\+\+\s*%s\b'     # ++p
                         r'|\b%s\s*\+='      # p += n
                         r'|\b%s\s*=[^=]*\+' % (p, p, p, p))
        _ADV_CACHE[ptr] = pat
    return pat.search(text) is not None


def body_is_only_advance(body, ptr):
    """Corpo e exatamente `p++;` (com ou sem chaves)?"""
    b = body.strip()
    if b.startswith('{') and b.endswith('}'):
        b = b[1:-1].strip()
    b = b.rstrip(';').strip()
    return re.fullmatch(r'(\+\+\s*%s|%s\s*\+\+)' % (re.escape(ptr), re.escape(ptr)), b) is not None


def polarity_of(cond, ptr):
    """SKIP = consome os caracteres do conjunto. SCAN = para neles."""
    c = cond.replace('!=', ' NE ')
    # so a parte que testa o conjunto interessa; `*p &&` e guarda de fim
    if re.search(r'!\s*[A-Za-z_(]', c):
        return 'SCAN'
    if ' NE ' in c:
        return 'SCAN'
    if '==' in c or re.search(r'\b[A-Za-z_]\w*\s*\(', c):
        return 'SKIP'
    return None


GUARD_ADV = 160


def has_progress_guard(body, ptr):
    """`if (p == inicio) { p++; ... }` — a guarda do CASO 1.

    Exige as duas metades: a comparacao com uma copia salva do ponteiro E um
    avanco logo em seguida. So a comparacao nao prova nada.
    """
    p = re.escape(ptr)
    for m in re.finditer(r'\b%s\s*==\s*([A-Za-z_]\w*)|\b([A-Za-z_]\w*)\s*==\s*%s\b' % (p, p), body):
        other = m.group(1) or m.group(2)
        if other == ptr:
            continue
        # a copia tem que ter sido tirada DO PROPRIO ponteiro dentro do corpo
        if not re.search(r'\b%s\s*=\s*%s\b' % (re.escape(other), p), body):
            continue
        if advances(body[m.end():m.end() + GUARD_ADV], ptr):
            return True
    return False


def enclosing_function(src, pos):
    """Nome da funcao que contem `pos` — chave estavel para o .allow, que nao
    muda quando as linhas andam (numero de linha mudaria a cada edicao)."""
    best = '?'
    for m in re.finditer(r'^[A-Za-z_][\w \t\*]*?([A-Za-z_]\w*)\s*\([^;]*?\)\s*\{', src, re.M | re.S):
        if m.start() > pos:
            break
        best = m.group(1)
    return best


# ──────────────── avanco garantido em TODO caminho da volta ──────────────
# Um `p++` no corpo nao prova nada se estiver dentro de um `if`. O que prova e
# uma das duas coisas: um avanco no nivel RASO do corpo (executa sempre), ou
# uma cadeia if/else-if/**else** em que TODO ramo avanca ou sai. Sem o `else`
# final a cadeia tem um caminho de fuga — e o caminho de fuga e o bug.
def inner_of(body):
    b = body.strip()
    if b.startswith('{') and b.endswith('}'):
        return body[body.index('{') + 1:body.rindex('}')]
    return body


def stmt_spans(m):
    """Fatia um corpo nas suas instrucoes de nivel raso. `}` seguido de `else`
    nao termina a instrucao — a cadeia inteira e uma so."""
    spans = []
    i, n = 0, len(m)
    while i < n:
        while i < n and m[i] in ' \t\r\n':
            i += 1
        if i >= n:
            break
        start, depth = i, 0
        while i < n:
            c = m[i]
            if c in '{([':
                depth += 1
            elif c in '})]':
                depth -= 1
                if depth == 0 and c == '}':
                    i += 1
                    j = i
                    while j < n and m[j] in ' \t\r\n':
                        j += 1
                    if m[j:j + 4] == 'else' and (j + 4 >= n or not (m[j + 4].isalnum() or m[j + 4] == '_')):
                        i = j + 4
                        continue
                    break
            elif c == ';' and depth == 0:
                i += 1
                break
            i += 1
        spans.append((start, min(i, n)))
    return spans


def shallow(text):
    """O texto sem nada do que estiver dentro de chaves aninhadas."""
    t = inner_of(text)
    m = mask(t)
    out, depth = [], 0
    for i, ch in enumerate(m):
        if ch == '{':
            depth += 1
            out.append(' ')
        elif ch == '}':
            depth -= 1
            out.append(' ')
        else:
            out.append(t[i] if depth == 0 else ' ')
    return ''.join(out)


def exits(text):
    return re.search(r'\b(break|return|goto)\b', shallow(text)) is not None


def branch_ok(text, ptr):
    return advances(text, ptr) or exits(text)


def if_chain_covers(st, ptr):
    m = mask(st)
    if not re.match(r'\s*if\s*\(', m):
        return False
    cuts = [0]
    for mm in re.finditer(r'\belse\b', m):
        pre = m[:mm.start()]
        if pre.count('{') == pre.count('}') and pre.count('(') == pre.count(')'):
            cuts.append(mm.start())
    cuts.append(len(st))
    branches, has_else = [], False
    for k in range(len(cuts) - 1):
        s = st[cuts[k]:cuts[k + 1]].strip()
        if s.startswith('else'):
            s = s[4:].strip()
            if not re.match(r'if\s*\(', s):
                has_else = True
        if re.match(r'if\s*\(', s):
            ms = mask(s)
            op = ms.index('(')
            cp = match_paren(ms, op)
            if cp < 0:
                return False
            s = s[cp + 1:]
        branches.append(s)
    return has_else and all(branch_ok(b, ptr) for b in branches)


def continues_covered(body, ptr):
    """Todo `continue` tem que ser precedido de um avanco no seu proprio bloco
    — senao ele e um atalho de volta ao topo com o ponteiro parado."""
    m = mask(body)
    for c in re.finditer(r'\bcontinue\s*;', m):
        pos, depth, j, bs = c.start(), 0, c.start() - 1, 0
        while j >= 0:
            if m[j] == '}':
                depth += 1
            elif m[j] == '{':
                if depth == 0:
                    bs = j + 1
                    break
                depth -= 1
            j -= 1
        if not advances(body[bs:pos], ptr):
            return False
    return True


def all_paths_advance(body, ptr):
    t = inner_of(body)
    m = mask(t)
    covered = False
    for (a, b) in stmt_spans(m):
        st = t[a:b]
        head = st.lstrip()
        if re.match(r'if\s*[(\s]', head):
            if if_chain_covers(st, ptr):
                covered = True
        elif re.match(r'(while|for|do|switch)\s*[({\s]', head):
            continue        # laco/switch pode nao executar nenhuma volta
        elif advances(st, ptr):
            covered = True
    return covered and continues_covered(body, ptr)


def first_effective(body, ptr):
    """A primeira instrucao do corpo que MEXE no ponteiro ou desvia — pulando
    declaracoes auxiliares (`const char* iter_start = p;`) que so guardam
    estado. Sem esse pulo, a forma do CASO 1 nao seria reconhecida: la o corpo
    comeca salvando o ponteiro antes de pular espaco."""
    t = inner_of(body)
    for (a, b) in stmt_spans(mask(t)):
        st = t[a:b]
        h = st.lstrip()
        if re.match(r'(while|for|do|switch|if)\s*[({\s]', h):
            return st
        if re.search(r'(^|[^\w.>-])%s\s*=[^=]' % re.escape(ptr), st):
            return st
        if advances(st, ptr):
            return st
    return ''


# ────────────────────────────── analise ──────────────────────────────────
class Finding(object):
    def __init__(self, path, kind, func, ptr, verdict, line, detail):
        self.path, self.kind, self.func, self.ptr = path, kind, func, ptr
        self.verdict, self.line, self.detail = verdict, line, detail

    def key(self):
        # chave do .allow: SEM numero de linha, que muda a cada edicao
        return '%s: %s %s %s' % (self.path, self.kind, self.func, self.ptr)

    def record(self):
        return '%s  ::  %s' % (self.key(), self.detail)


def innermost_enclosing(loops, start, end):
    best = None
    for lp in loops:
        if lp.start < start and lp.end >= end:
            if best is None or lp.start > best.start:
                best = lp
    return best


def strip_blocks(seg):
    """Apaga o que estiver dentro de `{}`: e codigo condicional, nao esta
    garantido no caminho."""
    m = mask(seg)
    out, depth = [], 0
    for i, ch in enumerate(m):
        if ch == '{':
            depth += 1
            out.append(' ')
        elif ch == '}':
            depth = max(0, depth - 1)
            out.append(' ')
        else:
            out.append(seg[i] if depth == 0 else ' ')
    return ''.join(out)


def advance_on_path(src, m, frm, to, ptr):
    """Todo caminho que sai de `frm` e chega em `to` ja avancou `ptr`?

    Subindo bloco a bloco: o que esta ANTES, no MESMO bloco, executa sempre que
    se chega aqui. E o que absolve `jash_run_multiline`, onde o par de lacos
    vive dentro de um `if` que ja fez `rp += 2` — a volta avancou, o par
    assimetrico nao trava nada.
    """
    cur = to
    for _ in range(12):
        if cur <= frm:
            break
        depth, j, bs = 0, cur - 1, None
        while j > frm:
            if m[j] == '}':
                depth += 1
            elif m[j] == '{':
                if depth == 0:
                    bs = j
                    break
                depth -= 1
            j -= 1
        start = bs + 1 if bs is not None else frm
        if advances(strip_blocks(src[start:cur]), ptr):
            return True
        if bs is None:
            break
        cur = bs
    return False


def conditional_skips(body, ptr, defs):
    """`if (*p == ',') p++;` no nivel raso do corpo — um pulo de um caractere
    so, condicional, que conta tanto quanto um laco de pular: o caractere que
    ele consome nao trava a volta. Sem modelar isto, `jash_b_pick` e
    `chg_parse_schema` seriam acusados por uma virgula que eles ja tratam."""
    t = inner_of(body)
    m = mask(t)
    chars, opaque = set(), set()
    for (a, b) in stmt_spans(m):
        st = t[a:b]
        if not re.match(r'\s*if\s*\(', st.lstrip()) and not re.match(r'\s*if\s*\(', st):
            continue
        sm = mask(st)
        if '(' not in sm:
            continue
        op = sm.index('(')
        cp = match_paren(sm, op)
        if cp < 0:
            continue
        branch = st[cp + 1:]
        if re.search(r'\b(break|continue|return|goto)\b', mask(branch)):
            continue
        if not (body_is_only_advance(branch, ptr) or all_paths_advance(branch, ptr)):
            continue
        c, o = charset(st[op + 1:cp], defs)
        chars |= c
        opaque |= o
    return chars, opaque


def declared_inside(body, ptr):
    """`const char* fp = linha;` DENTRO do corpo: o ponteiro renasce a cada
    volta, logo o progresso do laco externo nao depende dele."""
    return re.search(r'\b[A-Za-z_]\w*\s*\*+\s*%s\s*=' % re.escape(ptr), body) is not None


def break_conditions(body, base, loops, ptr):
    """Condicoes dos `break` do laco — o conjunto onde ele PARA quando isso nao
    esta escrito na condicao do `while`. E o que revela o terceiro caso desta
    familia: `split_statements` para em `at_keyword(p,"fi")`, um separador que
    nenhum laco de pular consome, e nada disso aparece na condicao do laco."""
    m = mask(body)
    out = []
    for br in re.finditer(r'\bbreak\s*;', m):
        pos = br.start()
        absolute = base + pos
        # um `break` dentro de laco/switch aninhado sai DAQUELE, nao deste
        if any(lp.start > base and lp.start <= absolute < lp.end for lp in loops):
            continue
        if re.search(r'\bswitch\s*\(', m[:pos]):
            continue
        for cond in governing_conds(body, m, pos):
            if re.search(r'\b%s\b' % re.escape(ptr), cond):
                out.append(cond)
    return out


def _if_cond_before(m, body, k):
    """Se o que vem imediatamente antes de `k` for `if (...)`, devolve a
    condicao e onde o `if` comeca."""
    while k >= 0 and m[k] in ' \t\r\n':
        k -= 1
    if k < 0 or m[k] != ')':
        return None, k
    d, op = 0, -1
    for t in range(k, -1, -1):
        if m[t] == ')':
            d += 1
        elif m[t] == '(':
            d -= 1
            if d == 0:
                op = t
                break
    if op < 0:
        return None, k
    e = op - 1
    while e >= 0 and m[e] in ' \t\r\n':
        e -= 1
    if e >= 1 and m[e - 1:e + 1] == 'if' \
            and (e - 2 < 0 or not (m[e - 2].isalnum() or m[e - 2] == '_')):
        return body[op + 1:k], e - 1
    return None, k


def governing_conds(body, m, pos):
    """TODAS as condicoes `if` que governam a posicao, subindo os blocos.

    Pegar so a mais proxima textualmente nao serve, e a diferenca decide um
    caso real: em `split_statements` o `break` mora em

        if (at_keyword(p, "fi", 2)) {
            if (if_depth > 0) { ...; continue; }
            break;
        }

    O `if` textualmente anterior ao `break` e `if (if_depth > 0)`, que nao fala
    do ponteiro — quem governa e o `at_keyword` do bloco de fora. Com a
    atribuicao ingenua o laco parece nao parar em nada e o bug fica invisivel.
    """
    conds, cur = [], pos
    for _ in range(8):
        c, k = _if_cond_before(m, body, cur - 1)
        if c is not None:
            conds.append(c)
            cur = k
            continue
        depth, j, bs = 0, cur - 1, None
        while j >= 0:
            if m[j] == '}':
                depth += 1
            elif m[j] == '{':
                if depth == 0:
                    bs = j
                    break
                depth -= 1
            j -= 1
        if bs is None:
            break
        cur = bs
    return conds


FUNC_DEF = re.compile(r'^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{', re.M)


def collect_skip_fns(src, masked, defs):
    """Funcoes de pular do proprio arquivo: `static const char* skip_ws(const
    char* p) { while (*p == ' ' || *p == '\\t') p++; return p; }`.

    Resolver esta indirecao e o que faz o CASO 1 ser analisavel: la o corpo do
    laco comeca com `p = skip_ws(p)`, e sem abrir a funcao o lint so veria uma
    chamada opaca — exatamente o ponto cego que deixaria passar o bug que ele
    existe para pegar. Mesmo principio da expansao de macro.
    """
    out = {}
    for m in FUNC_DEF.finditer(masked):
        name, params = m.group(1), m.group(2)
        pm = re.search(r'char\s*\*\s*([A-Za-z_]\w*)\s*$', params.strip())
        if not pm:
            continue
        arg = pm.group(1)
        ob = masked.find('{', m.end() - 1)
        cb = match_brace(masked, ob)
        if cb < 0 or cb - ob > 600:
            continue
        body = src[ob:cb + 1]
        t = inner_of(body)
        spans = stmt_spans(mask(t))
        cond = None
        ok = True
        for (a, b) in spans:
            st = t[a:b].strip()
            if re.match(r'while\s*\(', st):
                sm = mask(st)
                op = sm.index('(')
                cp = match_paren(sm, op)
                if cp < 0 or not body_is_only_advance(st[cp + 1:], arg):
                    ok = False
                    break
                cond = st[op + 1:cp]
            elif re.match(r'return\s', st) and re.search(r'\b%s\b' % re.escape(arg), st):
                continue
            else:
                ok = False
                break
        if ok and cond:
            out[name] = charset(cond, defs)
    return out


def skipfns_cons(src, masked, skipfns):
    """`p = skip_ws(p);` vira um consumidor SKIP sintetico, com o conjunto da
    funcao — do ponto de vista do avanco e o mesmo que o laco escrito ali."""
    out = []
    for m in re.finditer(r'\b([A-Za-z_]\w*)\s*=\s*([A-Za-z_]\w*)\s*\(\s*\1\s*\)\s*;', masked):
        ptr, fn = m.group(1), m.group(2)
        if fn not in skipfns:
            continue
        lp = Loop()
        lp.kw, lp.cond, lp.body = 'while', '', ''
        lp.start, lp.end = m.start(), m.end()
        lp.line = src.count('\n', 0, m.start()) + 1
        lp.ptr, lp.polarity = ptr, 'SKIP'
        c = Consumer()
        c.loop, c.ptr, c.kind = lp, ptr, 'SKIP'
        c.chars, c.opaque = skipfns[fn]
        out.append(c)
    return out


class Consumer(object):
    """Um laco que consome caracteres de `ptr` e provadamente avanca a cada
    volta interna. `chars`/`opaque` sao o conjunto em que ele PARA (SCAN) ou
    que ele CONSOME (SKIP)."""
    __slots__ = ('loop', 'ptr', 'kind', 'chars', 'opaque')


def consumers_of(loops, defs, ptr_filter=None):
    out = []
    for lp in loops:
        if lp.kw != 'while':
            continue
        vs = deref_vars(lp.cond)
        if not vs:
            continue
        ptr = vs[0]
        if not (body_is_only_advance(lp.body, ptr) or all_paths_advance(lp.body, ptr)):
            continue
        pol = polarity_of(lp.cond, ptr)
        chars, opaque = charset(lp.cond, defs)
        c = Consumer()
        c.loop, c.ptr = lp, ptr
        if pol == 'SKIP' and (chars or opaque):
            c.kind, c.chars, c.opaque = 'SKIP', chars, opaque
        else:
            # SCAN: para no que a condicao nega MAIS no que os `break` testam
            bchars, bop = set(), set()
            for cond in break_conditions(lp.body, lp.start, loops, ptr):
                cc, co = charset(cond, defs)
                bchars |= cc
                bop |= co
            c.kind = 'SCAN'
            c.chars = (chars if pol == 'SCAN' else set()) | bchars
            c.opaque = (opaque if pol == 'SCAN' else set()) | bop
        lp.ptr, lp.polarity = ptr, c.kind
        out.append(c)
    return out


def analyze_file(path, src):
    masked = mask(src)
    defs = collect_defines(src)
    loops = find_loops(src, masked)
    findings = []

    skipfns = collect_skip_fns(src, masked, defs)
    cons = consumers_of(loops, defs) + skipfns_cons(src, masked, skipfns)
    cons.sort(key=lambda c: c.loop.start)

    # ── (a) ASSIMETRIA DE CONJUNTO — o padrao do CASO 2 ──────────────────
    #
    # A direcao IMPORTA e e contraintuitiva. O par
    #     while (*p &&  A(*p)) p++;   /* pula  */
    #     while (*p && !B(*p)) p++;   /* varre */
    # trava exatamente nos caracteres de  B \ A : nao sao pulados (fora de A)
    # e param a varredura (dentro de B). O conjunto A \ B e inofensivo para o
    # travamento — no CASO 2, `(` estava so no PULA e nao travava nada; quem
    # travou foi `)`, que estava so no PARA. Reportar a diferenca simetrica
    # inteira seria meio ruido; so B \ A e bug.
    paired = set()
    for i, ca in enumerate(cons):
        if ca.kind != 'SKIP':
            continue
        a = ca.loop
        for cb in cons[i + 1:]:
            b = cb.loop
            if b.start <= a.end:
                continue
            if b.line - a.line > PAIR_GAP_LINES:
                break
            if cb.ptr != ca.ptr or cb.kind != 'SCAN':
                continue
            ptr = ca.ptr
            enc_a = innermost_enclosing(loops, a.start, a.end)
            enc_b = innermost_enclosing(loops, b.start, b.end)
            func = enclosing_function(src, a.start)
            same_enc = enc_a is not None and enc_a is enc_b
            # Um avanco INCONDICIONAL entre os dois ja fecha a questao. Um
            # avanco condicional (`if (*p == '"') { quote = *p; p++; }`) nao —
            # esse entra no conjunto do que se pula, mais abaixo.
            if advances(strip_blocks(src[a.end:b.start]), ptr):
                findings.append(Finding(path, 'PAR', func, ptr, 'SEGURO', a.line,
                                        'linhas %d/%d: ha avanco incondicional de `%s` '
                                        'entre os dois lacos' % (a.line, b.line, ptr)))
                if same_enc:
                    paired.add(enc_a.start)
                break
            # Um caractere que PARA a varredura mas e consumido por OUTRO laco
            # de pular da mesma volta nao trava nada. Contam todos os `while
            # (SEP(*p)) p++;` da iteracao, nao so o vizinho imediato — e assim
            # que `frecency_load` sobrevive a um ' ' fora do primeiro conjunto.
            skip_c, skip_o = set(ca.chars), set(ca.opaque)
            if same_enc:
                for s in cons:
                    if (s.kind == 'SKIP' and s.ptr == ptr
                            and enc_a.start < s.loop.start and s.loop.end <= enc_a.end):
                        skip_c |= s.chars
                        skip_o |= s.opaque
                cc, co = conditional_skips(enc_a.body, ptr, defs)
                skip_c |= cc
                skip_o |= co
            hang = cb.chars - skip_c
            hang_o = cb.opaque - skip_o
            where = 'linhas %d (pula) / %d (varre) sobre `%s`' % (a.line, b.line, ptr)

            def keep(verdict, why, _f=func, _p=ptr, _l=a.line, _w=where):
                findings.append(Finding(path, 'PAR', _f, _p, verdict, _l,
                                        '%s: %s' % (_w, why)))
            faltam = ' + '.join(x for x in (showset(hang) if hang else '',
                                            ('predicado(s) ' + ' '.join(sorted(hang_o)))
                                            if hang_o else '') if x)
            if not hang and not hang_o:
                keep('SEGURO', 'todo caractere que PARA a varredura e consumido por '
                               'um laco de pular da mesma volta — avanco garantido')
            elif not same_enc:
                # sem laco em volta (ou os dois em niveis diferentes) a
                # iteracao nao se repete: assimetria sem travamento
                keep('SEGURO', 'assimetria em %s, mas o par nao forma a volta de '
                               'um mesmo laco externo' % faltam)
            elif declared_inside(enc_a.body, ptr):
                keep('SEGURO', 'assimetria em %s, mas `%s` e redeclarado a cada '
                               'volta do laco externo' % (faltam, ptr))
            elif has_progress_guard(enc_a.body, ptr):
                keep('SEGURO', 'assimetria em %s, mas o laco externo tem guarda '
                               'de progresso explicita' % faltam)
            elif all_paths_advance(enc_a.body, ptr):
                keep('SEGURO', 'assimetria em %s, mas o laco externo avanca `%s` '
                               'em todo caminho' % (faltam, ptr))
            elif advance_on_path(src, masked, enc_a.start, a.start, ptr):
                keep('SEGURO', 'assimetria em %s, mas `%s` ja avancou antes do par, '
                               'em todo caminho da volta' % (faltam, ptr))
            else:
                senc, _ = charset(enc_a.cond, defs)
                if hang and not hang_o and polarity_of(enc_a.cond, ptr) == 'SCAN' \
                        and hang <= senc:
                    keep('SEGURO', 'assimetria em %s, mas o laco externo PARA '
                                   'nesses mesmos caracteres' % faltam)
                else:
                    det = 'TRAVA em %s — ' % faltam
                    det += ('nao e pulado pelo primeiro laco e para o segundo: '
                            'comprimento zero, `%s` imovel, iteracao identica. '
                            'PULA=%s PARA=%s' % (ptr, showset(skip_c),
                                                 showset(cb.chars) +
                                                 ((' + ' + ' '.join(sorted(cb.opaque)))
                                                  if cb.opaque else '')))
                    keep('SUSPEITO', det)
            if same_enc:
                paired.add(enc_a.start)
            break

    # ── (b) LACO EXTERNO SEM GUARDA — o padrao do CASO 1 ─────────────────
    #
    # Restrito de proposito a UMA forma: o laco que come um item por volta e
    # comeca pulando separadores (`while (*p) { while (SEP(*p)) p++; ... }`),
    # delegando todo o avanco a varreduras internas. E a forma dos dois bugs
    # reais. Sem esse recorte a heuristica acusa ~55 lacos deste repositorio,
    # quase todos legitimos — e um lint com 90% de ruido nao e usado, e
    # contornado.
    for lp in loops:
        if lp.kw != 'while' or not lp.body.strip().startswith('{'):
            continue
        if lp.body.count('\n') < OUTER_MIN_LINES or lp.start in paired:
            continue
        vs = deref_vars(lp.cond)
        if not vs:
            continue
        ptr = vs[0]
        fs = first_effective(lp.body, ptr)
        shape = None
        if re.match(r'\s*while\s*\(', fs):
            fsm = mask(fs)
            fop = fsm.index('(')
            fcp = match_paren(fsm, fop)
            if fcp >= 0:
                fcond, frest = fs[fop + 1:fcp], fs[fcp + 1:]
                if ptr in deref_vars(fcond) and body_is_only_advance(frest, ptr) \
                        and polarity_of(fcond, ptr) == 'SKIP':
                    shape = 'laco de pular separadores'
        elif re.match(r'\s*%s\s*=\s*[A-Za-z_]\w*\s*\(' % re.escape(ptr), fs.lstrip()) \
                and not advances(fs, ptr):
            shape = 'chamada de pular (`%s = %s`)' % (ptr, fs.strip().split('=', 1)[1].strip(' ;'))
        if shape is None:
            continue
        if declared_inside(lp.body, ptr):
            continue
        func = enclosing_function(src, lp.start)
        if has_progress_guard(lp.body, ptr):
            findings.append(Finding(path, 'EXTERNO', func, ptr, 'SEGURO', lp.line,
                                    'linha %d: guarda de progresso explicita '
                                    '(`if (%s == inicio) %s++`)' % (lp.line, ptr, ptr)))
        elif all_paths_advance(lp.body, ptr):
            findings.append(Finding(path, 'EXTERNO', func, ptr, 'SEGURO', lp.line,
                                    'linha %d: `%s` avanca em todo caminho do corpo'
                                    % (lp.line, ptr)))
        else:
            findings.append(Finding(path, 'EXTERNO', func, ptr, 'SUSPEITO', lp.line,
                                    'linha %d: laco que consome um item por volta sobre `%s`, '
                                    'com o avanco delegado a varreduras internas — se alguma '
                                    'delas parar sem consumir, a volta recomeca identica. '
                                    'Sem `%s++` incondicional e sem guarda `if (%s == inicio)`'
                                    % (lp.line, ptr, ptr, ptr)))
    return findings


def walk(scope):
    for s in scope:
        if os.path.isfile(s):
            yield s
        else:
            for root, dirs, files in os.walk(s):
                dirs[:] = [d for d in dirs if d not in ('.git', 'build', 'node_modules')]
                for f in sorted(files):
                    if f.endswith('.c'):
                        yield os.path.join(root, f)


def main(argv):
    mode = 'records'
    scope = []
    for a in argv[1:]:
        if a == '--records':
            mode = 'records'
        elif a == '--all':
            mode = 'all'
        elif a == '--counts':
            mode = 'counts'
        else:
            scope.append(a)
    if not scope:
        scope = ['c']

    found = []
    for path in walk(scope):
        try:
            with open(path, 'r', encoding='utf-8', errors='replace') as fh:
                src = fh.read()
        except OSError:
            continue
        try:
            found.extend(analyze_file(path, src))
        except Exception as exc:  # um arquivo torto nao pode derrubar o lint
            sys.stderr.write('lint-pointer-advance: %s: %s\n' % (path, exc))
    safe = [f for f in found if f.verdict == 'SEGURO']
    susp = [f for f in found if f.verdict == 'SUSPEITO']

    if mode == 'counts':
        print('SEGURO %d' % len(safe))
        print('SUSPEITO %d' % len(susp))
    elif mode == 'all':
        for f in sorted(found, key=lambda x: (x.path, x.line)):
            print('%-8s %s:%d  %s %s %s  ::  %s'
                  % (f.verdict, f.path, f.line, f.kind, f.func, f.ptr, f.detail))
        print('— SEGURO %d · SUSPEITO %d' % (len(safe), len(susp)))
    else:
        for f in sorted(susp, key=lambda x: (x.path, x.line)):
            print(f.record())
        sys.stderr.write('SEGURO %d\nSUSPEITO %d\n' % (len(safe), len(susp)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
