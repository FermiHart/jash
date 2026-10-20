#!/usr/bin/env bash
# mutation-check — um gate que NAO PODE FALHAR e indistinguivel de um gate que passa.
#
# JINV-061. Origem: `tests/test_jinv019_cmdword_quotes.sh` esteve verde desde
# que foi escrito e NUNCA por merito. Dois defeitos independentes, os dois
# invisiveis a olho nu numa revisao:
#
#   1. `out=$("$JASH" -c "..." 2>&1 | tail -1)` comparado por igualdade exata.
#      O scorecard de adoption vai para o stderr real e dispara a cada 10o
#      comando com contador PERSISTENTE entre invocacoes. A captura fundia o
#      aviso com a saida do programa, entao o gate passava ou falhava conforme
#      o contador global caisse num multiplo de 10. Verde por sorte.
#
#   2. `out=$(cmd | tail -1); rc=$?` — o `$?` de um pipeline e o do ULTIMO
#      comando do pipe. `tail` sempre sucede, logo `rc` era sempre 0, logo a
#      assercao "aspa nao terminada continua sendo erro" NAO PODIA FALHAR.
#
# O defeito 2 e o mais grave e o mais barato de matar: e sintatico. Uma
# assercao que nao pode falhar nao e um teste fraco — e um teste AUSENTE que
# ocupa a vaga de um presente e ainda produz a linha `ok` que convence o
# proximo agente a nao olhar. Custa mais que nao ter gate nenhum.
#
# ═══ ESTRATEGIA — por que estas e nao a mutacao de fonte por padrao ═══
#
# A pergunta da mutacao e sempre a mesma: se eu sabotar o que este gate diz
# proteger, ele fica VERMELHO? Tres formas de perguntar, medidas neste repo:
#
#   (b) MUTACAO DE FONTE (padrao-ouro): patch mecanico no .c, recompila, roda
#       so o gate correspondente, exige vermelho, reverte. MEDIDO AQUI:
#       configure 3s + build cheio 35s + 18s por mutacao incremental (o link
#       domina). Tres mutacoes ja passam de um minuto — caro demais para o
#       caminho default de uma suite de 7min. Fica em `src`, opt-in.
#       (Um `cmake -B` novo tambem nao configura sozinho neste ambiente: o
#       CMake acha `x86_64-elf-gcc` do projeto Beyond antes do clang. O modo
#       `src` fixa `/usr/bin/clang` e compila uma COPIA da arvore em /tmp —
#       nunca mutila o fonte real nem o `build/jash` que outro processo usa.)
#
#   (a) MUTANTE NULO DO BINARIO (barato, escolhido): troca `build/jash` por um
#       stub `exit 0` que nao imprime nada, dentro de um SHADOW ROOT de
#       symlinks — o binario real jamais e tocado. Os gates fazem
#       `cd "$(dirname "$0")/.."` e leem `$PWD/build/jash`, entao basta
#       invocar `bash $SHADOW/tests/<gate>.sh` para que enxerguem o stub.
#       E a mutacao mais radical possivel: o shell inteiro deixa de existir.
#       Um gate que continua VERDE contra um shell que nao faz nada nao esta
#       testando o shell — nao ha mutacao mais sutil que ele possa pegar.
#
#   (3) LINT DE ASSERCAO INFALSIFICAVEL (instantaneo, obrigatorio): varre
#       `tests/*.sh` procurando as formas que nao podem falhar. Pega o defeito
#       2 acima de forma mecanica, em milissegundos, em TODOS os `tests/*.sh`
#       de uma vez — inclusive nos que ainda nao existem. Melhor razao
#       custo/beneficio das tres, e a unica que escala com a suite.
#
# O default roda (3) e (a). `src` roda (b) sob demanda.
#
# Uso:
#   bash tools/mutation-check.sh              # lint + mutante nulo
#   bash tools/mutation-check.sh lint         # so o lint (com catraca)
#   bash tools/mutation-check.sh lint ARQ...  # lint cru em arquivos dados
#   bash tools/mutation-check.sh null         # so o mutante nulo do binario
#   bash tools/mutation-check.sh src          # mutacao de fonte (lento, ~1min+)
#   bash tools/mutation-check.sh all          # os tres
# Saida: 0 se nada novo apareceu; 1 e a lista se apareceu.

set -u
cd "$(dirname "$0")/.." || exit 2
REPO="$PWD"

# Sobrescritiveis para que o proprio gate possa PROVAR que a catraca reprova
# (ver tests/test_jinv061_mutation.sh). Uma catraca que nunca foi vista
# reprovando e mais uma afirmacao infalsificavel.
ALLOW="${MUTCHECK_ALLOW:-tools/mutation-check.allow}"
# ──── catraca: a secao DIVIDA so encolhe ────
# Uma excecao SEGURO e permanente (a forma parece o padrao mas a assercao tem
# como falhar de verdade). Uma excecao DIVIDA e uma assercao que realmente nao
# pode falhar e esta ali so ate alguem consertar. Se o numero subir, alguem
# escreveu um gate infalsificavel e o DECLAROU em vez de corrigir.
DEBT_CEILING="${MUTCHECK_CEILING:-0}"

# Cao de guarda. Ordem deliberada: `tools/deadline.sh` PRIMEIRO, porque e o
# watchdog canonico do repo (mata o GRUPO de processos e distingue 124 de 127).
# `timeout(1)` NAO existe neste macOS — e `command -v timeout` MENTE quando quem
# pergunta e o jash, onde `timeout` e builtin; os gates rodam sob bash, onde nao
# e. Por isso a checagem so vale rodando sob bash, como aqui. Sem nenhum dos
# dois, cai no laco em bash puro de run_gate_in: um harness de mutacao sem
# limite de tempo pendura para sempre, e travar e o desfecho MAIS provavel de
# sabotar um shell, nao o menos.
TO=""
[ -x "$REPO/tools/deadline.sh" ] && TO="$REPO/tools/deadline.sh"
[ -z "$TO" ] && command -v timeout  >/dev/null 2>&1 && TO=timeout
[ -z "$TO" ] && command -v gtimeout >/dev/null 2>&1 && TO=gtimeout

# O build do modo `src` sai serial por default do cmake, e serial ele leva
# minutos em vez dos 35s medidos. Ajustavel porque este repo costuma ter outro
# agente rodando a suite ao lado — MUTCHECK_JOBS=2 cede CPU.
JOBS="${MUTCHECK_JOBS:-$( { sysctl -n hw.ncpu || nproc; } 2>/dev/null || echo 4 )}"

# ═══════════════════════════════════════════════════════════════════════════
#  POR QUE cada classe nao pode falhar
# ═══════════════════════════════════════════════════════════════════════════
why() {
    case "$1" in
    PIPE-RC) echo "o \$? depois de 'var=\$(... | ...)' e o rc do ULTIMO comando do pipe (tail/head/cat/grep), nunca o do programa sob teste; filtros sempre sucedem, entao o rc e constante 0 e a assercao nunca reprova" ;;
    CONST-TEST) echo "os dois lados do teste sao literais — sem \$ e sem substituicao — logo o resultado independe do programa sob teste: a condicao e uma constante" ;;
    ALWAYS-TRUE-GUARD) echo "a guarda antes do && e um comando que sempre sai 0 (echo/printf/:/true/cat), entao o ramo || bad e codigo morto" ;;
    NO-FAIL-BRANCH) echo "'&& ok' sem '|| bad': quando a condicao e falsa nada e contado, o contador de falhas nao sobe e a suite segue verde" ;;
    MERGED-EXACT) echo "captura com 2>&1 comparada por igualdade exata: qualquer aviso de UX no stderr (nudge/adoption/loopguard) e fundido na captura e decide o veredito — passa ou falha por sorte, nao por merito" ;;
    *) echo "assercao infalsificavel" ;;
    esac
}

# ═══════════════════════════════════════════════════════════════════════════
#  (3) LINT — assercoes que nao podem falhar
# ═══════════════════════════════════════════════════════════════════════════
scan_one() {
    awk -v FILE="$1" '
BEGIN { SQ=sprintf("%c",39); DQ=sprintf("%c",34) }
function trim(x){ sub(/^[ \t]+/,"",x); sub(/[ \t]+$/,"",x); return x }
# Troca cada trecho entre aspas por UM token: @V@ se ele continha `$`, @L@ se
# era puramente literal. Duas armadilhas de uma vez:
#
#  1. Sem isto, o PROGRAMA SOB TESTE vira assercao. Em
#     `t <nome> <[ abc = abc ] && echo eq> <eq>` o `[ abc = abc ]` e a ENTRADA
#     que o jash tem que avaliar, nao o teste que decide o veredito — e a
#     assercao (comparar a saida com `eq`) e perfeitamente falsificavel.
#     Eram 21 de 21 nas primeiras deteccoes de CONST-TEST.
#
#  2. E por que MASCARAR e nao APAGAR: apagar o conteudo transforma
#     `[ "$out" = "brace" ]` em `[ = ]`, que parece constante e nao e — o `$out`
#     apagado era justamente o que fazia o teste depender do programa. Apagar
#     produziu 182 falsos positivos, mais do que nao ter detector nenhum.
#     Mascarar preserva a unica informacao que importa: tinha variavel ou nao.
function mask_quotes(x,   o,i,c,q,z,buf) {
    o=""; q=""; buf=""; z=length(x)
    for (i=1;i<=z;i++) {
        c=substr(x,i,1)
        if (q=="") { if (c==SQ || c==DQ) { q=c; buf="" } else o = o c }
        else if (c==q) { q=""; o = o (index(buf,"$")>0 ? "@V@" : "@L@") }
        else buf = buf c
    }
    if (q!="") o = o (index(buf,"$")>0 ? "@V@" : "@L@")
    return o
}
# Mascara SO os trechos entre aspas SIMPLES. Para o PIPE-RC nao serve a
# mascara total: `out="$(cmd | tail -1)"` tem a substituicao inteira dentro de
# aspas duplas, e apagar isso perderia justamente o caso procurado. Aspas
# simples, ao contrario, nunca contem substituicao — ali dentro e sempre dado.
# (Foi um `printf` com uma linha de exemplo entre aspas simples, dentro do
# proprio gate do JINV-061, que denunciou a diferenca.)
function mask_sq(x,   o,i,c,q,z) {
    o=""; q=""; z=length(x)
    for (i=1;i<=z;i++) {
        c=substr(x,i,1)
        if (q=="") {
            if (c==SQ) { q=SQ; continue }
            if (c==DQ) q=DQ
            o = o c
        } else if (q==SQ) {
            if (c==SQ) { q=""; o = o "@L@" }
        } else { o = o c; if (c==DQ) q="" }
    }
    return o
}
function snip(x, y){ y=trim(x); gsub(/[ \t]+/," ",y);
                     if(length(y)>110) y=substr(y,1,110) "..."; return y }
function emit(cls, ln, s){ printf "%s:%d:%s: %s\n", FILE, ln, cls, snip(s) }
# Corpo de heredoc e DADO, nao codigo. Um gate que gera um arquivo de teste
# via `cat > x <<FIM` tem shell dentro do heredoc que nunca sera executado por
# ESTE arquivo — inclusive, no caso do JINV-061, um fixture ENVENENADO de
# proposito. Sem esta guarda o lint acusa o proprio material de teste dele.
# (E a mesma fronteira do JINV-022: corpo de heredoc nao e comando.)
# Posicao do primeiro `<<` que e mesmo um abridor: FORA de aspas e FORA de
# comentario. Sem esta disciplina o detector confunde tres coisas que nao sao
# heredoc e fica cego no resto do arquivo:
#   `$(...(1<<4)...)` deslocamento aritmetico   (test_functional_battery.sh)
#   `# JMOD_CAP_CRYPTO = 1 << 7` num comentario (test_jinv025_module_caps.sh)
#   `chk heredoc <cat <<EOF...>` heredoc DENTRO do programa sob teste, entre
#   aspas — dado, nao codigo deste arquivo    (test_bash_parity.sh, jinv032)
function hd_pos(x,   i,c,q,z) {
    q=""; z=length(x)
    for (i=1;i<=z;i++) {
        c=substr(x,i,1)
        if (q=="") {
            if (c==SQ || c==DQ) { q=c; continue }
            if (c=="#" && (i==1 || substr(x,i-1,1)==" " || substr(x,i-1,1)=="\t")) return 0
            if (c=="<" && substr(x,i+1,1)=="<") {
                if (substr(x,i+2,1)=="<") { i=i+2; continue }   # herestring
                return i
            }
        } else if (c==q) q=""
    }
    return 0
}
function hd_open(x,   p,rest,i,c,w) {
    p = hd_pos(x)
    if (p == 0) return ""
    rest = substr(x, p+2)
    HDDASH = 0
    if (substr(rest,1,1) == "-") { HDDASH=1; rest=substr(rest,2) }
    while (substr(rest,1,1)==" " || substr(rest,1,1)=="\t") rest=substr(rest,2)
    w=""
    for (i=1;i<=length(rest);i++) {
        c=substr(rest,i,1)
        if (c==SQ || c==DQ) continue
        if (c ~ /[A-Za-z0-9_]/) w = w c
        else break
    }
    # delimitador que comeca com digito nao existe na pratica e e o que sobra
    # de um deslocamento aritmetico que escapou (`x << 4`)
    if (w ~ /^[0-9]/) return ""
    return w
}
{
    line=$0; sub(/\r$/,"",line)
    if (hd != "") {
        t2=line; if (hdstrip) sub(/^[ \t]+/,"",t2)
        if (trim(t2) == hd) hd=""
        next
    }
    w = hd_open(line)
    if (w != "") { hd=w; hdstrip=HDDASH; hdline=FNR }
    if (pend==0) { start=FNR; acc="" }
    t=line; cont=0
    # junta continuacao explicita (\) e operadores pendurados (&& / ||),
    # senao a forma canonica da suite (teste numa linha, || bad na seguinte)
    # escaparia de todos os detectores.
    if (t ~ /\\[ \t]*$/)            { sub(/\\[ \t]*$/,"",t); cont=1 }
    else if (t ~ /(&&|\|\|)[ \t]*$/) { cont=1 }
    acc = acc (acc=="" ? "" : " ") t
    if (cont) { pend=1; next }
    pend=0; n++; L[n]=acc; NL[n]=start
}
END {
    if (pend) { n++; L[n]=acc; NL[n]=start }
    # Heredoc que nunca fecha = o lint parou de enxergar dali para baixo. Cegueira
    # silenciosa num lint e o mesmo defeito que ele existe para caçar, entao ela
    # tem que ser BARULHENTA.
    if (hd != "")
        printf "%s:%d:HEREDOC-ABERTO: terminador %s nunca apareceu — o lint ficou cego daqui para baixo\n", FILE, hdline, hd

    # ── pre-passo: descobrir os VERBOS de assercao deste arquivo ──────────
    # A suite nao usa um par unico: ok/bad, ok/no, ok/err aparecem todos. Um
    # detector que procure literalmente "bad" produz 172 falsos positivos e
    # morre de desconfianca no primeiro dia. Entao: um verbo e de FALHA se o
    # corpo da funcao incrementa um contador de falha.
    for (i=1;i<=n;i++) {
        s=L[i]
        if (match(s, /^[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*\(\)/)) {
            fn=trim(substr(s,RSTART,RLENGTH)); sub(/[ \t]*\(\)$/,"",fn)
            bt=substr(s,RSTART+RLENGTH)
            # So olha adiante se o corpo NAO fecha na propria linha. A forma
            # canonica da suite poe as duas definicoes em linhas vizinhas:
            #   ok()  { pass=$((pass+1)); ... }
            #   bad() { fail=$((fail+1)); ... }
            # Engolir a linha seguinte fazia `ok` herdar o "fail" do `bad` e ser
            # classificado como verbo de FALHA — e ai NO-FAIL-BRANCH nunca
            # disparava, porque toda linha `&& ok` parecia ja ter ramo de falha.
            if (index(bt,"}") == 0)
                for (k=i+1; k<=n && k<=i+5; k++) { bt = bt " " L[k]; if (trim(L[k]) ~ /^\}/) break }
            if (bt ~ /[Ff][Aa][Ii][Ll]/ && bt ~ /(\+[ \t]*1|\+\+)/) FV[fn]=1
            else if (bt ~ /([Pp][Aa][Ss][Ss]|[Oo][Kk]_?[Cc]?[Nn]?[Tt]?)/ && bt ~ /(\+[ \t]*1|\+\+)/) OV[fn]=1
        }
    }
    okre="ok"; failre="bad|no|err|fail|nope|ko"
    j=0; for (v in OV) { okre = (j++ ? okre "|" v : v) }
    j=0; for (v in FV) { failre = (j++ ? failre "|" v : v) }
    OKRE="(^|[^A-Za-z0-9_])(" okre ")([ \t]|$)"
    FAILRE="(^|[^A-Za-z0-9_])(" failre ")([ \t]|$)"
    ASSERTRE="(^|[^A-Za-z0-9_])(" okre "|" failre ")[ \t]"

    for (i=1;i<=n;i++) {
        s=L[i]; ln=NL[i]; ls=trim(s)
        if (ls ~ /^#/ || ls=="") continue
        ms_=mask_quotes(s); mls=trim(ms_)

        # ── A. PIPE-RC ────────────────────────────────────────────────────
        # var=$(... | ...) seguido da leitura de $?
        # Escape hatch legitimo: `exit "${PIPESTATUS[0]}"` DENTRO da captura
        # propaga o rc do programa sob teste para fora do pipe — o $? lido e
        # o dele, nao o do filtro, entao a assercao continua falsificavel.
        sq_=mask_sq(s)
        if (match(sq_, /[A-Za-z_][A-Za-z0-9_]*=\$\(/)) {
            body = substr(sq_, RSTART)
            probe = body; gsub(/\|\|/,"@@",probe)
            if (index(probe,"|") > 0 && index(body,"PIPESTATUS") == 0) {
                rdnow  = (sq_ ~ /;[ \t]*(local[ \t]+)?[A-Za-z_][A-Za-z0-9_]*=\$\?/)
                nxt = (i<n) ? trim(mask_sq(L[i+1])) : ""
                rdnext = (nxt ~ /^(local[ \t]+)?[A-Za-z_][A-Za-z0-9_]*=\$\?/) \
                      || (nxt ~ /^(if|\[|\[\[|test)[^#]*\$\?/)
                if (rdnow || rdnext) emit("PIPE-RC", ln, s)
            }
        }

        # ── B. CONST-TEST ─────────────────────────────────────────────────
        # assercao cujo teste so tem literais
        if (ms_ ~ ASSERTRE) {
            tmp=ms_
            while (match(tmp, /\[+[^]]*\]+/)) {
                seg=substr(tmp,RSTART,RLENGTH)
                if (seg !~ /\$/ && seg !~ /@V@/ && seg !~ /`/ && \
                    (seg ~ /[ \t](=|==|!=|-eq|-ne|-gt|-lt|-ge|-le)[ \t]/ || \
                     seg ~ /\[[ \t]*-[nz][ \t]/))
                    emit("CONST-TEST", ln, s)
                tmp=substr(tmp,RSTART+RLENGTH)
            }
        }

        # ── C. ALWAYS-TRUE-GUARD ──────────────────────────────────────────
        p=index(ms_,"&&")
        if (p>0 && ms_ ~ ("\\|\\|[ \t]*(" failre ")([ \t]|$)")) {
            guard=substr(ms_,1,p-1); g2=trim(guard)
            if (index(guard,"|")==0 && g2 ~ /^(echo|printf|:|true|cat|sleep)([ \t]|$)/)
                emit("ALWAYS-TRUE-GUARD", ln, s)
        }

        # ── D. NO-FAIL-BRANCH ─────────────────────────────────────────────
        # `[ cond ] && ok "..."` sem nenhum ramo que conte falha
        if (ms_ ~ ("&&[ \t]*(" okre ")([ \t]|$)") && ms_ !~ FAILRE && \
            mls ~ /^(\[|\[\[|test|!)/)
            emit("NO-FAIL-BRANCH", ln, s)

        # ── E. MERGED-EXACT ───────────────────────────────────────────────
        if (match(s, /^[ \t]*(local[ \t]+)?[A-Za-z_][A-Za-z0-9_]*=/)) {
            vn=trim(substr(s,RSTART,RLENGTH)); sub(/^local[ \t]+/,"",vn); sub(/=$/,"",vn)
            # `2>&1 >/dev/null` NAO e fusao: manda o stderr para a captura e o
            # stdout para o lixo. E o idioma correto de capturar SO o stderr,
            # e usa-lo e deliberado — nao entra como achado.
            #
            # `>/dev/null 2>&1` (JINV-127) tambem NAO e fusao: a ordem inversa
            # manda os DOIS fluxos para o lixo. Nada fundido chega na captura,
            # entao o stderr nao pode decidir veredito nenhum — tipicamente o
            # idioma `cmd >/dev/null 2>&1; echo rc=$?`, onde o que se captura e
            # so o echo. Acusar isso e falso positivo, e um lint que grita em
            # codigo legitimo ensina o proximo agente a silenciar o lint.
            if (s ~ /=\$\(/ && s ~ /2>&1/ && s !~ /2>&1[ \t]*1?>[ \t]*\/dev\/null/ \
                                          && s !~ /1?>[ \t]*\/dev\/null[ \t]*2>&1/) \
                { mg[vn]=ln; mi[vn]=i; ms[vn]=s }
            else if (s ~ /=/)              { delete mg[vn] }
        }
        tmp=s
        while (match(tmp, /\[+[^]]*\]+/)) {
            seg=substr(tmp,RSTART,RLENGTH)
            if (seg ~ /[ \t](=|==|!=)[ \t]/) {
                for (v in mg) {
                    if (i-mi[v] <= 8 && seg ~ ("[$]\\{?" v "\\}?([^A-Za-z0-9_]|$)")) {
                        emit("MERGED-EXACT", mg[v], ms[v]); dead[v]=1
                    }
                }
                for (v in dead) delete mg[v]
                split("", dead)
            }
            tmp=substr(tmp,RSTART+RLENGTH)
        }
    }
}' "$1"
}

norm() { sed -E 's/^([^:]+):[0-9]+:/\1:/'; }

do_lint() {
    local raw="" f
    if [ "$#" -gt 0 ]; then
        for f in "$@"; do raw="$raw$(scan_one "$f")
"; done
        # modo cru: sem catraca, qualquer achado e erro (usado pelo auto-teste)
        raw=$(printf '%s' "$raw" | sed '/^$/d')
        if [ -n "$raw" ]; then
            printf '%s\n' "$raw"
            return 1
        fi
        return 0
    fi

    for f in tests/*.sh; do raw="$raw$(scan_one "$f")
"; done
    local atual permitido novos mortos rc=0
    atual=$(printf '%s' "$raw" | sed '/^$/d' | norm | sort -u)

    [ -f "$ALLOW" ] || : > "$ALLOW"
    permitido=$(grep -vE '^[[:space:]]*#|^[[:space:]]*$' "$ALLOW" 2>/dev/null | sort -u)

    novos=$(comm -23 <(printf '%s\n' "$atual" | sed '/^$/d') <(printf '%s\n' "$permitido" | sed '/^$/d'))
    mortos=$(comm -13 <(printf '%s\n' "$atual" | sed '/^$/d') <(printf '%s\n' "$permitido" | sed '/^$/d'))

    if [ -n "$novos" ]; then
        echo "✗ assercao que NAO PODE FALHAR (nova):"
        local cls last=""
        while IFS= read -r l; do
            [ -z "$l" ] && continue
            printf '    %s\n' "$l"
            cls=$(printf '%s' "$l" | sed -E 's/^[^:]+:([A-Z-]+):.*/\1/')
            if [ "$cls" != "$last" ]; then
                printf '      POR QUE: %s\n' "$(why "$cls")"
                last="$cls"
            fi
        done <<< "$novos"
        echo "  Conserte a assercao. Se ela REALMENTE pode falhar apesar da forma,"
        echo "  acrescente a linha em $ALLOW precedida de '# porque: <razao>'."
        rc=1
    fi
    if [ -n "$mortos" ]; then
        echo "✗ entradas obsoletas em $ALLOW (a linha sumiu ou mudou — remova-as):"
        printf '%s\n' "$mortos" | sed 's/^/    /'
        rc=1
    fi

    local n_debt
    # O marcador e um token ASCII fixo, NAO a prosa do cabecalho. Com um regex
    # sobre o texto ("DIVIDA — nao pode falhar"), um travessao multi-byte ou
    # qualquer reescrita do titulo faria o marcador nunca casar — e ai n_debt
    # seria 0 para sempre e o teto viraria decoracao, sem que nada acusasse.
    # Zero-por-secao-vazia e zero-por-marcador-quebrado sao indistinguiveis.
    if ! grep -q '@@DIVIDA@@' "$ALLOW" 2>/dev/null; then
        echo "✗ $ALLOW perdeu o marcador '@@DIVIDA@@' — sem ele a catraca de divida nao conta nada"
        rc=1
    fi
    n_debt=$(awk '/@@DIVIDA@@/{f=1;next} f && $0 !~ /^[[:space:]]*#/ && NF' "$ALLOW" | wc -l | tr -d ' ')
    if [ "$n_debt" -gt "$DEBT_CEILING" ]; then
        echo "✗ a divida de assercao infalsificavel SUBIU: $n_debt (teto $DEBT_CEILING)"
        echo "  Esta secao so encolhe. Conserte em vez de declarar."
        rc=1
    elif [ "$n_debt" -lt "$DEBT_CEILING" ]; then
        echo "✗ a divida caiu para $n_debt — abaixe DEBT_CEILING para $n_debt neste mesmo commit"
        rc=1
    fi

    local sem_porque
    sem_porque=$(awk '/^[[:space:]]*#/ { if ($0 ~ /porque:/) seen=1; next } /^[[:space:]]*$/ { seen=0; next } { if (!seen) print "    " $0; seen=0 }' "$ALLOW")
    if [ -n "$sem_porque" ]; then
        echo "✗ excecao sem '# porque:' na linha anterior (divida escondida):"
        printf '%s\n' "$sem_porque"
        rc=1
    fi

    local n_atual
    n_atual=$(printf '%s\n' "$atual" | sed '/^$/d' | wc -l | tr -d ' ')
    [ "$rc" -eq 0 ] && echo "✓ nenhuma assercao infalsificavel nova (seguro: $((n_atual - n_debt)) · divida: $n_debt)"
    return $rc
}

# ═══════════════════════════════════════════════════════════════════════════
#  (a) MUTANTE NULO DO BINARIO
# ═══════════════════════════════════════════════════════════════════════════
# Os gates abaixo alegam exercitar o binario. Contra um `jash` que so faz
# `exit 0` todos TEM que ficar vermelhos. Um sobrevivente esta verde por outra
# razao que nao o comportamento do shell.
#
# CRITERIO DE ENTRADA: so entra aqui gate que INVOCA "$JASH". Gates de lint de
# fonte (JINV-029 truncamento, JINV-030 derivacao de tetos, JINV-001 honestidade
# de build) sobrevivem ao mutante nulo com toda a razao — eles nao afirmam nada
# sobre o binario. Declarar um deles aqui produz um falso achado; foi o que
# aconteceu com JINV-030 na primeira versao desta lista.
# E gate que RECONSTROI o binario (JINV-001 roda `make build`) nunca entra:
# ele apaga a propria mutacao. Ver o comentario de make_shadow.
NULL_GATES="
tests/test_jinv008_redir_restore.sh
tests/test_jinv009_glob_spaces.sh
tests/test_jinv010_cmdsub_size.sh
tests/test_jinv011_printf_ls_args.sh
tests/test_jinv016_execjson_inject.sh
tests/test_jinv019_cmdword_quotes.sh
tests/test_jinv020_cmdsub_separator.sh
tests/test_jinv021_pipe_segment.sh
tests/test_jinv022_heredoc_body.sh
tests/test_jinv026_long_line_structure.sh
tests/test_jinv027_unquote_overflow.sh
tests/test_jinv032_separator_family.sh
tests/test_jinv033_glob_family.sh
tests/test_jinv036_block_tail.sh
tests/test_jinv040_tilde_assign.sh
tests/test_jinv046_unknown_flags.sh
tests/test_jinv055_assert.sh
tests/test_jinv056_why.sh
tests/test_jinv057_resume.sh
tests/test_jinv059_nudge_killswitch.sh
tests/test_jinv066_test_negation.sh
tests/test_jinv067_invocation_flags.sh
tests/test_jinv068_stdin_multiline.sh
tests/test_jinv069_noexec_refusal.sh
tests/test_jinv070_block_comment.sh
tests/test_jinv071_block_continuation.sh
tests/test_jinv072_keyword_is_word.sh
tests/test_jinv073_exit255_not_break.sh
tests/test_jinv074_if_tail_once.sh
tests/test_jinv075_loop_tail_break.sh
tests/test_jinv076_printf_no_arg_ceiling.sh
tests/test_jinv079_echo_argv.sh
tests/test_jinv081_compound_accumulator.sh
tests/test_jinv082_loop_body_heap.sh
tests/test_jinv086_silent_caps.sh
tests/test_jinv092_heredoc_delim.sh
tests/test_jinv093_escaped_operator.sh
tests/test_jinv088_cmdsub_body.sh
tests/test_jinv089_dollar_as_data.sh
tests/test_jinv090_eval_no_reexpand.sh
tests/test_jinv094_operator_continuation.sh
tests/test_jinv095_multiple_heredocs.sh
"

# Monta um shadow root de symlinks onde SO o binario e falso.
# Nunca toca em $REPO/build/jash — outro processo pode estar usando.
#
# O `build/` do shadow fica VAZIO fora o binario, de proposito. A primeira
# versao deste harness symlinkava o conteudo de build/ (CMakeCache.txt,
# CMakeFiles/...) e o resultado foi didatico: `test_jinv001_build_honesty.sh`
# roda `make build` como parte do que ele testa, achou o cache pelo symlink,
# COMPILOU POR CIMA do stub — e todos os 41 gates seguintes rodaram contra um
# jash de verdade e "sobreviveram". Um harness de mutacao que perde a mutacao
# no meio do caminho reporta 100% de sobrevivencia, ou seja, exatamente o
# resultado que ele existe para desmentir. Sem cache no shadow, nenhum gate
# consegue construir ali; e `stub_intact` confere o corpo de delito depois de
# cada gate em vez de confiar.
make_shadow() {
    local sh="$1" bin="$2" e b
    # `.git` NUNCA entra no shadow. Nenhum gate precisa dele, e um symlink para o
    # repositorio de verdade dentro de um diretorio descartavel e uma armadilha
    # esperando: qualquer `rm -rf` que um dia resolva seguir symlink leva a
    # historia junto. (O guard do proprio jash gritou "deleting a git repo root"
    # quando limpei um shadow — `rm -rf` nao segue symlink e nada se perdeu, mas
    # o susto e a correcao.) Nao existe motivo para correr esse risco.
    for e in "$REPO"/* "$REPO"/.[!.]*; do
        [ -e "$e" ] || continue
        b=$(basename "$e")
        case "$b" in build|.git) continue ;; esac
        ln -s "$e" "$sh/$b" 2>/dev/null
    done
    mkdir -p "$sh/build"
    cp "$bin" "$sh/build/jash" && chmod +x "$sh/build/jash"
}

# O binario do shadow ainda e o mutante que instalamos?
stub_intact() { cmp -s "$1/build/jash" "$2"; }

# Roda um gate no shadow com LIMITE DE TEMPO obrigatorio.
#
# Esta maquina nao tem `timeout` nem `gtimeout`, e sem limite o harness pendura
# para sempre: o primeiro mutante de fonte fez o JINV-057 entrar em laco
# infinito (justamente a classe de bug que aquele gate existe para pegar) e o
# `src` ficou preso quatro minutos sem imprimir nada. Um harness de mutacao TEM
# que sobreviver ao mutante — travar e o desfecho mais provavel de sabotar um
# shell, nao o menos. Sem `timeout` no PATH, faz-se o limite em bash puro.
run_gate_in() {   # $1=shadow  $2=gate  [$3=limite_s]  -> rc (124 = estourou)
    local sh="$1" g="$2" lim="${3:-90}" pid waited=0 rc=0
    if [ -n "$TO" ]; then
        $TO "$lim" bash "$sh/$g" >/dev/null 2>&1
        return $?
    fi
    bash "$sh/$g" >/dev/null 2>&1 &
    pid=$!
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$lim" ]; then
            kill -9 "$pid" 2>/dev/null
            # o gate morreu, mas o jash do shadow pode ter ficado orfao
            pkill -9 -f "$sh/build/jash" 2>/dev/null
            wait "$pid" 2>/dev/null
            return 124
        fi
        sleep 1; waited=$((waited+1))
    done
    wait "$pid"; rc=$?
    return $rc
}

do_null() {
    local sh stub rc=0 surv=0 kills=0 g rcg
    sh=$(mktemp -d) || return 2
    stub=$(mktemp) || return 2
    trap 'rm -rf "$sh" "$stub"' RETURN
    printf '#!/bin/sh\nexit 0\n' > "$stub"; chmod +x "$stub"
    make_shadow "$sh" "$stub"

    echo "── mutante NULO: build/jash → 'exit 0' (shadow root, binario real intacto)"
    for g in $NULL_GATES; do
        [ -f "$REPO/$g" ] || continue
        cp "$stub" "$sh/build/jash"; chmod +x "$sh/build/jash"
        run_gate_in "$sh" "$g" 90; rcg=$?
        [ "$rcg" -eq 124 ] && printf '  ! %s estourou o limite de tempo (contado como morto)\n' "$g"
        if [ "$rcg" -eq 0 ]; then
            if ! stub_intact "$sh" "$stub"; then
                surv=$((surv+1)); rc=1
                printf '  ✗ MUTACAO PERDIDA  %s\n' "$g"
                printf '      o gate reconstruiu o binario no shadow: o veredito dele nao vale\n'
                continue
            fi
            surv=$((surv+1)); rc=1
            printf '  ✗ SOBREVIVEU  %s\n' "$g"
            printf '      o gate ficou VERDE contra um shell que nao faz nada — ele nao esta testando o shell\n'
        else
            stub_intact "$sh" "$stub" || {
                printf '  ! ATENCAO  %s reconstruiu o binario no shadow (removido do conjunto?)\n' "$g"; }
            kills=$((kills+1))
        fi
    done
    if [ "$rc" -eq 0 ]; then
        echo "✓ mutante nulo morto por todos os $kills gates declarados"
    else
        echo "  $surv sobrevivente(s) de $((surv+kills))"
    fi
    return $rc
}

# ═══════════════════════════════════════════════════════════════════════════
#  (b) MUTACAO DE FONTE — padrao-ouro, opt-in (lento)
# ═══════════════════════════════════════════════════════════════════════════
# Formato: gate|arquivo|sed-expr|descricao   (o sed-expr nao pode conter "|")
# Poucas e de alto valor: cada uma custa ~18s de rebuild (o link domina).
#
# Um mutante que NAO APLICA e pior que nenhum: ele nao muda nada, o gate passa,
# e o relatorio conta isso como "gate matou o mutante". Por isso patch que nao
# altera o arquivo, ou que nao compila, e ERRO — nunca SKIP.
# O par gate↔mutante e uma AFIRMACAO e pode estar errada. Na primeira versao os
# dois sobreviveram, e nenhum dos dois por fraqueza do gate:
#   - `resume` foi pareado com JINV-046, que nao exercita `resume` (quem cobre a
#     guarda de flag do resume e o JINV-057);
#   - o teto de linha foi pareado com JINV-010, que so afirma "nao trava" — ele
#     IMPRIME `len=131071` e nao confere o numero, entao um teto pela metade
#     passa batido. Quem quebra com teto de 64K e o JINV-007 (script de 70 KiB).
# Sobrevivente e sempre duas hipoteses: gate fraco OU par errado. Confira o par
# antes de acusar o gate.
#
# ═══ REGRA DE ESCRITA DESTA TABELA (custou uma ironia para aprender) ═══════
#
# A string abaixo esta entre ASPAS DUPLAS, entao o shell a expande NA HORA DA
# ATRIBUICAO. Na DESCRICAO (o 4o campo) isso significa:
#
#   - CRASE e substituicao de comando: `\|` EXECUTA `\|`, a descricao perde o
#     trecho e o harness cospe "|: command not found" a cada rodada;
#   - `$` expande: escrever X$0 numa descricao produz X/bin/bash.
#
# Nos tres primeiros campos o `$` e NECESSARIO (ancora de sed: `$/`, `{$/`) e
# sobrevive porque o byte seguinte nao inicia um nome de variavel. Na
# descricao, nao escreva NEM crase NEM `$`: escreva o simbolo por extenso.
#
# O registro vale por si: a descricao do mutante do JINV-093 — o conserto de
# "DADO VIRANDO COMANDO" — VIROU COMANDO ao descrever o defeito, dentro do
# arquivo cujo trabalho e verificar se os gates conseguem falhar. E a decima
# aparicao da familia no mesmo dia (argv[0] do exec-json, separador em aspas
# aninhadas, `;;` em aspas, palavra-chave em aspas, `|` em aspas, IFS em
# palavra citada, corpo de `$( )` truncado, o idioma X-cifrao-zero do
# autoconf, um titulo de teste no test_suite.sh, e esta). A familia nao mora
# no parser: mora em toda fronteira onde texto que descreve vira texto que
# executa.
SRC_MUTANTS="
tests/test_jinv057_resume.sh|c/builtins/builtin_resume.c|s/if (jash_reject_unknown_flag(\"resume\"/if (0 \&\& jash_reject_unknown_flag(\"resume\"/|resume deixa de recusar flag desconhecida (JINV-046/057)
tests/test_jinv007_script_size.sh|c/core/limits.h|/^#define JASH_LINE_CAP  /s/(128 \* 1024)/(64 * 1024)/|teto de linha cai pela metade — script de 70 KiB volta a ser truncado (JINV-007/029)
tests/test_jinv066_test_negation.sh|c/builtins/registry.c|s/jash_builtin_test_argv }/0 }/; s/jash_builtin_bracket_argv }/0 }/|test e [ voltam a porta de string: o ! vira operando e [ ! -f x ] da SEMPRE verdadeiro (JINV-066)
tests/test_jinv065_glob_star_run.sh|c/parser/case_stmt.c|s/^            while (\*pattern == '\*') pattern++;$/            pattern++;/|o ramo quente do case volta a avancar UM asterisco: a corrida de * explode de novo (JINV-065)
tests/test_jinv073_exit255_not_break.sh|c/core/state.h|s/rc == JASH_RC_BREAK \&\& s \&\& s->loop_ctl == JASH_LOOPCTL_BREAK/rc == JASH_RC_BREAK/; s/rc == JASH_RC_CONTINUE \&\& s \&\& s->loop_ctl == JASH_LOOPCTL_CONTINUE/rc == JASH_RC_CONTINUE/|o canal de controle volta a ser o codigo de saida: um (exit 255) do usuario vira break (JINV-073)
tests/test_jinv074_if_tail_once.sh|c/parser/eval.c|s/blk = blk_stack;/blk = line;/|o tratador de if volta a receber a linha inteira: a cauda depois do fi ganha dois donos e roda duas vezes (JINV-074)
tests/test_jinv075_loop_tail_break.sh|c/parser/for_loop.c|s/if (jash_loopctl_pending(s)) return exit_code;/;/|a cauda depois do done volta a rodar por cima de um break em voo, e o rc dela apaga o pedido (JINV-075)
tests/test_jinv076_printf_no_arg_ceiling.sh|c/builtins/printf.c|s/int argc = in_argc;/int argc = in_argc > 1024 ? 1024 : in_argc;/|o teto de 1024 argumentos do printf volta, agora descartando em silencio (JINV-076)
tests/test_jinv070_block_comment.sh|c/parser/quotes.c|s/^            if (c == '\\\\n') {$/            if (0) {/|o comentario deixa de acabar na nova-linha e volta a correr ate o fim do texto: o fi do bloco e engolido (JINV-070)
tests/test_jinv071_block_continuation.sh|c/parser/quotes.c|s/^        if (c == '\\\\\\\\' \&\& p\[1\] == '\\\\n') {       \/\* JINV-071: juncao POSIX \*\/$/        if (0) {/|a juncao barra-invertida+nova-linha some de dentro dos blocos: a segunda metade do argumento vira comando (JINV-071)
tests/test_jinv072_keyword_is_word.sh|c/parser/script.c|s/^            char nx = q\[kl\];$/            char nx = ' ';/|a fronteira DIREITA da palavra-chave some: echo first volta a fechar o if no meio da palavra e rst vira comando (JINV-072)
tests/test_jinv079_echo_argv.sh|c/builtins/registry.c|s/jash_b_echo_argv }/0 }/|o echo volta a porta de string: a saida se monta da linha crua, o branco citado da esquerda some e o de entre palavras nao colapsa (JINV-079)
tests/test_jinv086_silent_caps.sh|c/parser/eval.c|s/size_t fblen = strlen(fbody);/size_t fblen = strlen(fbody); if (fblen > 2047) fblen = 2047;/; s/memcpy(bodybuf, fbody, fblen + 1);/memcpy(bodybuf, fbody, fblen); bodybuf[fblen] = 0;/|o teto de 2048 volta na CHAMADA da funcao: o corpo fica guardado inteiro e e decapitado na hora de rodar, com rc 0 e stderr vazio (JINV-086)
tests/test_jinv086_silent_caps.sh|c/function_defs.c|s/if (blen < sizeof(f->body)) {/if (blen >= sizeof(f->body)) blen = sizeof(f->body) - 1; if (1) {/; s/memcpy(f->body, body, blen + 1);/memcpy(f->body, body, blen); f->body[blen] = 0;/|o teto de 2048 volta no ARMAZENAMENTO do corpo da funcao (JINV-086)
tests/test_jinv086_silent_caps.sh|c/parser/eval.c|s/memcpy(vbuf, raw_val, vlen);/if (vlen >= sizeof(vbuf_inline)) { vlen = (unsigned)sizeof(vbuf_inline) - 1; vbuf = vbuf_inline; } memcpy(vbuf, raw_val, vlen);/|o teto de 1023 volta no VALOR de variavel, escalar e de elemento de array (JINV-086)
tests/test_jinv086_silent_caps.sh|c/builtins/readonly.c|s/jash_env_set(s, name, eq + 1);/{ char v_[256]; int vi_ = 0; const char *v2_ = eq + 1; while (*v2_ \&\& vi_ < 254) v_[vi_++] = *v2_++; v_[vi_] = 0; jash_env_set(s, name, v_); }/|o teto de 254 volta no readonly: a variavel fica cortada E imutavel (JINV-086)
tests/test_jinv086_silent_caps.sh|c/builtins/local.c|s/if (eq \&\& strlen(val) + 1 > unquoted_cap) {/if (0) {/|o teto de 4095 volta no local (JINV-086)
tests/test_jinv086_silent_caps.sh|c/builtins/export.c|s/if (strlen(args) + 1 > value_cap) {/if (0) {/|o teto de JASH_ENV_VAL_LEN volta no export (JINV-086)
tests/test_jinv092_heredoc_delim.sh|c/parser/heredoc.c|s/if (quoted) \*quoted = q_any;/if (quoted) *quoted = 0;/|a palavra delimitadora deixa de reportar que estava CITADA: o corpo de heredoc-barra-EOF (e o de aspas) volta a ser EXPANDIDO, que e' metade do JINV-092
tests/test_jinv093_escaped_operator.sh|c/parser/param_expand.c|s/^static int jash_pe_is_operator_meta(char c) {$/static int jash_pe_is_operator_meta(char c) { (void)c; return 0;/|a barra do operador CITADO volta a morrer na expansao: o pipe escapado vira cano de verdade e o idioma X-cifrao-zero do autoconf executa X./configure (JINV-093)
tests/test_jinv088_cmdsub_body.sh|c/parser/cmdsub.c|s/if ((size_t)ilen + 1 > inner_cap) {/if ((size_t)ilen + 1 > inner_cap) { if (ilen + 1 > inner_cap) ilen = (unsigned)inner_cap - 1; } else if (0) {/|teto 1 de 3: o clamp de 512 volta na ANALISE de cifrao-parentese e decapita o enunciado do comando (JINV-088)
tests/test_jinv088_cmdsub_body.sh|c/parser/cmdsub.c|s/if (cleaned_need > cleaned_cap) {/if (cleaned_need > cleaned_cap \&\& 0) {/|teto 2 de 3: o de 512 volta na REMOCAO DE COMENTARIOS — consertar so o primeiro nao movia o penhasco um byte (JINV-088)
tests/test_jinv088_cmdsub_body.sh|c/parser/cmdsub.c|s/if ((size_t)need + 1 > inner_cap) {/if ((size_t)need + 1 > inner_cap \&\& 0) {/|teto 3 de 3: o de 512 volta na ANALISE de crase (JINV-088)
tests/test_jinv089_dollar_as_data.sh|c/parser/cmdsub.c|s/if (!sq \&\& p\[0\] == '\$' \&\& p\[1\] == '.') {   /if (0) {   /|o cifrao antes de crase deixa de ser blindado: quando a crase vira a saida do comando ele encosta num NOME e o valor e expandido duas vezes (JINV-089)
tests/test_jinv089_dollar_as_data.sh|c/parser/case_stmt.c|s/                jash_quote_unshield(tmp);        \/\* BUG-096 \*\//                jash_cmdsub_unprotect(tmp); jash_quote_unshield(tmp);/|o restauro do cifrao blindado volta a ser PREMATURO no case: desfaz a blindagem bem a tempo de a passada de parametros expandir (JINV-089)
tests/test_jinv089_dollar_as_data.sh|c/parser/cmdsub.c|s/p\[0\] == '\$' \&\& p\[1\] == '\$'/p[0] == '\$' \&\& 0/|o escudo fica LARGO DEMAIS: o segundo cifrao de cifrao-cifrao (o PID) e blindado e a expansao legitima morre — o bug oposto (JINV-089)
tests/test_jinv090_eval_no_reexpand.sh|c/parser/eval.c|s/    if (!jash_line_has_pipe(line)) {/    if (1) {/|eval.c volta a restaurar o escudo ANTES do cano: o motor de canos expande o texto ja expandido uma SEGUNDA vez (JINV-090)
tests/test_jinv090_eval_no_reexpand.sh|c/parser/eval.c|s/jash_cmdsub_protect_residual((char\*)line);/(void)0;/|o cifrao RESIDUAL deixa de ser blindado: o VALOR de uma variavel volta a virar sintaxe ao atravessar um cano (JINV-090)
tests/test_jinv090_eval_no_reexpand.sh|c/parser/pipeline.c|s/for (int i = 0; i < argc; i++) jash_cmdsub_unprotect(argv\[i\]);/(void)0;/|o restauro adiado some do argv do EXTERNO: o sentinela vaza cru para a saida (JINV-090)
tests/test_jinv094_operator_continuation.sh|c/main.c|s/        jash_join_operator_lines(pbuf, \&syntax_eof);/        (void)0;/|a juncao pela continuacao de operador some do pre-processamento: o cano no fim da linha volta a sumir em silencio, com rc 0 e a saida do lado esquerdo (JINV-094)
tests/test_jinv094_operator_continuation.sh|c/main.c|s/        if (syntax_eof) goto multiline_done;/        if (0) goto multiline_done;/|a recusa do programa que ACABA em operador pendente some: o lado esquerdo de um cano que nao existe roda assim mesmo (JINV-094)
tests/test_jinv095_multiple_heredocs.sh|c/parser/builtin_redir.c|s/^void jash_stdin_stream_sync(int drop_buffered) {/void jash_stdin_stream_sync(int drop_buffered) { (void)drop_buffered; return;/|a ressincronizacao do stdin do C some: o fim-de-arquivo fica aceso no stream depois do primeiro redirecionamento e o SEGUNDO cmd-menor-arquivo (logo, todo heredoc do segundo em diante) roda com entrada vazia e rc 0 (JINV-095)
tests/test_jinv095_multiple_heredocs.sh|c/main.c|/^#define JASH_HD_PER_LINE /s/16/1/|a linha logica volta a coletar UM heredoc so: o segundo operador chega ao avaliador como texto literal e as linhas do corpo dele voltam ao parser como COMANDOS (JINV-095)
"

do_src() {
    local work bdir rc=0 spec gate file expr desc sh t0 t1 rcg
    command -v cmake >/dev/null 2>&1 || { echo "SKIP src: cmake ausente"; return 0; }
    echo "── mutacao de FONTE (copia da arvore em /tmp; fonte real e build/ intactos)"
    work=$(mktemp -d) || return 2
    bdir=$(mktemp -d) || return 2
    trap 'rm -rf "$work" "$bdir"' RETURN

    ( cd "$REPO" && tar cf - --exclude=build --exclude=.git . ) | ( cd "$work" && tar xf - ) || return 2
    t0=$(date +%s)
    cmake -B "$bdir" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/bin/clang \
          "$work" >/dev/null 2>&1 || { echo "  SKIP src: configure falhou"; return 0; }
    cmake --build "$bdir" --target jash -j "$JOBS" >/dev/null 2>&1 || { echo "  SKIP src: build base falhou"; return 0; }
    t1=$(date +%s)
    echo "  build base: $((t1-t0))s"

    # O laco NAO pode rodar em subshell (`... | while`), senao o rc de falha
    # morre com ela e o modo devolve 0 mesmo com sobrevivente — um harness de
    # mutacao que nao consegue reprovar e o proprio defeito que ele caca.
    # ═══ A ARMADILHA DO OBJETO VELHO — por que existe o `touch` ═══
    #
    # `cp f f.orig` carimba `f.orig` com a hora da COPIA. O `sed` reescreve `f`
    # depois disso, o build produz um `.o` mais NOVO que os dois, e o
    # `mv f.orig f` devolve o conteudo original com a hora ANTIGA. A partir
    # dai `f` e mais velho que o `.o` e o `make` NAO RECOMPILA: o binario
    # continua carregando a mutacao, e o mutante SEGUINTE da lista e medido
    # contra um shell que ainda tem a sabotagem do anterior dentro.
    #
    # Medido em 2026-07-28 num harness de iteracao com o mesmo padrao: depois
    # do primeiro mutante, o segundo acusou 14 falhas em secoes que o codigo
    # que ele mutava nem toca — porque o primeiro continuava vivo no binario.
    # Um harness de mutacao contaminado reporta mortes que nao aconteceram, e
    # e a mesma doenca que o JINV-061 existe para matar: uma assercao que nao
    # pode falhar (aqui, um mutante que nao pode sobreviver) nao e um teste
    # forte — e um teste AUSENTE que ainda imprime a linha de sucesso.
    #
    # `restaura` devolve o fonte E carimba a hora AGORA, para que o proximo
    # build veja o arquivo como mais novo que o objeto e recompile de verdade.
    restaura() { mv "$1.orig" "$1"; touch "$1"; }

    local ruim="$work/.falhas"; : > "$ruim"
    while IFS='|' read -r gate file expr desc; do
        [ -n "${gate:-}" ] || continue
        if [ ! -f "$work/$file" ]; then
            echo "  ✗ ALVO AUSENTE  $file ($desc)"; echo x >> "$ruim"; continue
        fi
        cp "$work/$file" "$work/$file.orig"
        sed "$expr" "$work/$file.orig" > "$work/$file"
        if cmp -s "$work/$file" "$work/$file.orig"; then
            echo "  ✗ PATCH INERTE  $desc"
            echo "      o sed nao alterou $file — o alvo mudou de forma; um mutante que"
            echo "      nao muda nada faz o gate passar e conta como 'mutante morto'"
            echo x >> "$ruim"; restaura "$work/$file"; continue
        fi
        if ! cmake --build "$bdir" --target jash -j "$JOBS" >/dev/null 2>&1; then
            echo "  ✗ NAO COMPILA   $desc (o patch precisa produzir codigo valido)"
            echo x >> "$ruim"; restaura "$work/$file"; continue
        fi
        sh=$(mktemp -d); make_shadow "$sh" "$bdir/jash"
        run_gate_in "$sh" "$gate" 120; rcg=$?
        if [ "$rcg" -eq 0 ]; then
            echo "  ✗ SOBREVIVEU    $gate  ($desc)"
            echo "      duas hipoteses: o gate nao cobre o codigo mutado, ou o par"
            echo "      gate↔mutante esta errado. Confira o par ANTES de acusar o gate."
            echo x >> "$ruim"
        elif [ "$rcg" -eq 124 ]; then
            echo "  ✓ morto (TRAVOU) $gate  ($desc)"
            echo "      o mutante pendurou o gate; detectado, mas por timeout e nao"
            echo "      por assercao — o gate nao chegou a dar veredito."
        else
            echo "  ✓ morto         $gate  ($desc)"
        fi
        rm -rf "$sh"
        restaura "$work/$file"
    done <<SRCEOF
$SRC_MUTANTS
SRCEOF
    [ -s "$ruim" ] && rc=1
    # devolve o build ao estado limpo para a proxima chamada
    cmake --build "$bdir" --target jash -j "$JOBS" >/dev/null 2>&1
    return $rc
}

# ═══════════════════════════════════════════════════════════════════════════
MODE="${1:-default}"
[ "$#" -gt 0 ] && shift
case "$MODE" in
    lint)    do_lint "$@" ;;
    null)    do_null ;;
    src)     do_src ;;
    all)     r=0; do_lint || r=1; echo; do_null || r=1; echo; do_src || r=1; exit $r ;;
    default) r=0; do_lint || r=1; echo; do_null || r=1; exit $r ;;
    *) echo "uso: $0 [lint|null|src|all] [arquivos...]"; exit 2 ;;
esac
