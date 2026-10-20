// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/why — o diagnóstico mínimo, não o log.
//
// JINV-056 (Onda A3 do PLANO-10-ONDAS-AGENTE).
//
// ── Por que isto existe ────────────────────────────────────────────────────
//
// Um comando falha e o agente recebe duzentas linhas. Ele então gasta contexto
// — o recurso mais escasso que tem — relendo saída para extrair três fatos que
// o shell JÁ SABIA antes de imprimir a primeira linha:
//
//   o que rodou           `jash_status_t.cmd`      (state.h:128)
//   como falhou           `.code` e `.msg`
//   o que mudou desde     `~/.jash/drift.tsv`      (ADR-0043)
//   o que você editou     o timeline da sessão
//
// Nesta sessão o agente rodou a suíte completa doze vezes — ~82 minutos — para
// descobrir o efeito de edições de três linhas. Não por precisar: por não ter
// como perguntar mais barato.
//
// ── A tese ─────────────────────────────────────────────────────────────────
//
// As peças existem e nunca conversam. O `drift` guarda a impressão digital do
// ambiente por comando e só a usa inline, na hora da falha; o `timeline` sabe
// o que mudou e só responde quando perguntado sobre si mesmo; o `last_status`
// carrega o comando e some no próximo prompt.
//
// `why` é a junção. Não é uma feature nova — é a recusa a fazer o agente
// reconstruir, relendo bytes, o que o shell já tem estruturado.
//
// ── O que NÃO faz ──────────────────────────────────────────────────────────
//
// Não adivinha. Cada linha da saída é um fato que o shell registrou, com sua
// origem. Quando não há dado, diz que não há — em vez de inventar hipótese
// plausível, que é a forma de mentira que esta auditoria inteira combateu.
//
// F E R M I ∞ H A R T

#include <builtins/builtin.h>
#include <core/state.h>
#include <core/timeline.h>
#include <theme/io.h>
#include <core/agent_json.h>   /* jash_json_esc */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Procura no ledger de drift a última vez que ESTE comando passou.
 * Só leitura — nunca escreve, para que `why` não altere o que observa.
 * Devolve 1 se achou; preenche `ts_ok`. */
static int why_drift_last_ok(const char* cmd, long* ts_ok) {
    if (!cmd || !*cmd) return 0;
    const char* home = getenv("HOME");
    if (!home || !*home) return 0;
    char path[600];
    snprintf(path, sizeof path, "%s/.jash/drift.tsv", home);
    FILE* f = fopen(path, "r");
    if (!f) return 0;

    /* A chave do ledger é um hash de (cmd, cwd) que este builtin não recomputa
     * — recomputá-lo aqui duplicaria a função de hash, e duas implementações
     * da mesma coisa foi o pior achado desta auditoria (JINV-015/017). Em vez
     * disso casamos pelo prefixo do comando gravado na linha, que basta para
     * responder "quando isto passou pela última vez". */
    char line[2048];
    long melhor = 0;
    size_t cl = strlen(cmd);
    while (fgets(line, sizeof line, f)) {
        long ts = 0;
        if (sscanf(line, "%*16s\t%ld", &ts) != 1) continue;
        if (strstr(line, cmd) || (cl > 8 && strncmp(line + 17, cmd, 8) == 0)) {
            if (ts > melhor) melhor = ts;
        }
    }
    fclose(f);
    if (melhor > 0) { *ts_ok = melhor; return 1; }
    return 0;
}

static void why_idade(long ts, char* out, size_t cap) {
    long d = (long)time(NULL) - ts;
    if (d < 60)         snprintf(out, cap, "há %lds", d);
    else if (d < 3600)  snprintf(out, cap, "há %ldmin", d / 60);
    else if (d < 86400) snprintf(out, cap, "há %ldh", d / 3600);
    else                snprintf(out, cap, "há %ldd", d / 86400);
}

int jash_b_why(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    /* JINV-046 */
    { static const char* const ok_flags[] = { "--json", 0 };
      if (jash_reject_unknown_flag("why", args, ok_flags, w, ctx)) return 2; }

    int json = 0;
    { const char* a = args ? args : "";
      while (*a == ' ' || *a == '\t') a++;
      if (strncmp(a, "--json", 6) == 0 && (a[6] == '\0' || a[6] == ' ')) json = 1; }

    if (!s) return 1;
    const char* cmd = s->last_status.cmd;
    int rc = s->last_status.code;
    const char* msg = s->last_status.msg;

    if (!cmd || !*cmd) {
        if (json) jash_writes(w, ctx, "{\"known\":false,\"reason\":\"no last command recorded\"}\n");
        else jash_writes(w, ctx,
            "why: nenhum comando registrado ainda nesta sessão\r\n"
            "     (o `why` responde sobre o ÚLTIMO comando — rode algo primeiro)\r\n");
        return 0;
    }

    /* Quando o último comando passou, dizer isso é a resposta honesta —
     * inventar um problema seria pior que não responder. */
    if (rc == 0) {
        if (json)
            jash_writef(w, ctx, "{\"known\":true,\"rc\":0,\"cmd\":\"%s\",\"verdict\":\"ok\"}\n", cmd);
        else
            jash_writef(w, ctx,
                "why: o último comando \033[1m%s\033[0m passou (rc=0) — nada a "
                "diagnosticar\r\n", cmd);
        return 0;
    }

    long ts_ok = 0;
    int tinha = why_drift_last_ok(cmd, &ts_ok);
    char idade[32] = "";
    if (tinha) why_idade(ts_ok, idade, sizeof idade);

    int mut = jash_timeline_count();

    if (json) {
        char ec[600], em[400];
        jash_json_esc(cmd, ec, sizeof ec);
        jash_json_esc(msg ? msg : "", em, sizeof em);
        jash_writef(w, ctx,
            "{\"known\":true,\"rc\":%d,\"cmd\":\"%s\",\"msg\":\"%s\","
            "\"last_ok_ts\":%ld,\"mutations_this_session\":%d}\n",
            rc, ec, em, ts_ok, mut);
        return 0;
    }

    jash_writef(w, ctx, "  \033[1mfalhou:\033[0m  %s \033[90m(rc=%d)\033[0m\r\n", cmd, rc);
    if (msg && *msg)
        jash_writef(w, ctx, "  \033[1merro:\033[0m    %s\r\n", msg);
    if (tinha)
        jash_writef(w, ctx,
            "  \033[1mantes:\033[0m   este comando passou %s\r\n", idade);
    else
        jash_writes(w, ctx,
            "  \033[1mantes:\033[0m   \033[90msem registro de que já tenha passado — "
            "pode ser a primeira vez\033[0m\r\n");
    if (mut > 0)
        jash_writef(w, ctx,
            "  \033[1mmudou:\033[0m   %d mutação(ões) nesta sessão — "
            "`timeline` lista, `rewind` desfaz\r\n", mut);
    else
        jash_writes(w, ctx,
            "  \033[1mmudou:\033[0m   \033[90mnenhuma mutação registrada nesta sessão — "
            "se algo mudou, veio de fora\033[0m\r\n");

    /* Sem hipótese inventada: o shell diz o que sabe e onde procurar o resto. */
    if (tinha && mut > 0)
        jash_writes(w, ctx,
            "  \033[90m→ passou antes e houve mutações desde então: comece pelo "
            "`timeline`\033[0m\r\n");
    else if (!tinha)
        jash_writes(w, ctx,
            "  \033[90m→ sem histórico de sucesso: provavelmente não é regressão, "
            "é a primeira execução\033[0m\r\n");

    return 0;
}

#include "why_native.inc"
