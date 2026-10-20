// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Native records are projected directly to text, JSON and JTV2. Checked
 * publication receipts describe each effect; multiple files are not claimed
 * to be an atomic transaction. */
#define _GNU_SOURCE
#include "builtin.h"
#include "discovery_output.h"
#include "mesh_snapshot.h"
#include <core/adopt.h>
#include <core/argv.h>
#include <core/claims.h>
#include <core/data_json.h>
#include <core/mesh_store.h>
#include <core/output_budget.h>
#include <core/query_cancel.h>
#include <core/structured_output.h>
#include <module/sigil.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "mesh_records.h"

typedef enum { M_REGISTER, M_HEARTBEAT, M_WORKERS, M_INIT, M_ADD, M_NEXT, M_DONE,
    M_FAIL, M_POST, M_BOARD, M_REP, M_SWEEP, M_CLAIM, M_CLAIMS, M_UNCLAIM, M_INVALID } mesh_operation_t;
static const char* const mesh_operations[] = {"register", "heartbeat", "workers", "init", "add", "next", "done",
    "fail", "post", "board", "rep", "sweep", "claim", "claims", "unclaim"};
enum { M_ID, M_PROVIDER, M_MODEL, M_STATUS, M_HEADROOM, M_TASK, M_TAG, M_NEEDS, M_ARTIFACT,
    M_PROVE, M_NOTE, M_TTL, M_OFFSET, M_LIMIT, M_OPTIONS };
static const char* const mesh_option_names[] = {"--id", "--provider", "--model", "--status", "--headroom",
    "--task", "--tag", "--needs", "--artifact", "--prove", "--note", "--ttl", "--offset", "--limit"};
typedef struct {
    mesh_operation_t operation; const char* operand; const char* value[M_OPTIONS];
    uint64_t ttl, offset, limit; int force, messages, paged, mode;
    char self[128]; long pid;
} mesh_options_t;
typedef struct {
    jash_state_t* state; jash_writer_t writer; void* context;
    mesh_options_t options; jash_mesh_store_t store; jash_discovery_rows_t rows;
    jash_structured_output_t output; jdv_value_t result;
    int admitted, json_started, store_opened, predicate;
    const char* reason; size_t emitted; long now;
    char store_path[PATH_MAX + 16], claim_path[JASH_CLAIM_PATH_MAX];
} mesh_call_t;

static int mesh_is_query(mesh_operation_t op) { return op == M_WORKERS || op == M_BOARD || op == M_REP || op == M_CLAIMS; }
static int mesh_option_index(const char* argument, size_t length) {
    for (int i = 0; i < M_OPTIONS; i++) if (strlen(mesh_option_names[i]) == length && !memcmp(argument, mesh_option_names[i], length)) return i;
    return -1;
}
static const char* mesh_parse(int argc, char** argv, mesh_options_t* o) {
    memset(o, 0, sizeof *o); o->operation = M_INVALID; o->ttl = JASH_CLAIM_DEFAULT_TTL; o->limit = JASH_DISCOVERY_ROWS_MAX;
    int formats = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--")) break;
        int mode = jash_discovery_format_flag(argv[i]);
        if (mode) { if (!formats) o->mode = mode; formats++; }
        else if (mesh_option_index(argv[i], strlen(argv[i])) >= 0 && i + 1 < argc) i++;
    }
    if (formats > 1) return "conflicting_formats";
    if (argc > 128) return "argument_limit";
    size_t bytes = 0;
    for (int i = 0; i < argc; i++) { size_t n = strlen(argv[i]); if (n > 16384 - bytes) return "argument_limit"; bytes += n; }
    int literal = 0;
    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i];
        if (!literal && !strcmp(arg, "--")) { literal = 1; continue; }
        if (!literal && jash_discovery_format_flag(arg)) continue;
        if (o->operation == M_INVALID) {
            const char* name = !strcmp(arg, "who") ? "workers" : arg;
            for (int k = 0; k < M_INVALID; k++) if (!strcmp(name, mesh_operations[k])) { o->operation = (mesh_operation_t)k; break; }
            if (o->operation == M_INVALID) return "invalid_operation";
        } else if (!literal && arg[0] == '-') {
            if (!strcmp(arg, "--force")) { if (o->force || (o->operation != M_DONE && o->operation != M_FAIL)) return "invalid_option"; o->force = 1; continue; }
            if (!strcmp(arg, "--messages")) { if (o->messages || o->operation != M_BOARD) return "invalid_option"; o->messages = 1; continue; }
            const char* equal = strchr(arg, '='); size_t n = equal ? (size_t)(equal - arg) : strlen(arg);
            int index = mesh_option_index(arg, n);
            if (index < 0 || o->value[index] || (!equal && i + 1 == argc)) return "invalid_option";
            o->value[index] = equal ? equal + 1 : argv[++i];
        } else { if (o->operand) return "unexpected_operand"; o->operand = arg; }
    }
    if (o->operation == M_INVALID) return "missing_operation";
    unsigned allowed = 0;
#define M_OPT(n) (1u << (n))
    switch (o->operation) {
        case M_REGISTER: allowed = M_OPT(M_ID)|M_OPT(M_PROVIDER)|M_OPT(M_MODEL); break;
        case M_HEARTBEAT: allowed = M_OPT(M_ID)|M_OPT(M_PROVIDER)|M_OPT(M_MODEL)|M_OPT(M_STATUS)|M_OPT(M_HEADROOM)|M_OPT(M_TASK); break;
        case M_ADD: allowed = M_OPT(M_TAG)|M_OPT(M_NEEDS); break;
        case M_NEXT: case M_POST: case M_UNCLAIM: allowed = M_OPT(M_ID); break;
        case M_DONE: allowed = M_OPT(M_ID)|M_OPT(M_ARTIFACT)|M_OPT(M_PROVE); break;
        case M_FAIL: allowed = M_OPT(M_ID)|M_OPT(M_ARTIFACT); break;
        case M_CLAIM: allowed = M_OPT(M_ID)|M_OPT(M_TTL)|M_OPT(M_NOTE); break;
        case M_WORKERS: case M_BOARD: case M_REP: case M_CLAIMS: allowed = M_OPT(M_OFFSET)|M_OPT(M_LIMIT); break;
        default: break;
    }
    static const size_t capacities[] = {128,128,128,32,32,256,32,256,512,1024,256,32,32,32};
    for (int i = 0; i < M_OPTIONS; i++) if (o->value[i] && (!(allowed & M_OPT(i)) || strlen(o->value[i]) >= capacities[i])) return "invalid_option_or_field_limit";
#undef M_OPT
    int operand = o->operation == M_ADD || o->operation == M_DONE || o->operation == M_FAIL || o->operation == M_POST || o->operation == M_CLAIM || o->operation == M_UNCLAIM;
    if ((operand && !o->operand) || (!operand && o->operand && o->operation != M_INIT)) return "invalid_operands";
    if (o->operation == M_ADD && strlen(o->operand) >= 512) return "field_limit";
    if ((o->operation == M_POST || o->operation == M_INIT) && o->operand && strlen(o->operand) > 4096) return "field_limit";
    if (!mesh_deps_valid(o->value[M_NEEDS])) return "invalid_dependencies";
    if (o->value[M_TTL] && (jash_mesh_number(o->value[M_TTL], LONG_MAX, &o->ttl) || !o->ttl)) return "invalid_ttl";
    if (o->value[M_OFFSET] && jash_mesh_number(o->value[M_OFFSET], SIZE_MAX, &o->offset)) return "invalid_page";
    if (o->value[M_LIMIT] && (jash_mesh_number(o->value[M_LIMIT], 256, &o->limit) || !o->limit)) return "invalid_page";
    o->paged = o->value[M_OFFSET] || o->value[M_LIMIT];
    if (o->operation == M_DONE || o->operation == M_FAIL) { uint64_t id; if (jash_mesh_number(o->operand, INT_MAX, &id) || !id) return "invalid_task_id"; }
    if (!mesh_is_query(o->operation)) {
        const char* id = o->value[M_ID] ? o->value[M_ID] : getenv("JASH_AGENT_ID");
        if (id && *id) { if (!mesh_id_valid(id)) return "invalid_agent_id"; memcpy(o->self, id, strlen(id) + 1); }
        else { struct timespec now; static unsigned long seq; if (clock_gettime(CLOCK_REALTIME, &now)) return "clock_unavailable"; snprintf(o->self, sizeof o->self, "w%ld-%lx-%lu", (long)getpid(), (unsigned long)now.tv_nsec, ++seq); }
        const char* pid = getenv("JASH_AGENT_PID");
        if (pid && *pid) { uint64_t number; if (jash_mesh_number(pid, INT_MAX, &number) || !number) return "invalid_agent_pid"; o->pid = (long)number; }
    }
    return NULL;
}

static jdv_value_t mesh_text(mesh_call_t* c, const char* text) { return jash_discovery_string(&c->rows, text); }
static jdv_value_t mesh_symbol(mesh_call_t* c, const char* text) {
    jdv_value_t value = jdv_value_nothing();
    if (text && jdv_value_symbol(&c->rows.arena, text, strlen(text), &value)) c->rows.error = 1;
    return value;
}
static jdv_record_input_t mesh_field(const char* name, jdv_value_t value) { return (jdv_record_input_t){{(const uint8_t*)name, strlen(name)}, value}; }
static void mesh_result(mesh_call_t* c, const jdv_record_input_t* fields, size_t count) { if (jdv_value_record(&c->rows.arena, fields, count, &c->result)) c->rows.error = 1; }
static int mesh_admit(mesh_call_t* c) {
    if (c->rows.error) return 125;
    int mode = c->options.mode, rc = 0;
    if (mode == JASH_DISCOVERY_DATA) rc = jash_discovery_effects_begin(&c->rows, c->writer, c->context, "mesh", mode, "jash.mesh.action.v1");
    else if (mode == JASH_DISCOVERY_JSON) {
        if (jash_structured_begin(&c->output, c->writer, c->context)) return 125;
        jash_jsonw_raw(&c->output.json, "\n"); if (jash_jsonw_finish(&c->output.json)) return 125;
    }
    if (!rc) c->admitted = 1;
    return rc;
}
static void mesh_json_start(mesh_call_t* c) {
    if (!c->json_started) { jash_jsonw_raw(&c->output.json, "{\"schema\":\"jash.mesh.action.v1\",\"receipts\":["); c->json_started = 1; }
}
static int mesh_receipt(mesh_call_t* c, const char* action, const char* target, const jash_mesh_io_t* io, uint64_t count, const char* code) {
    if (!code && io->error) code = "store_io_failed";
    int proof = !strcmp(action,"execute_proof"), attest = !strcmp(action,"request_attestation");
    int callback = proof || attest;
    jdv_record_input_t fields[] = {
        mesh_field("row_kind", mesh_symbol(c,"receipt")), mesh_field("operation",mesh_symbol(c,mesh_operations[c->options.operation])),
        mesh_field("action",mesh_symbol(c,action)), mesh_field("target",jash_discovery_path(&c->rows,target)),
        mesh_field("applied",jdv_value_bool(io->applied)), mesh_field("durable",jdv_value_bool(io->durable)),
        mesh_field("verified",jdv_value_bool(io->verified)), mesh_field("bytes_written",jdv_value_filesize(io->bytes_written)),
        mesh_field("count",jdv_value_u64(count)), mesh_field("errno",io->error?jdv_value_u64((uint64_t)io->error):jdv_value_nothing()),
        mesh_field("error",mesh_symbol(c,code)),
        mesh_field("scope",mesh_symbol(c,proof?"expect-callback":attest?"sigil-attestation-callback":"home-mesh-store")),
        mesh_field("transaction",mesh_symbol(c,callback?"callback-invocation":"per-file-publication")),
        mesh_field("target_base",jash_discovery_path(&c->rows,!callback&&*c->store_path?c->store_path:NULL)),
        mesh_field("callback_invoked",jdv_value_bool(callback)),
        mesh_field("persistence_verified",callback?jdv_value_nothing():jdv_value_bool(io->durable))
    };
    if (jash_discovery_add(&c->rows, fields, sizeof fields / sizeof *fields)) return 125;
    if (c->options.mode == JASH_DISCOVERY_DATA) return jash_discovery_effects_flush(&c->rows);
    if (c->options.mode == JASH_DISCOVERY_JSON) {
        mesh_json_start(c); if (c->emitted) jash_jsonw_raw(&c->output.json,",");
        if (jdv_json_write(&c->output.json,&c->rows.items[c->rows.count-1],&JDV_DEFAULT_LIMITS) || jash_jsonw_finish(&c->output.json)) return 125;
        c->emitted++;
    }
    return 0;
}
static int mesh_fail(mesh_call_t* c, const char* reason, int status) {
    c->reason = reason;
    if (!c->options.mode) jash_writef(jash_stderr_writer,NULL,"mesh: %s\n",reason);
    if (c->admitted) { jash_mesh_io_t none={0}; if(mesh_receipt(c,"refused",NULL,&none,0,reason)) return 125; }
    return status;
}
static int mesh_io_receipt(mesh_call_t* c,const char* action,const char* target,const jash_mesh_io_t* io,uint64_t count) {
    if(mesh_receipt(c,action,target,io,count,NULL)) return 125;
    if(io->error) { c->reason="store_io_failed"; return 1; }
    return 0;
}
static int mesh_cancel_callback(void* context) { return jash_query_cancel_poll(context); }
static int mesh_store_begin(mesh_call_t* c,int create) {
    int rc=jash_mesh_store_open_poll(&c->store,create,create?NULL:mesh_cancel_callback,c->state), error=rc<0?errno:0; c->store_opened=1;
    if(create&&c->store.created) {
        jash_mesh_io_t io={.applied=1,.durable=c->store.durable,.verified=!error,.error=error};
        if(mesh_receipt(c,"initialize_store",".",&io,c->store.created,NULL)) return 125;
    }
    if(error==ECANCELED) return mesh_fail(c,"request_cancelled",jash_query_cancel_poll(c->state));
    return error?mesh_fail(c,"store_open_failed",1):0;
}
static int mesh_finish(mesh_call_t* c,int status) {
    if(c->store_opened&&jash_mesh_store_close(&c->store)) {
        jash_mesh_io_t io={.error=errno};
        if(c->admitted&&mesh_receipt(c,"close_store",".",&io,0,"store_close_failed")) status=125;
        else if(!status) { status=1; c->reason="store_close_failed"; }
    }
    c->store_opened=0;
    if(c->rows.error) status=125;
    if(status==125) {
        if(c->rows.effects) return jash_discovery_effects_end(&c->rows,status);
        return c->output.restore_sigpipe?jash_structured_finish(&c->output,status):status;
    }
    if(!c->admitted) return jash_discovery_error(c->writer,c->context,"mesh",c->options.mode,c->reason?c->reason:"operation_failed",status?status:1);
    jdv_record_input_t fields[40]; size_t count=0;
    if(c->result.kind==JDV_RECORD) for(size_t i=0;i<c->result.as.record.len;i++) { const jdv_record_field_t* f=&c->result.as.record.fields[i]; fields[count++]=(jdv_record_input_t){f->key,f->value}; }
    fields[count++]=mesh_field("row_kind",mesh_symbol(c,"result"));
    fields[count++]=mesh_field("operation",mesh_symbol(c,mesh_operations[c->options.operation]));
    fields[count++]=mesh_field("complete",jdv_value_bool(!status||c->predicate));
    fields[count++]=mesh_field("status",jdv_value_i64(status)); fields[count++]=mesh_field("error",mesh_symbol(c,c->reason));
    if(jash_discovery_add(&c->rows,fields,count)) {
        if(c->rows.effects) return jash_discovery_effects_end(&c->rows,125);
        return c->output.restore_sigpipe?jash_structured_finish(&c->output,125):125;
    }
    if(c->options.mode==JASH_DISCOVERY_DATA) return jash_discovery_effects_end_outcome(&c->rows,status,!status||c->predicate?JDV_STREAM_COMPLETE:JDV_STREAM_FAILED,0,c->reason);
    if(c->options.mode==JASH_DISCOVERY_JSON) {
        jash_json_writer_t* out=&c->output.json;
        if(c->predicate&&!c->json_started) jash_jsonw_raw(out,"null\n");
        else {
            mesh_json_start(c); jash_jsonw_raw(out,"]");
            for(size_t i=0;i<count;i++) {
                if(fields[i].key.len==8&&!memcmp(fields[i].key.data,"row_kind",8)) continue;
                jash_jsonw_raw(out,","); jash_jsonw_string_n(out,(const char*)fields[i].key.data,fields[i].key.len); jash_jsonw_raw(out,":");
                if(jdv_json_write(out,&fields[i].value,&JDV_DEFAULT_LIMITS)) out->error=EINVAL;
            }
            jash_jsonw_raw(out,"}\n");
        }
        return jash_structured_finish(&c->output,status);
    }
    return status;
}

#include "mesh_queries.inc"
#include "mesh_actions.inc"

/* Validate argument-dependent storage paths before any store initialization.
 * Effect classification itself stays pure and never reads the filesystem. */
static int mesh_preflight(mesh_call_t* c) {
    const char* home=getenv("HOME");
    if(home&&home[0]=='/'&&strlen(home)<PATH_MAX)
        (void)snprintf(c->store_path,sizeof c->store_path,"%s/.jash/mesh",home);
    if(c->options.operation==M_CLAIM||c->options.operation==M_UNCLAIM) {
        if(!jash_claims_norm(c->options.operand,c->claim_path,sizeof c->claim_path))
            return jash_discovery_error(c->writer,c->context,"mesh",c->options.mode,"path_unresolvable",2);
    }
    if((c->options.operation==M_DONE||c->options.operation==M_FAIL)&&c->options.value[M_ARTIFACT]) {
        jash_mesh_store_t store; int opened=jash_mesh_store_open(&store,0),error=opened<0?errno:0;
        uint64_t id=0; (void)jash_mesh_number(c->options.operand,INT_MAX,&id);
        mesh_task_t task; int found=error?0:mesh_task_read(&store,(int)id,&task);
        if(found<0) error=errno;
        if(jash_mesh_store_close(&store)&&!error) error=errno;
        if(error) return jash_discovery_error(c->writer,c->context,"mesh",c->options.mode,"task_source_invalid",1);
        if(!found) return jash_discovery_error(c->writer,c->context,"mesh",c->options.mode,"task_not_found",1);
        if(strlen(task.desc)+strlen(c->options.value[M_ARTIFACT])+7>=sizeof task.desc)
            return jash_discovery_error(c->writer,c->context,"mesh",c->options.mode,"artifact_description_limit",2);
    }
    return 0;
}

int jash_mesh_snapshot_counts(jash_state_t* state,jash_mesh_snapshot_t* snapshot) {
    if(!snapshot) { errno=EINVAL; return 1; }
    *snapshot=(jash_mesh_snapshot_t){0};
    time_t now=time(NULL);
    if(now<0) { snapshot->status=1; snapshot->error_number=errno?errno:EIO; snapshot->reason="clock_unavailable"; return 1; }
    snapshot->observed_at=(int64_t)now;
    jash_mesh_store_t store; int opened=jash_mesh_store_open_poll(&store,0,mesh_cancel_callback,state),error=opened<0?errno:0;
    snapshot->store_present=store.root>=0;
    mesh_worker_t* workers=calloc(MESH_WORKERS_MAX,sizeof *workers);
    mesh_task_t* tasks=calloc(MESH_TASKS_MAX,sizeof *tasks);
    jash_claim_t* claims=calloc(JASH_CLAIM_MAX,sizeof *claims);
    size_t worker_count=0,task_count=0; int claim_count=0;
    if(!error&&(!workers||!tasks||!claims)) error=ENOMEM;
    if(!error&&(mesh_collect(&store,0,workers,&worker_count,state)||mesh_collect(&store,1,tasks,&task_count,state))) error=errno;
    if(!error&&(claim_count=jash_claims_read_at(store.root,claims,JASH_CLAIM_MAX))<0) error=errno;
    if(!error) {
        snapshot->workers=worker_count; snapshot->tasks=task_count; snapshot->claims=(uint64_t)claim_count;
        for(size_t i=0;i<worker_count;i++) {
            int alive=mesh_pid_alive(workers[i].pid);
            if(alive>0) snapshot->workers_live++; else if(!alive) snapshot->workers_dead++; else snapshot->workers_unknown++;
        }
        for(size_t i=0;i<task_count;i++) {
            if(!strcmp(tasks[i].state,"ready")) snapshot->tasks_ready++;
            else if(!strcmp(tasks[i].state,"claimed")) snapshot->tasks_claimed++;
            else if(!strcmp(tasks[i].state,"done")) snapshot->tasks_done++;
            else snapshot->tasks_failed++;
        }
        for(int i=0;i<claim_count;i++) { if(jash_claims_live(&claims[i],(long)now)) snapshot->claims_live++; else snapshot->claims_expired++; }
    }
    free(workers); free(tasks); free(claims);
    if(jash_mesh_store_close(&store)&&!error) error=errno;
    int cancelled=jash_query_cancel_poll(state);
    if(cancelled) error=ECANCELED;
    snapshot->error_number=error;
    if(error) {
        snapshot->status=cancelled?cancelled:error==E2BIG?75:1;
        snapshot->reason=cancelled?"request_cancelled":error==E2BIG?"source_limit":"store_source_invalid";
        return snapshot->status;
    }
    snapshot->complete=1; return 0;
}

int jash_b_mesh_argv(jash_state_t*,jash_writer_t,void*,int,char**);
int jash_b_mesh_argv(jash_state_t* state,jash_writer_t writer,void* context,int argc,char** argv) {
    if(!argc) {
        jash_writes(writer,context,"mesh — multi-model swarm substrate (ADR-0024)\n"
            "  register --provider P --model M [--id ID]; heartbeat [--id ID] [--status S]\n"
            "  workers|who; init [goal]; add task [--tag T] [--needs ID,ID]; next [--id ID]\n"
            "  done ID [--artifact PATH] [--prove ASSERTION] [--force]; fail ID [--force]\n"
            "  post message; board [--messages]; rep; sweep; claim PATH [--ttl SEC] [--note TEXT]\n"
            "  claims; unclaim PATH. All operations: --json, --data or --data-v2 before --.\n"
            "  Queries: --offset N --limit N (1..256). Data is JTV2; timestamps and TTL use seconds.\n");
        return 0;
    }
    mesh_call_t c={.state=state,.writer=writer,.context=context};
    const char* invalid=mesh_parse(argc,argv,&c.options);
    if(invalid) return jash_discovery_error(writer,context,"mesh",c.options.mode,invalid,2);
    int preflight=mesh_preflight(&c); if(preflight) return preflight;
    c.now=(long)time(NULL);
    if(c.now<0) return jash_discovery_error(writer,context,"mesh",c.options.mode,"clock_unavailable",1);
    if(jash_discovery_rows_init(&c.rows)) return jash_discovery_error(writer,context,"mesh",c.options.mode,"allocation_failed",125);
    int rc;
    if(mesh_is_query(c.options.operation)) rc=mesh_query(&c);
    else { rc=mesh_admit(&c); if(!rc) rc=mesh_store_begin(&c,1); if(!rc) rc=mesh_action(&c); rc=mesh_finish(&c,rc); }
    if(c.store_opened) (void)jash_mesh_store_close(&c.store);
    if(c.output.restore_sigpipe) (void)jash_structured_finish(&c.output,125);
    jash_discovery_rows_free(&c.rows); return rc;
}
int jash_b_mesh(jash_state_t* state,jash_writer_t writer,void* context,const char* text) {
    jash_argv_t args={0}; if(jash_argv_build(text?text:"",&args)<0) return 2;
    int rc=jash_b_mesh_argv(state,writer,context,args.argc,args.argv); jash_argv_free(&args); return rc;
}

/* Capability classification shares the same literal option grammar. */
const char* jash_mesh_effect_argv(int argc, char* const* argv) {
    mesh_options_t options;
    if (mesh_parse(argc, (char**)argv, &options) || !options.mode) return "exec";
    if (mesh_is_query(options.operation)) return "read";
    if (options.operation == M_DONE && options.value[M_PROVE]) return "exec";
    return "mutate";
}
