// SPDX-License-Identifier: LicenseRef-SPL-1.0
static int mesh_worker_row(mesh_call_t* c,const mesh_worker_t* worker) {
    int alive=mesh_pid_alive(worker->pid);
    uint64_t age=worker->timestamp<c->now?(uint64_t)c->now-(uint64_t)worker->timestamp:0;
    jdv_record_input_t fields[]={
        mesh_field("agent_id",mesh_text(c,worker->id)),mesh_field("provider",mesh_text(c,worker->provider)),
        mesh_field("model",mesh_text(c,worker->model)),mesh_field("status",mesh_text(c,worker->status)),
        mesh_field("headroom",mesh_text(c,worker->headroom)),mesh_field("task",mesh_text(c,worker->task)),
        mesh_field("age_s",jdv_value_u64(age)),mesh_field("pid",jdv_value_i64(worker->pid)),
        mesh_field("alive",alive<0?jdv_value_nothing():jdv_value_bool(alive)),
        mesh_field("last_seen",jdv_value_i64(worker->timestamp)),mesh_field("timestamp_unit",mesh_symbol(c,"unix-seconds")),
        mesh_field("liveness_source",mesh_symbol(c,"explicit-harness-pid-kill-zero"))
    };
    return jash_discovery_add(&c->rows,fields,sizeof fields/sizeof *fields);
}
static int mesh_task_row(mesh_call_t* c,const mesh_task_t* task) {
    jdv_record_input_t fields[]={
        mesh_field("id",jdv_value_u64((uint64_t)task->id)),mesh_field("state",mesh_symbol(c,task->state)),
        mesh_field("tag",mesh_text(c,task->tag)),mesh_field("owner",mesh_text(c,task->owner)),
        mesh_field("deps",mesh_text(c,task->deps)),mesh_field("desc",mesh_text(c,task->desc)),
        mesh_field("claim_ts",jdv_value_i64(task->claim_ts)),mesh_field("timestamp_unit",mesh_symbol(c,"unix-seconds")),
        mesh_field("row_kind",mesh_symbol(c,"task"))
    };
    return jash_discovery_add(&c->rows,fields,sizeof fields/sizeof *fields);
}
static int mesh_claim_row(mesh_call_t* c,const jash_claim_t* claim) {
    jdv_record_input_t fields[]={
        mesh_field("path",jash_discovery_path(&c->rows,claim->path)),mesh_field("owner",mesh_text(c,claim->owner)),
        mesh_field("claim_ts",jdv_value_i64(claim->claim_ts)),mesh_field("ttl",jdv_value_u64((uint64_t)claim->ttl)),
        mesh_field("note",mesh_text(c,claim->note)),mesh_field("timestamp_unit",mesh_symbol(c,"unix-seconds")),
        mesh_field("ttl_unit",mesh_symbol(c,"seconds"))
    };
    return jash_discovery_add(&c->rows,fields,sizeof fields/sizeof *fields);
}
static int mesh_rep_row(mesh_call_t* c,const mesh_rep_t* rep) {
    jdv_record_input_t fields[]={
        mesh_field("model",mesh_text(c,rep->model)),mesh_field("tag",mesh_text(c,rep->tag)),
        mesh_field("done",jdv_value_u64(rep->done)),mesh_field("fail",jdv_value_u64(rep->fail)),
        mesh_field("score",jdv_value_i64((int64_t)rep->done-(int64_t)rep->fail)),
        mesh_field("source",mesh_symbol(c,"local-chain-events")),mesh_field("verification",mesh_symbol(c,"not-verified"))
    };
    return jash_discovery_add(&c->rows,fields,sizeof fields/sizeof *fields);
}

/* Validate all records before publishing a query or appending a message. */
static int mesh_blackboard_parse(mesh_call_t* c,char* bytes,size_t length,int emit) {
    if(memchr(bytes,0,length)||(length&&bytes[length-1]!='\n')) { errno=EINVAL; return -1; }
    size_t count=0; char* cursor=bytes;
    while(*cursor) {
        if(mesh_is_query(c->options.operation)&&jash_query_cancel_poll(c->state)) { errno=ECANCELED; return -1; }
        if(count++==MESH_POSTS_MAX) { errno=E2BIG; return -1; }
        char* end=strchr(cursor,'\n'); if(!end||end==cursor) { errno=EINVAL; return -1; } *end=0;
        char* fields[4]; int n=jash_mesh_fields(cursor,fields,4);
        int encoded=n==4&&!strcmp(fields[3],"@jash-mesh-v2");
        uint64_t timestamp; char owner[160],message[4097];
        if((n!=3&&!encoded)||jash_mesh_number(fields[0],LONG_MAX,&timestamp)||
            jash_mesh_unescape(fields[1],owner,sizeof owner,encoded)||jash_mesh_unescape(fields[2],message,sizeof message,encoded)) { errno=EINVAL; return -1; }
        if(emit) {
            jdv_record_input_t row[]={mesh_field("timestamp",jdv_value_i64((int64_t)timestamp)),
                mesh_field("owner",mesh_text(c,owner)),mesh_field("message",mesh_text(c,message)),
                mesh_field("timestamp_unit",mesh_symbol(c,"unix-seconds")),mesh_field("row_kind",mesh_symbol(c,"message"))};
            if(jash_discovery_add(&c->rows,row,sizeof row/sizeof *row)) { errno=E2BIG; return -1; }
        }
        cursor=end+1;
    }
    return (int)count;
}
static int mesh_blackboard_read(mesh_call_t* c,char** bytes,size_t* length,int emit) {
    int rc=jash_mesh_read_at(c->store.root,"blackboard.log",MESH_BLACKBOARD_BYTES,bytes,length);
    if(rc<=0) return rc;
    char* inspected=emit?*bytes:malloc(*length+1);
    if(!inspected) { free(*bytes); *bytes=NULL; return -1; }
    if(!emit) memcpy(inspected,*bytes,*length+1);
    int count=mesh_blackboard_parse(c,inspected,*length,emit);
    if(!emit) free(inspected);
    if(count<0) { int saved=errno; free(*bytes); *bytes=NULL; errno=saved; return -1; }
    return count;
}

static const char* mesh_row_text(const jdv_value_t* row,const char* key,char* buffer,size_t capacity) {
    const jdv_value_t* value=jash_discovery_field(row,key);
    if(!value||value->kind<JDV_STRING||value->kind>JDV_SYMBOL||value->as.slice.len>=capacity) return "";
    memcpy(buffer,value->as.slice.data,value->as.slice.len); buffer[value->as.slice.len]=0; return buffer;
}
static uint64_t mesh_row_unsigned(const jdv_value_t* row,const char* key) {
    const jdv_value_t* value=jash_discovery_field(row,key);
    return value&&value->kind==JDV_U64?value->as.u64:0;
}
static void mesh_query_text(mesh_call_t* c) {
    const char* title=c->options.operation==M_WORKERS?"MESH FLEET":c->options.operation==M_BOARD?"MESH BOARD":c->options.operation==M_REP?"MESH REPUTATION":"MESH CLAIMS";
    jash_writef(c->writer,c->context,"=== %s ===\n",title);
    if(!c->rows.count) jash_writes(c->writer,c->context,"  (none)\n");
    for(size_t i=0;i<c->rows.count;i++) {
        const jdv_value_t* row=&c->rows.items[i]; char a[4097],b[4097],d[4097],e[4097],f[4097];
        if(c->options.operation==M_WORKERS) jash_writef(c->writer,c->context,"  %s %s/%s %s %s\n",mesh_row_text(row,"agent_id",a,sizeof a),mesh_row_text(row,"provider",b,sizeof b),mesh_row_text(row,"model",d,sizeof d),mesh_row_text(row,"status",e,sizeof e),mesh_row_text(row,"task",f,sizeof f));
        else if(c->options.operation==M_BOARD) {
            if(jash_discovery_field(row,"message")) jash_writef(c->writer,c->context,"  %s: %s\n",mesh_row_text(row,"owner",a,sizeof a),mesh_row_text(row,"message",b,sizeof b));
            else jash_writef(c->writer,c->context,"  #%llu %s [%s] %s @%s\n",(unsigned long long)mesh_row_unsigned(row,"id"),mesh_row_text(row,"state",a,sizeof a),mesh_row_text(row,"tag",b,sizeof b),mesh_row_text(row,"desc",d,sizeof d),mesh_row_text(row,"owner",e,sizeof e));
        } else if(c->options.operation==M_REP) jash_writef(c->writer,c->context,"  %s [%s] done=%llu fail=%llu\n",mesh_row_text(row,"model",a,sizeof a),mesh_row_text(row,"tag",b,sizeof b),(unsigned long long)mesh_row_unsigned(row,"done"),(unsigned long long)mesh_row_unsigned(row,"fail"));
        else jash_writef(c->writer,c->context,"  %s @%s ttl=%llus %s\n",mesh_row_text(row,"path",a,sizeof a),mesh_row_text(row,"owner",b,sizeof b),(unsigned long long)mesh_row_unsigned(row,"ttl"),mesh_row_text(row,"note",d,sizeof d));
    }
}
static int mesh_json_query_row(jash_json_writer_t* out,const jdv_value_t* row,const char* const* keys) {
    jash_jsonw_raw(out,"{");
    for(size_t i=0;keys[i];i++) {
        if(i) jash_jsonw_raw(out,",");
        jash_jsonw_string(out,keys[i]); jash_jsonw_raw(out,":");
        const jdv_value_t* value=jash_discovery_field(row,keys[i]);
        if(!value) jash_jsonw_raw(out,"null"); else if(jdv_json_write(out,value,&JDV_DEFAULT_LIMITS)) return -1;
    }
    jash_jsonw_raw(out,"}"); return 0;
}
static int mesh_query_emit(mesh_call_t* c,size_t total,int partial) {
    int cancelled=jash_query_cancel_poll(c->state);
    if(cancelled) {
        if(c->options.mode==JASH_DISCOVERY_DATA) return jash_discovery_emit_data_outcome(c->writer,c->context,"mesh",&c->rows,cancelled,JDV_STREAM_CANCELLED,"request_cancelled");
        return jash_discovery_error(c->writer,c->context,"mesh",c->options.mode,"request_cancelled",cancelled);
    }
    if(!c->options.mode) { mesh_query_text(c); return 0; }
    if(c->options.mode==JASH_DISCOVERY_DATA) return jash_discovery_emit_data_outcome(c->writer,c->context,"mesh",&c->rows,0,partial?JDV_STREAM_INCOMPLETE:JDV_STREAM_COMPLETE,partial?"page_window":NULL);
    static const char* const workers[]={"agent_id","provider","model","status","headroom","task","age_s","pid","alive","last_seen","timestamp_unit","liveness_source",NULL};
    static const char* const tasks[]={"id","state","tag","owner","deps","desc","claim_ts","timestamp_unit",NULL};
    static const char* const claims[]={"path","owner","claim_ts","ttl","note","timestamp_unit","ttl_unit",NULL};
    static const char* const rep[]={"model","tag","done","fail","score","source","verification",NULL};
    static const char* const messages[]={"timestamp","owner","message","timestamp_unit",NULL};
    const char* const* keys=c->options.operation==M_WORKERS?workers:c->options.operation==M_BOARD?(c->options.messages?messages:tasks):c->options.operation==M_REP?rep:claims;
    jash_structured_buffer_t buffer; jash_structured_buffer_init(&buffer,8u*1024u*1024u);
    jash_json_writer_t out; jash_jsonw_init(&out,jash_structured_buffer_sink,&buffer);
    if(c->options.paged) {
        jash_jsonw_raw(&out,"{\"schema\":\"jash.mesh.query.v1\",\"operation\":"); jash_jsonw_string(&out,mesh_operations[c->options.operation]);
        jash_jsonw_raw(&out,",\"complete\":"); jash_jsonw_raw(&out,partial?"false":"true"); jash_jsonw_raw(&out,",\"total\":"); jash_jsonw_safe_u64(&out,total);
        jash_jsonw_raw(&out,",\"next_offset\":"); jash_jsonw_safe_u64(&out,c->options.offset+c->rows.count); jash_jsonw_raw(&out,",\"items\":");
    }
    jash_jsonw_raw(&out,"[");
    for(size_t i=0;i<c->rows.count;i++) { if(i) jash_jsonw_raw(&out,","); if(mesh_json_query_row(&out,&c->rows.items[i],keys)) { out.error=EINVAL; break; } }
    jash_jsonw_raw(&out,c->options.paged?"]}\n":"]\n");
    int rc=jash_jsonw_finish(&out);
    if(!rc) { long budget=jash_budget_limit(); rc=jash_structured_document(c->writer,c->context,"mesh",buffer.data,buffer.used,buffer.required,budget>0?budget:8u*1024u*1024u,0); }
    else rc=125;
    jash_structured_buffer_free(&buffer); return rc;
}
static int mesh_query(mesh_call_t* c) {
    int rc=mesh_store_begin(c,0), error=0;
    if(rc) goto done;
    if(c->options.operation==M_WORKERS) {
        mesh_worker_t* workers=calloc(MESH_WORKERS_MAX,sizeof *workers); size_t count=0;
        if(!workers) error=ENOMEM;
        else if(mesh_collect(&c->store,0,workers,&count,c->state)) error=errno;
        else for(size_t i=0;i<count;i++) if(mesh_worker_row(c,&workers[i])) { error=E2BIG; break; }
        free(workers);
    } else if(c->options.operation==M_BOARD) {
        if(!c->options.messages) {
            mesh_task_t* tasks=calloc(MESH_TASKS_MAX,sizeof *tasks); size_t count=0;
            if(!tasks) error=ENOMEM;
            else if(mesh_collect(&c->store,1,tasks,&count,c->state)) error=errno;
            else for(size_t i=0;i<count;i++) if(mesh_task_row(c,&tasks[i])) { error=E2BIG; break; }
            free(tasks);
        }
        if(!error&&(c->options.messages||!c->options.mode)) { char* bytes=NULL; size_t length=0; if(mesh_blackboard_read(c,&bytes,&length,1)<0) error=errno; free(bytes); }
    } else if(c->options.operation==M_CLAIMS) {
        jash_claim_t* claims=calloc(JASH_CLAIM_MAX,sizeof *claims);
        int count=claims?jash_claims_read_at(c->store.root,claims,JASH_CLAIM_MAX):-1;
        if(count<0) error=claims?errno:ENOMEM;
        else for(int i=0;i<count;i++) if(jash_claims_live(&claims[i],c->now)&&mesh_claim_row(c,&claims[i])) { error=E2BIG; break; }
        free(claims);
    } else {
        mesh_rep_t* rep=calloc(MESH_REP_MAX,sizeof *rep); size_t count=0;
        if(!rep) error=ENOMEM;
        else if(mesh_rep_read(&c->store,rep,&count,c->state)) error=errno;
        else for(size_t i=0;i<count;i++) if(mesh_rep_row(c,&rep[i])) { error=E2BIG; break; }
        free(rep);
    }
done:
    if(c->store_opened&&jash_mesh_store_close(&c->store)&&!error&&!rc) error=errno;
    c->store_opened=0;
    if(rc||error) {
        int cancelled=jash_query_cancel_poll(c->state);
        const char* reason=cancelled?"request_cancelled":c->reason?c->reason:error==E2BIG?"source_limit":"store_source_invalid";
        int status=cancelled?cancelled:error==E2BIG?75:rc?rc:1;
        if(cancelled&&c->options.mode==JASH_DISCOVERY_DATA) return jash_discovery_emit_data_outcome(c->writer,c->context,"mesh",&c->rows,status,JDV_STREAM_CANCELLED,reason);
        return jash_discovery_error(c->writer,c->context,"mesh",c->options.mode,reason,status);
    }
    size_t total=c->rows.count;
    size_t offset=c->options.offset>total?total:(size_t)c->options.offset;
    size_t selected=total-offset; if(selected>c->options.limit) selected=(size_t)c->options.limit;
    if(offset&&selected) memmove(c->rows.items,c->rows.items+offset,selected*sizeof *c->rows.items);
    c->rows.count=selected;
    return mesh_query_emit(c,total,c->options.offset!=0||selected<total);
}
