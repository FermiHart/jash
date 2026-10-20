// SPDX-License-Identifier: LicenseRef-SPL-1.0
static int mesh_next(mesh_call_t* c) {
    mesh_task_t* tasks=calloc(MESH_TASKS_MAX,sizeof *tasks);
    mesh_worker_t* workers=calloc(MESH_WORKERS_MAX,sizeof *workers);
    mesh_rep_t* reputation=calloc(MESH_REP_MAX,sizeof *reputation);
    size_t task_count=0,worker_count=0,rep_count=0;
    int rc=0,picked=-1,claimed=0,registry_updated=0; int64_t best_score=0;
    mesh_worker_t self={0};
    if(!tasks||!workers||!reputation) { rc=mesh_fail(c,"allocation_failed",125); goto done; }
    if(mesh_collect(&c->store,1,tasks,&task_count,NULL)||mesh_collect(&c->store,0,workers,&worker_count,NULL)||mesh_rep_read(&c->store,reputation,&rep_count,NULL)) { rc=mesh_source_status(c,"routing_source_invalid"); goto done; }
    rc=mesh_prepare_worker(c,&self,0); if(rc) goto done;
    for(size_t i=0;i<task_count;i++) {
        mesh_task_t* task=&tasks[i]; int reclaimable=0;
        if(!strcmp(task->state,"claimed")) {
            int gone=0;
            for(size_t j=0;j<worker_count;j++) if(!strcmp(workers[j].id,task->owner)) { gone=mesh_pid_alive(workers[j].pid)==0; break; }
            reclaimable=gone||(c->now>task->claim_ts&&(uint64_t)c->now-(uint64_t)task->claim_ts>MESH_LEASE_TTL);
        }
        if((strcmp(task->state,"ready")&&!reclaimable)||!mesh_task_dependencies(task,tasks,task_count)) continue;
        int64_t score=0;
        for(size_t j=0;j<rep_count;j++) if(!strcmp(reputation[j].model,self.model)&&!strcmp(reputation[j].tag,task->tag)) { score=(int64_t)reputation[j].done-(int64_t)reputation[j].fail; break; }
        if(picked<0||score>best_score) { picked=(int)i; best_score=score; }
    }
    if(picked<0) {
        c->predicate=1; rc=1;
        if(!c->options.mode) jash_writes(c->writer,c->context,"mesh: no ready task\n");
        goto done;
    }
    mesh_task_t* selected=&tasks[picked];
    strcpy(selected->state,"claimed"); strcpy(selected->owner,c->options.self); selected->claim_ts=c->now;
    rc=mesh_write_task_receipt(c,selected,"claim_task",&claimed);
    if(!rc) {
        strcpy(self.status,"working"); snprintf(self.task,sizeof self.task,"%d",selected->id);
        rc=mesh_write_worker_receipt(c,&self,"assign_worker",&registry_updated);
    }
    if(!rc&&!c->options.mode) jash_writef(c->writer,c->context,"#%d [%s] %s\n",selected->id,selected->tag,selected->desc);
done:
    {
        const mesh_task_t* observed=picked>=0?&tasks[picked]:NULL;
        jdv_record_input_t result[]={mesh_field("available",jdv_value_bool(observed!=NULL)),
            mesh_field("id",observed?jdv_value_u64((uint64_t)observed->id):jdv_value_nothing()),
            mesh_field("tag",mesh_text(c,observed?observed->tag:NULL)),mesh_field("desc",mesh_text(c,observed?observed->desc:NULL)),
            mesh_field("cap_score",jdv_value_i64(best_score)),mesh_field("claimed",jdv_value_bool(claimed)),
            mesh_field("registry_updated",jdv_value_bool(registry_updated)),mesh_field("routing_source",mesh_symbol(c,"local-chain-events-unverified"))};
        mesh_result(c,result,sizeof result/sizeof *result);
    }
    free(tasks); free(workers); free(reputation); return rc;
}

static long mesh_null_writer(void* context,const char* bytes,size_t length) { (void)context; (void)bytes; return (long)length; }
static int mesh_task_same(const mesh_task_t* a,const mesh_task_t* b) {
    return a->id==b->id&&a->claim_ts==b->claim_ts&&!strcmp(a->state,b->state)&&!strcmp(a->owner,b->owner)&&
        !strcmp(a->tag,b->tag)&&!strcmp(a->deps,b->deps)&&!strcmp(a->desc,b->desc);
}
static int mesh_attest_task(mesh_call_t* c,const char* event,const mesh_task_t* task,const char* model) {
    char tag[96],encoded_model[384],owner[384],detail[900];
    if(mesh_token_encode(task->tag,tag,sizeof tag)||mesh_token_encode(model,encoded_model,sizeof encoded_model)||
        mesh_token_encode(c->options.self,owner,sizeof owner)) return mesh_fail(c,"attestation_field_limit",75);
    int n=snprintf(detail,sizeof detail,"task=%d tag=%s owner=%s model=%s mesh_format=2",task->id,tag,owner,encoded_model);
    if(n<0||(size_t)n>=sizeof detail) return mesh_fail(c,"attestation_field_limit",75);
    jash_sigil_attest(event,detail);
    /* The existing callback returns void and may be disabled by SIGIL=0.
     * Invocation is observable; persistence and signature validity are not. */
    jash_mesh_io_t requested={0};
    return mesh_receipt(c,"request_attestation",NULL,&requested,1,NULL);
}
static int mesh_finish_task(mesh_call_t* c) {
    uint64_t id; (void)jash_mesh_number(c->options.operand,INT_MAX,&id);
    mesh_task_t task; int loaded=mesh_task_read(&c->store,(int)id,&task);
    if(loaded<=0) return loaded<0?mesh_source_status(c,"task_source_invalid"):mesh_fail(c,"task_not_found",1);
    int foreign=!strcmp(task.state,"claimed")&&strcmp(task.owner,c->options.self);
    if(foreign&&!c->options.force) {
        if(!c->options.mode) jash_writef(jash_stderr_writer,NULL,"mesh: task #%d belongs to %s; use --force to override\n",task.id,task.owner);
        return mesh_fail(c,"task_owned_by_another_worker",1);
    }
    mesh_worker_t worker; int rc=mesh_prepare_worker(c,&worker,0); if(rc) return rc;
    char description[sizeof task.desc]; strcpy(description,task.desc);
    if(c->options.value[M_ARTIFACT]) {
        int n=snprintf(description,sizeof description,"%s  [→%s]",task.desc,c->options.value[M_ARTIFACT]);
        if(n<0||(size_t)n>=sizeof description) return mesh_fail(c,"artifact_description_limit",2);
    }
    char original_state[sizeof task.state]; strcpy(original_state,task.state);
    int proof_executed=0,proof_status=0,applied=0,attestation_requested=0;
    const char* proof=c->options.value[M_PROVE];
    if(proof) {
        /* expect may execute commands, including mesh. Release our flock
         * before invoking it, then reacquire and revalidate the exact task. */
        struct stat root_identity;
        if(fstat(c->store.root,&root_identity)) return mesh_source_status(c,"store_identity_failed");
        if(jash_mesh_store_close(&c->store)) { c->store_opened=0; return mesh_fail(c,"store_close_failed",1); }
        c->store_opened=0;
        int saved=-1;
        if(jash_discovery_callback_begin(c->options.mode,&saved)) return mesh_fail(c,"proof_output_setup_failed",125);
        proof_status=jash_b_expect(c->state,mesh_null_writer,NULL,proof); proof_executed=1;
        int restored=jash_discovery_callback_end(saved);
        jash_mesh_io_t proof_effect={.applied=1,.verified=proof_status==0};
        if(mesh_receipt(c,"execute_proof",NULL,&proof_effect,1,proof_status?"proof_failed":restored?"proof_output_restore_failed":NULL)) { rc=125; goto done; }
        if(restored) { rc=mesh_fail(c,"proof_output_restore_failed",125); goto done; }
        rc=mesh_store_begin(c,1); if(rc) goto done;
        struct stat current_identity; mesh_task_t current;
        int found=mesh_task_read(&c->store,task.id,&current);
        if(fstat(c->store.root,&current_identity)||current_identity.st_dev!=root_identity.st_dev||current_identity.st_ino!=root_identity.st_ino||found!=1||!mesh_task_same(&task,&current)) { rc=mesh_fail(c,"task_changed_during_proof",1); goto done; }
        if(proof_status) {
            rc=mesh_attest_task(c,"mesh-unproven",&task,worker.model); attestation_requested=!rc;
            if(!rc) rc=mesh_fail(c,"proof_failed",1);
            if(!c->options.mode) jash_writes(jash_stderr_writer,NULL,"mesh done: proof failed; task remains unchanged\n");
            goto done;
        }
    }
    strcpy(task.desc,description); strcpy(task.state,c->options.operation==M_DONE?"done":"failed");
    rc=mesh_write_task_receipt(c,&task,"finish_task",&applied);
    if(!rc) { strcpy(worker.status,"idle"); strcpy(worker.task,"-"); rc=mesh_write_worker_receipt(c,&worker,"idle_worker",NULL); }
    if(!rc) { rc=mesh_attest_task(c,c->options.operation==M_DONE?"mesh-done":"mesh-fail",&task,worker.model); attestation_requested=!rc; }
    if(!rc&&foreign&&c->options.force) jash_writef(jash_stderr_writer,NULL,"mesh: WARNING — task #%d closed by %s via --force\n",task.id,c->options.self);
    if(!rc&&!c->options.mode) jash_writef(c->writer,c->context,"mesh: task #%d → %s%s\n",task.id,task.state,proof?" (proven)":"");
done:
    {
        jdv_record_input_t result[]={mesh_field("id",jdv_value_u64(id)),mesh_field("state",mesh_text(c,applied?task.state:original_state)),
            mesh_field("requested_state",mesh_symbol(c,c->options.operation==M_DONE?"done":"failed")),
            mesh_field("applied",jdv_value_bool(applied)),mesh_field("proof_executed",jdv_value_bool(proof_executed)),
            mesh_field("proof_status",proof_executed?jdv_value_i64(proof_status):jdv_value_nothing()),
            mesh_field("attestation_requested",jdv_value_bool(attestation_requested)),mesh_field("attestation_verified",jdv_value_bool(0)),
            mesh_field("force",jdv_value_bool(c->options.force))}; mesh_result(c,result,sizeof result/sizeof *result);
    }
    return rc;
}

static int mesh_sweep(mesh_call_t* c) {
    mesh_worker_t* workers=calloc(MESH_WORKERS_MAX,sizeof *workers);
    mesh_task_t* tasks=calloc(MESH_TASKS_MAX,sizeof *tasks);
    jash_claim_t* claims=calloc(JASH_CLAIM_MAX,sizeof *claims);
    size_t worker_count=0,task_count=0,dead=0,reclaimed=0,dropped=0,agents_count=0;
    jdv_value_t agents[MESH_WORKERS_MAX]; int claim_count=0,rc=0;
    if(!workers||!tasks||!claims) { rc=mesh_fail(c,"allocation_failed",125); goto done; }
    if(mesh_collect(&c->store,0,workers,&worker_count,NULL)||mesh_collect(&c->store,1,tasks,&task_count,NULL)) { rc=mesh_source_status(c,"sweep_source_invalid"); goto done; }
    claim_count=jash_claims_read_at(c->store.root,claims,JASH_CLAIM_MAX);
    if(claim_count<0) { rc=mesh_source_status(c,"claims_source_invalid"); goto done; }
    for(size_t i=0;i<worker_count&&!rc;i++) {
        mesh_worker_t* worker=&workers[i];
        if(mesh_pid_alive(worker->pid)!=0) continue;
        int already_dead=!strcmp(worker->status,"dead"),pending=0;
        for(size_t j=0;j<task_count;j++) if(!strcmp(tasks[j].state,"claimed")&&!strcmp(tasks[j].owner,worker->id)) pending=1;
        for(int j=0;j<claim_count;j++) if(!strcmp(claims[j].owner,worker->id)) pending=1;
        if(already_dead&&!pending) continue;
        char previous_task[256]; strcpy(previous_task,worker->task);
        uint64_t silent=worker->timestamp<c->now?(uint64_t)c->now-(uint64_t)worker->timestamp:0;
        /* Bound the callback detail before the first effect for this worker.
         * The existing Sigil API accepts at most 899 detail bytes. */
        char agent[384],task[768],detail[1600];
        if(mesh_token_encode(worker->id,agent,sizeof agent)||mesh_token_encode(previous_task,task,sizeof task)) { rc=mesh_fail(c,"attestation_field_limit",75); break; }
        int n=snprintf(detail,sizeof detail,"agent=%s task=%s silent=%llus reclaimed=%u mesh_format=2",agent,task,(unsigned long long)silent,MESH_TASKS_MAX);
        if(n<0||n>=900) { rc=mesh_fail(c,"attestation_field_limit",75); break; }
        agents[agents_count++]=mesh_text(c,worker->id);
        if(!already_dead) {
            strcpy(worker->status,"dead"); strcpy(worker->task,"-"); worker->timestamp=c->now;
            int applied=0; rc=mesh_write_worker_receipt(c,worker,"mark_dead_worker",&applied); if(applied) dead++;
        }
        size_t owner_reclaimed=0;
        for(size_t j=0;j<task_count&&!rc;j++) if(!strcmp(tasks[j].state,"claimed")&&!strcmp(tasks[j].owner,worker->id)) {
            strcpy(tasks[j].state,"ready"); strcpy(tasks[j].owner,"-"); tasks[j].claim_ts=0;
            int applied=0; rc=mesh_write_task_receipt(c,&tasks[j],"reclaim_dead_task",&applied);
            if(applied) { reclaimed++; owner_reclaimed++; }
        }
        if(!rc) {
            int kept=0,removed=0;
            for(int j=0;j<claim_count;j++) { if(!strcmp(claims[j].owner,worker->id)) removed++; else claims[kept++]=claims[j]; }
            if(removed) { int applied=0; rc=mesh_claims_receipt(c,claims,kept,"drop_dead_claims",(uint64_t)removed,&applied); if(applied) { dropped+=(size_t)removed; claim_count=kept; } }
        }
        if(!rc) {
            n=snprintf(detail,sizeof detail,"agent=%s task=%s silent=%llus reclaimed=%zu mesh_format=2",agent,task,(unsigned long long)silent,owner_reclaimed);
            if(n<0||n>=900) { rc=mesh_fail(c,"attestation_field_limit",75); break; }
            jash_sigil_attest("mesh-dead",detail); jash_mesh_io_t requested={0};
            rc=mesh_receipt(c,"request_attestation",NULL,&requested,1,NULL);
        }
    }
    if(!rc&&!c->options.mode) jash_writef(c->writer,c->context,"mesh: %zu dead agents swept, %zu tasks reclaimed\n",dead,reclaimed);
done:
    {
        jdv_value_t list=jdv_value_nothing(); if(jdv_value_list(&c->rows.arena,agents,agents_count,&list)) c->rows.error=1;
        jdv_record_input_t result[]={mesh_field("dead",jdv_value_u64(dead)),mesh_field("reclaimed",jdv_value_u64(reclaimed)),
            mesh_field("claims_dropped",jdv_value_u64(dropped)),mesh_field("agents",list),mesh_field("liveness_source",mesh_symbol(c,"explicit-harness-pid-kill-zero"))};
        mesh_result(c,result,sizeof result/sizeof *result);
    }
    free(workers); free(tasks); free(claims); return rc;
}
