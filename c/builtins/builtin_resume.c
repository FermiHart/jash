// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/resume — o bilhete que sabe se envelheceu.
//
// JINV-057 (Onda C1 do PLANO-10-ONDAS-AGENTE).
//
// ── Por que isto existe ────────────────────────────────────────────────────
//
// A coisa mais perigosa que uma sessão deixa para a próxima é um bilhete de
// handoff. Ele foi verdade quando escrito, chega à sessão seguinte com
// autoridade de fato — e o repositório andou embaixo dele.
//
// Nesta sessão o agente afirmou quatro coisas falsas. Nenhuma foi mentira: em
// todas ele verificou uma vez, continuou afirmando, e o mundo mudou. O
// `assert` (JINV-055) matou esse padrão DENTRO de uma sessão, amarrando a
// afirmação ao seu verificador. Este builtin o mata ATRAVESSANDO sessões, que
// é onde ele é pior — porque do outro lado não há ninguém que possa conferir.
//
// ── A ideia ────────────────────────────────────────────────────────────────
//
// O bilhete carrega a impressão digital do repositório no instante da escrita.
// Ao ser lido, ele se reverifica e diz **o que mudou desde então**.
//
// E há um segundo verificador, mais afiado: o bilhete é lido em busca dos
// caminhos que ele próprio cita. Um bilhete que diz "o conserto está em
// c/parser/eval.c, não mexa em if_block.c" contém, no seu próprio texto, a
// lista exata dos arquivos cuja mudança o invalidaria. Se `eval.c` foi tocado
// depois do bilhete, a sessão seguinte precisa saber disso ANTES de confiar
// naquela frase — e precisa saber que é sobre `eval.c`, não um aviso genérico.
//
// Custa um `stat` por caminho citado. Nenhuma varredura de árvore, nenhum
// `git status`, nenhuma dependência nova.
//
// ── O que NÃO faz (a parte que importa) ────────────────────────────────────
//
// Nunca diz "ainda vale". Não pode: verificou o HEAD e os arquivos que o
// bilhete cita — não o resto do mundo. Então relata as duas coisas, separadas:
// o que conferiu (`checks`) e o que ficou de fora (`not_verified`).
//
// É a mesma régua do JINV-051, onde `sigil chain verify` passou a declarar o
// que sustenta o seu `valid:true` em vez de afirmar validade a seco. Um
// veredito que afirma mais do que sustenta é a forma de mentira de máquina
// que esta auditoria inteira perseguiu; aqui ela apontaria diretamente para o
// próximo agente, no momento em que ele tem menos condição de duvidar.
//
// F E R M I ∞ H A R T

#include <builtins/builtin.h>
#include <core/state.h>
#include <core/drift.h>       /* jash_drift_git_head — reuso, não cópia */
#include <core/agent_json.h>  /* jash_json_esc */
#include <theme/io.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>   /* getcwd */
#include <limits.h>
#include <sys/syscalls.h>  /* jash_tmpdir — J1 fix */

#define RS_NOTE_MAX   4000    /* o bilhete em si */
#define RS_PATHS_MAX  16      /* caminhos citados que reverificamos */
#define RS_PATH_LEN   256

typedef struct {
    char  note[RS_NOTE_MAX];
    char  head[16];           /* git HEAD quando escrito */
    char  cwd[PATH_MAX];
    long  ts;
    int   ok;                 /* 1 = havia bilhete legível */
} rs_t;

static void rs_path(char* out, size_t cap) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(out, cap, "%s/.jash/resume.txt", home);
}

/* Formato: 3 linhas de cabeçalho + corpo livre. Texto puro de propósito —
 * o bilhete tem que ser legível por um humano com `cat` quando tudo o mais
 * falhar, inclusive este builtin. */
static int rs_load(rs_t* r) {
    memset(r, 0, sizeof *r);
    char p[PATH_MAX]; rs_path(p, sizeof p);
    FILE* f = fopen(p, "r");
    if (!f) return 0;
    char line[PATH_MAX + 64];
    if (fgets(line, sizeof line, f)) { line[strcspn(line,"\r\n")]='\0'; r->ts = atol(line); }
    if (fgets(line, sizeof line, f)) {
        size_t len = strcspn(line,"\r\n");
        if (len >= sizeof r->head) { fclose(f); return 0; }
        memcpy(r->head,line,len);r->head[len]='\0';
    }
    if (fgets(line, sizeof line, f)) {
        size_t len = strcspn(line,"\r\n");
        if (len >= sizeof r->cwd) { fclose(f); return 0; }
        memcpy(r->cwd,line,len);r->cwd[len]='\0';
    }
    size_t n = fread(r->note, 1, sizeof r->note - 1, f);
    r->note[n] = '\0';
    while (n > 0 && (r->note[n-1]=='\n' || r->note[n-1]=='\r')) r->note[--n] = '\0';
    fclose(f);
    r->ok = (r->ts > 0);
    return r->ok;
}

static int rs_save(const char* note, const char* head, const char* cwd) {
    char p[PATH_MAX]; rs_path(p, sizeof p);
    /* Garante ~/.jash — o mesmo cuidado do assertions.tsv. */
    { char d[PATH_MAX]; const char* h = getenv("HOME");
      if (h && *h) { snprintf(d, sizeof d, "%s/.jash", h); (void)mkdir(d, 0700); } }
    FILE* f = fopen(p, "w");
    if (!f) return 0;
    fprintf(f, "%ld\n%s\n%s\n%s\n", (long)time(NULL), head, cwd, note);
    fclose(f);
    return 1;
}

/* Extrai do TEXTO do bilhete os tokens que parecem caminho de arquivo.
 * Deliberadamente conservador: só conta o que tem barra ou extensão de código,
 * porque um falso positivo aqui vira um alerta de staleness sobre um arquivo
 * que o bilhete nunca quis dizer — ruído que corrói a confiança no aviso. */
static int rs_looks_like_path(const char* t, size_t len) {
    if (len < 3 || len >= RS_PATH_LEN) return 0;

    /* Um caminho tem pelo menos uma letra. Sem esta guarda, "83/83" e
     * "2026/07/27" viram caminhos por conterem barra, e o bilhete passa a
     * avisar que um arquivo inexistente "nao existe mais" — o falso positivo
     * exatamente contra o qual o comentario acima adverte.
     *
     * Achado ao USAR o proprio builtin para gravar o bilhete de handoff desta
     * sessao, trinta segundos depois de ele ficar pronto. O aviso estava
     * escrito e o defeito embarcou junto; so a experimentacao pegou. */
    int tem_letra = 0;
    for (size_t i = 0; i < len; i++) {
        char c = t[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) { tem_letra = 1; break; }
    }
    if (!tem_letra) return 0;

    if (memchr(t, '/', len)) return 1;
    static const char* const ext[] = { ".c",".h",".sh",".md",".py",".txt",".json",".mk", 0 };
    for (int i = 0; ext[i]; i++) {
        size_t el = strlen(ext[i]);
        if (len > el && memcmp(t + len - el, ext[i], el) == 0) return 1;
    }
    return 0;
}

static int rs_collect_paths(const char* note, char out[][RS_PATH_LEN], int cap) {
    int n = 0;
    const char* p = note;
    /* Os dois conjuntos TEM que ser o mesmo. Quando eram assimetricos — `(`
     * pulava mas nao parava, `)` parava mas nao pulava — um `)` no texto
     * deixava o ponteiro parado: o laco de pular nao o consumia, o de varrer
     * parava nele, comprimento zero, e a iteracao recomecava identica.
     * Laco infinito. O bilhete de handoff desta sessao tinha parenteses. */
    #define RS_SEP(c) ((c)==' '||(c)=='\t'||(c)=='\n'||(c)=='\r'||(c)==','|| \
                       (c)=='('||(c)==')'||(c)=='`'||(c)=='\''||(c)=='"')
    while (*p && n < cap) {
        const char* iter_start = p;
        while (*p && RS_SEP(*p)) p++;
        const char* st = p;
        while (*p && !RS_SEP(*p)) p++;
        size_t len = (size_t)(p - st);
        /* Guarda de avanco: nenhuma iteracao pode terminar onde comecou.
         * A simetria acima ja basta hoje, mas esta linha e o que impede a
         * CLASSE inteira de voltar se algum separador for editado no futuro.
         * A mesma guarda foi necessaria em tokenize_if_block, nesta sessao. */
        if (p == iter_start) { if (!*p) break; p++; continue; }
        /* pontuação final não faz parte do caminho */
        while (len > 0 && (st[len-1]=='.'||st[len-1]==':'||st[len-1]==';')) len--;
        if (rs_looks_like_path(st, len)) {
            int dup = 0;
            for (int i = 0; i < n; i++)
                if (strlen(out[i]) == len && memcmp(out[i], st, len) == 0) { dup = 1; break; }
            if (!dup) { memcpy(out[n], st, len); out[n][len] = '\0'; n++; }
        }
    }
    return n;
}

int jash_b_resume(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s;
    /* JINV-046: uma flag desconhecida não pode virar texto de bilhete —
     * seria a mesma falha do JINV-016, com o dado virando comando na
     * direção contrária. */
    { static const char* const ok_flags[] = { "--json", "--forget", 0 };
      if (jash_reject_unknown_flag("resume", args, ok_flags, w, ctx)) return 2; }

    const char* a = args ? args : "";
    while (*a == ' ' || *a == '\t') a++;

    int json = 0;
    if (strncmp(a, "--json", 6) == 0 && (a[6]=='\0' || a[6]==' ')) { json = 1; a += 6; while (*a==' ') a++; }

    if (strncmp(a, "--forget", 8) == 0) {
        char p[PATH_MAX]; rs_path(p, sizeof p);
        int had = (remove(p) == 0);
        if (json) jash_writef(w, ctx, "{\"forgotten\":%s}\n", had ? "true" : "false");
        else jash_writes(w, ctx, had ? "resume: bilhete apagado\r\n"
                                     : "resume: nao havia bilhete\r\n");
        return 0;
    }

    char cwd[PATH_MAX] = ".";
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");

    /* ── ESCREVER ───────────────────────────────────────────────────────── */
    if (*a) {
        char head[13]; jash_drift_git_head(cwd, head);
        if (!rs_save(a, head, cwd)) {
            jash_writes(w, ctx, "resume: nao consegui gravar o bilhete\r\n");
            return 1;
        }
        char paths[RS_PATHS_MAX][RS_PATH_LEN];
        int np = rs_collect_paths(a, paths, RS_PATHS_MAX);
        if (json)
            jash_writef(w, ctx, "{\"saved\":true,\"head\":\"%s\",\"watching\":%d}\n", head, np);
        else {
            jash_writef(w, ctx, "\033[1;32m✓\033[0m bilhete gravado \033[90m(HEAD %s)\033[0m\r\n", head);
            if (np > 0) {
                jash_writef(w, ctx, "  \033[90mvigiando %d arquivo(s) citado(s):\033[0m", np);
                for (int i = 0; i < np; i++) jash_writef(w, ctx, " %s", paths[i]);
                jash_writes(w, ctx, "\r\n");
            } else {
                jash_writes(w, ctx,
                    "  \033[90m(nenhum caminho citado — cite arquivos no bilhete e eles "
                    "serao reverificados)\033[0m\r\n");
            }
        }
        return 0;
    }

    /* ── LER ────────────────────────────────────────────────────────────── */
    rs_t r;
    if (!rs_load(&r)) {
        if (json) jash_writes(w, ctx, "{\"note\":null,\"reason\":\"no note recorded\"}\n");
        else jash_writes(w, ctx,
            "resume: nenhum bilhete desta sessao ou de anteriores\r\n"
            "        \033[90m(resume \"texto\" grava um; cite os arquivos que importam)\033[0m\r\n");
        return 0;
    }

    char head_now[13]; jash_drift_git_head(cwd, head_now);
    int head_moved = (r.head[0] && strcmp(r.head, head_now) != 0);
    long age = (long)time(NULL) - r.ts;

    char paths[RS_PATHS_MAX][RS_PATH_LEN];
    int np = rs_collect_paths(r.note, paths, RS_PATHS_MAX);
    int moved[RS_PATHS_MAX]; int gone[RS_PATHS_MAX];
    int n_moved = 0, n_gone = 0;
    for (int i = 0; i < np; i++) {
        struct stat st;
        moved[i] = gone[i] = 0;
        if (stat(paths[i], &st) != 0) { gone[i] = 1; n_gone++; }
        else if ((long)st.st_mtime > r.ts) { moved[i] = 1; n_moved++; }
    }
    int stale = head_moved || n_moved > 0 || n_gone > 0;

    if (json) {
        char en[RS_NOTE_MAX * 2 + 16];
        jash_json_esc(r.note, en, sizeof en);
        jash_writef(w, ctx,
            "{\"note\":\"%s\",\"written\":%ld,\"age_s\":%ld,"
            "\"head_then\":\"%s\",\"head_now\":\"%s\",\"head_moved\":%s,\"stale\":%s,\"files\":[",
            en, r.ts, age, r.head, head_now,
            head_moved ? "true" : "false", stale ? "true" : "false");
        for (int i = 0; i < np; i++) {
            char ep[RS_PATH_LEN * 2 + 8];
            jash_json_esc(paths[i], ep, sizeof ep);
            jash_writef(w, ctx, "%s{\"path\":\"%s\",\"missing\":%s,\"changed_since_note\":%s}",
                        i ? "," : "", ep,
                        gone[i] ? "true" : "false", moved[i] ? "true" : "false");
        }
        /* JINV-051: dizer o que sustenta o veredito — e o que NAO sustenta. */
        jash_writef(w, ctx,
            "],\"checks\":[\"git HEAD\",\"mtime dos %d caminho(s) citado(s) no bilhete\"],"
            "\"not_verified\":[\"arquivos que o bilhete nao cita\","
            "\"mudancas sem commit fora dos caminhos citados\","
            "\"se o conteudo do bilhete era verdade quando foi escrito\"]}\n", np);
        return stale ? 1 : 0;
    }

    char idade[48];
    if (age < 3600)       snprintf(idade, sizeof idade, "%ldmin atras", age/60);
    else if (age < 86400) snprintf(idade, sizeof idade, "%ldh atras", age/3600);
    else                  snprintf(idade, sizeof idade, "%ldd atras", age/86400);

    jash_writef(w, ctx, "\033[1mbilhete\033[0m \033[90m(%s, HEAD %s)\033[0m\r\n", idade, r.head);
    jash_writef(w, ctx, "  %s\r\n", r.note);

    if (!stale) {
        jash_writef(w, ctx,
            "\r\n\033[90mHEAD nao mudou e nenhum dos %d arquivo(s) citado(s) foi tocado "
            "desde o bilhete.\033[0m\r\n", np);
    } else {
        jash_writes(w, ctx, "\r\n\033[1;33mo mundo andou desde este bilhete:\033[0m\r\n");
        if (head_moved)
            jash_writef(w, ctx, "  \033[33m•\033[0m git HEAD %s → %s\r\n", r.head, head_now);
        for (int i = 0; i < np; i++) {
            if (gone[i])
                jash_writef(w, ctx, "  \033[33m•\033[0m %s \033[90mnao existe mais\033[0m\r\n", paths[i]);
            else if (moved[i])
                jash_writef(w, ctx, "  \033[33m•\033[0m %s \033[90mfoi modificado DEPOIS do bilhete\033[0m\r\n", paths[i]);
        }
        jash_writes(w, ctx,
            "  \033[90m→ o que o bilhete diz sobre esses pontos pode estar velho\033[0m\r\n");
    }

    /* Nunca "ainda vale": o que foi conferido, e o que nao foi. */
    jash_writef(w, ctx,
        "\033[90mconferido: git HEAD, mtime de %d caminho(s) citado(s).\r\n"
        "nao conferido: arquivos nao citados, mudancas sem commit fora deles, "
        "e se o bilhete era verdade quando escrito.\033[0m\r\n", np);

    return stale ? 1 : 0;
}

#include "resume_native.inc"
