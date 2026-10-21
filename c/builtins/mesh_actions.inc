// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/checked_string.h>
static int mesh_prepare_worker(mesh_call_t* c,mesh_worker_t* worker,int reset) {
    int found=mesh_worker_read(&c->store,c->options.self,worker);
    if(found<0) return mesh_fail(c,"worker_source_invalid",1);
    if(!found) {
        memset(worker,0,sizeof *worker); memcpy(worker->id,c->options.self,strlen(c->options.self)+1);
        if (jash_copy_cstr(worker->provider, sizeof(worker->provider), "?")) { return mesh_fail(c,"field_byte_limit",75); } if (jash_copy_cstr(worker->model, sizeof(worker->model), "?")) { return mesh_fail(c,"field_byte_limit",75); } if (jash_copy_cstr(worker->status, sizeof(worker->status), "idle")) { return mesh_fail(c,"field_byte_limit",75); }
        if (jash_copy_cstr(worker->headroom, sizeof(worker->headroom), "-")) { return mesh_fail(c,"field_byte_limit",75); } if (jash_copy_cstr(worker->task, sizeof(worker->task), "-")) { return mesh_fail(c,"field_byte_limit",75); }
    }
    const char* const* values=c->options.value;
    if(reset) {
        if (jash_copy_cstr(worker->provider, sizeof(worker->provider), values[M_PROVIDER]?values[M_PROVIDER]:"?")) { return mesh_fail(c,"field_byte_limit",75); }
        if (jash_copy_cstr(worker->model, sizeof(worker->model), values[M_MODEL]?values[M_MODEL]:"?")) { return mesh_fail(c,"field_byte_limit",75); }
        if (jash_copy_cstr(worker->status, sizeof(worker->status), "idle")) { return mesh_fail(c,"field_byte_limit",75); } if (jash_copy_cstr(worker->headroom, sizeof(worker->headroom), "-")) { return mesh_fail(c,"field_byte_limit",75); } if (jash_copy_cstr(worker->task, sizeof(worker->task), "-")) { return mesh_fail(c,"field_byte_limit",75); }
    } else {
        if(values[M_PROVIDER]) if (jash_copy_cstr(worker->provider, sizeof(worker->provider), values[M_PROVIDER])) { return mesh_fail(c,"field_byte_limit",75); }
        if(values[M_MODEL]) if (jash_copy_cstr(worker->model, sizeof(worker->model), values[M_MODEL])) { return mesh_fail(c,"field_byte_limit",75); }
        if(values[M_STATUS]) if (jash_copy_cstr(worker->status, sizeof(worker->status), values[M_STATUS])) { return mesh_fail(c,"field_byte_limit",75); }
        if(values[M_HEADROOM]) if (jash_copy_cstr(worker->headroom, sizeof(worker->headroom), values[M_HEADROOM])) { return mesh_fail(c,"field_byte_limit",75); }
        if(values[M_TASK]) if (jash_copy_cstr(worker->task, sizeof(worker->task), values[M_TASK])) { return mesh_fail(c,"field_byte_limit",75); }
    }
    if(!worker->pid) worker->pid=c->options.pid;
    worker->timestamp=c->now; return 0;
}
static int mesh_write_worker_receipt(mesh_call_t* c,const mesh_worker_t* worker,const char* action,int* applied) {
    char target[160]; snprintf(target,sizeof target,"workers/%s.tsv",worker->id);
    jash_mesh_io_t io; (void)mesh_worker_write(&c->store,worker,&io); if(applied) *applied=io.applied;
    return mesh_io_receipt(c,action,target,&io,io.applied?1u:0u);
}
static int mesh_write_task_receipt(mesh_call_t* c,const mesh_task_t* task,const char* action,int* applied) {
    char target[48]; snprintf(target,sizeof target,"tasks/%d.tsv",task->id);
    jash_mesh_io_t io; (void)mesh_task_write(&c->store,task,&io); if(applied) *applied=io.applied;
    return mesh_io_receipt(c,action,target,&io,io.applied?1u:0u);
}
static int mesh_claims_receipt(mesh_call_t* c,const jash_claim_t* claims,int count,const char* action,uint64_t changed,int* applied) {
    jash_mesh_io_t io; (void)jash_claims_write_at(c->store.root,claims,count,&io); if(applied) *applied=io.applied;
    return mesh_io_receipt(c,action,"claims.tsv",&io,io.applied?changed:0);
}
static int mesh_source_status(mesh_call_t* c,const char* reason) { return mesh_fail(c,errno==E2BIG?"source_limit":reason,errno==E2BIG?75:1); }

static int mesh_register(mesh_call_t* c) {
    mesh_worker_t worker;
    int rc=mesh_prepare_worker(c,&worker,1); if(rc) return rc;
    jdv_record_input_t result[]={mesh_field("agent_id",mesh_text(c,worker.id))}; mesh_result(c,result,1);
    if(c->rows.error) return mesh_fail(c,"response_limit",75);
    rc=mesh_write_worker_receipt(c,&worker,"register_worker",NULL);
    if(!rc&&!c->options.mode) jash_writef(c->writer,c->context,"%s\n",worker.id);
    return rc;
}
static int mesh_heartbeat(mesh_call_t* c) {
    mesh_worker_t worker; int rc=mesh_prepare_worker(c,&worker,0); if(rc) return rc;
    mesh_task_t* tasks=calloc(MESH_TASKS_MAX,sizeof *tasks);
    jash_claim_t* claims=calloc(JASH_CLAIM_MAX,sizeof *claims);
    size_t count=0,renewed=0; int claim_count=0,refreshed=0;
    if(!tasks||!claims) { rc=mesh_fail(c,"allocation_failed",125); goto done; }
    if(mesh_collect(&c->store,1,tasks,&count,NULL)) { rc=mesh_source_status(c,"task_source_invalid"); goto done; }
    claim_count=jash_claims_read_at(c->store.root,claims,JASH_CLAIM_MAX);
    if(claim_count<0) { rc=mesh_source_status(c,"claims_source_invalid"); goto done; }
    rc=mesh_write_worker_receipt(c,&worker,"heartbeat_worker",NULL);
    for(size_t i=0;i<count&&!rc;i++) if(!strcmp(tasks[i].state,"claimed")&&!strcmp(tasks[i].owner,worker.id)) {
        tasks[i].claim_ts=c->now; int applied=0;
        rc=mesh_write_task_receipt(c,&tasks[i],"renew_task",&applied); if(applied) renewed++;
    }
    if(!rc) {
        int changed=0;
        for(int i=0;i<claim_count;i++) if(!strcmp(claims[i].owner,worker.id)) { claims[i].claim_ts=c->now; changed++; }
        if(changed) { int applied=0; rc=mesh_claims_receipt(c,claims,claim_count,"renew_claims",(uint64_t)changed,&applied); if(applied) refreshed=changed; }
    }
done:
    {
        jdv_record_input_t result[]={mesh_field("agent_id",mesh_text(c,worker.id)),mesh_field("refreshed_tasks",jdv_value_u64(renewed)),mesh_field("refreshed_claims",jdv_value_u64((uint64_t)refreshed))};
        mesh_result(c,result,sizeof result/sizeof *result);
    }
    free(tasks); free(claims); return rc;
}
static int mesh_counter_write(mesh_call_t* c,int next) {
    char bytes[32]; int n=snprintf(bytes,sizeof bytes,"%d\n",next);
    jash_mesh_io_t io; (void)jash_mesh_write_at(c->store.root,"board.meta",bytes,(size_t)n,&io);
    return mesh_io_receipt(c,"advance_task_counter","board.meta",&io,io.applied?1u:0u);
}
static int mesh_add(mesh_call_t* c) {
    mesh_task_t* tasks=calloc(MESH_TASKS_MAX,sizeof *tasks); size_t count=0;
    if(!tasks) return mesh_fail(c,"allocation_failed",125);
    int rc=0,next=1;
    if(mesh_collect(&c->store,1,tasks,&count,NULL)||mesh_board_next(&c->store,&next)<0) { rc=mesh_source_status(c,"task_source_invalid"); goto done; }
    if(count==MESH_TASKS_MAX) { rc=mesh_fail(c,"task_limit",75); goto done; }
    if(count&&next<=tasks[count-1].id) {
        if(tasks[count-1].id==INT_MAX) { rc=mesh_fail(c,"task_id_exhausted",75); goto done; }
        next=tasks[count-1].id+1;
    }
    if(next==INT_MAX) { rc=mesh_fail(c,"task_id_exhausted",75); goto done; }
    mesh_task_t task={.id=next}; if (jash_copy_cstr(task.state, sizeof(task.state), "ready")) { rc=mesh_fail(c,"field_byte_limit",75); goto done; } if (jash_copy_cstr(task.owner, sizeof(task.owner), "-")) { rc=mesh_fail(c,"field_byte_limit",75); goto done; }
    if (jash_copy_cstr(task.tag, sizeof(task.tag), c->options.value[M_TAG]?c->options.value[M_TAG]:"-")) { rc=mesh_fail(c,"field_byte_limit",75); goto done; }
    if (jash_copy_cstr(task.deps, sizeof(task.deps), c->options.value[M_NEEDS]?c->options.value[M_NEEDS]:"-")) { rc=mesh_fail(c,"field_byte_limit",75); goto done; } if (jash_copy_cstr(task.desc, sizeof(task.desc), c->options.operand)) { rc=mesh_fail(c,"field_byte_limit",75); goto done; }
    jdv_record_input_t result[]={mesh_field("id",jdv_value_u64((uint64_t)next)),mesh_field("desc",mesh_text(c,task.desc)),mesh_field("tag",mesh_text(c,task.tag))};
    mesh_result(c,result,sizeof result/sizeof *result);
    if(c->rows.error) { rc=mesh_fail(c,"response_limit",75); goto done; }
    /* Reserve first: a later failed task publication may leave a gap, but it
     * cannot cause the next caller to overwrite a successfully published ID. */
    rc=mesh_counter_write(c,next+1);
    if(!rc) rc=mesh_write_task_receipt(c,&task,"add_task",NULL);
    if(!rc&&!c->options.mode) jash_writef(c->writer,c->context,"mesh: task #%d added\n",next);
done:
    free(tasks); return rc;
}
static int mesh_blackboard_line(char* buffer,size_t capacity,size_t* used,long timestamp,const char* owner,const char* message) {
    char numeric[32]; snprintf(numeric,sizeof numeric,"%ld",timestamp);
    return mesh_record_field(buffer,capacity,used,numeric,1)||mesh_record_field(buffer,capacity,used,owner,0)||
        mesh_record_field(buffer,capacity,used,message,0)||mesh_record_end(buffer,capacity,used)?-1:0;
}
static int mesh_post(mesh_call_t* c) {
    char* old=NULL; size_t length=0;
    int count=mesh_blackboard_read(c,&old,&length,0);
    if(count<0) return mesh_source_status(c,"blackboard_source_invalid");
    if(count==MESH_POSTS_MAX) { free(old); return mesh_fail(c,"message_limit",75); }
    char line[9000]; size_t used=0;
    if(mesh_blackboard_line(line,sizeof line,&used,c->now,c->options.self,c->options.operand)||used>MESH_BLACKBOARD_BYTES-length) { free(old); return mesh_fail(c,"blackboard_limit",75); }
    char* all=malloc(length+used+1); if(!all) { free(old); return mesh_fail(c,"allocation_failed",125); }
    if(length) memcpy(all,old,length);
    memcpy(all+length,line,used); free(old);
    jdv_record_input_t result[]={mesh_field("owner",mesh_text(c,c->options.self)),mesh_field("message",mesh_text(c,c->options.operand)),mesh_field("messages",jdv_value_u64((uint64_t)count+1))};
    mesh_result(c,result,sizeof result/sizeof *result);
    if(c->rows.error) { free(all); return mesh_fail(c,"response_limit",75); }
    jash_mesh_io_t io; (void)jash_mesh_write_at(c->store.root,"blackboard.log",all,length+used,&io); free(all);
    return mesh_io_receipt(c,"post_message","blackboard.log",&io,io.applied?1u:0u);
}
static int mesh_init(mesh_call_t* c) {
    mesh_task_t* tasks=calloc(MESH_TASKS_MAX,sizeof *tasks); size_t count=0,removed=0;
    if(!tasks) return mesh_fail(c,"allocation_failed",125);
    int rc=0;
    if(mesh_collect(&c->store,1,tasks,&count,NULL)) { rc=mesh_source_status(c,"task_source_invalid"); goto done; }
    /* Validate the blackboard inode before resetting any task. */
    char* prior=NULL; size_t prior_length=0;
    if(jash_mesh_read_at(c->store.root,"blackboard.log",MESH_BLACKBOARD_BYTES,&prior,&prior_length)<0) { rc=mesh_source_status(c,"blackboard_source_invalid"); goto done; }
    free(prior);
    for(size_t i=0;i<count&&!rc;i++) {
        char name[32],target[48]; snprintf(name,sizeof name,"%d.tsv",tasks[i].id); snprintf(target,sizeof target,"tasks/%s",name);
        jash_mesh_io_t io; (void)jash_mesh_remove_at(c->store.tasks,name,&io);
        if(io.applied) removed++;
        rc=mesh_io_receipt(c,"remove_task",target,&io,io.applied?1u:0u);
    }
    if(!rc) rc=mesh_counter_write(c,1);
    if(!rc) {
        jash_mesh_io_t io;
        if(c->options.operand) {
            char line[9000]; size_t used=0;
            if(mesh_blackboard_line(line,sizeof line,&used,c->now,"GOAL",c->options.operand)) { rc=mesh_fail(c,"goal_limit",75); goto done; }
            (void)jash_mesh_write_at(c->store.root,"blackboard.log",line,used,&io);
        } else (void)jash_mesh_remove_at(c->store.root,"blackboard.log",&io);
        rc=mesh_io_receipt(c,"reset_blackboard","blackboard.log",&io,io.applied?1u:0u);
    }
    if(!rc&&!c->options.mode) jash_writef(c->writer,c->context,"mesh: board initialized%s%s\n",c->options.operand?" — ":"",c->options.operand?c->options.operand:"");
done:
    {
        jdv_record_input_t result[]={mesh_field("removed_tasks",jdv_value_u64(removed)),mesh_field("goal",mesh_text(c,c->options.operand))}; mesh_result(c,result,2);
    }
    free(tasks); return rc;
}
static int mesh_claim_action(mesh_call_t* c,int remove) {
    const char* normalized=c->claim_path;
    jash_claim_t* claims=calloc(JASH_CLAIM_MAX,sizeof *claims);
    if(!claims) return mesh_fail(c,"allocation_failed",125);
    int count=jash_claims_read_at(c->store.root,claims,JASH_CLAIM_MAX),index=-1,rc=0,changed=0,applied=0;
    if(count<0) { rc=mesh_source_status(c,"claims_source_invalid"); goto done; }
    for(int i=0;i<count;i++) if(!strcmp(claims[i].path,normalized)) { index=i; break; }
    if(remove) {
        if(index>=0&&!strcmp(claims[index].owner,c->options.self)) { claims[index]=claims[--count]; changed=1; }
    } else {
        if(index>=0&&jash_claims_live(&claims[index],c->now)&&strcmp(claims[index].owner,c->options.self)) { rc=mesh_fail(c,"claim_held_by_another_owner",1); goto done; }
        if(index<0) {
            /* Expired rows are pruned only in this explicit write operation. */
            int live=0;
            for(int i=0;i<count;i++) if(jash_claims_live(&claims[i],c->now)) claims[live++]=claims[i];
            count=live;
            if(count==JASH_CLAIM_MAX) { rc=mesh_fail(c,"claim_limit",75); goto done; }
            index=count++;
        }
        jash_claim_t* claim=&claims[index]; memset(claim,0,sizeof *claim);
        if (jash_copy_cstr(claim->path, sizeof(claim->path), normalized)) { rc=mesh_fail(c,"field_byte_limit",75); goto done; } if (jash_copy_cstr(claim->owner, sizeof(claim->owner), c->options.self)) { rc=mesh_fail(c,"field_byte_limit",75); goto done; } claim->claim_ts=c->now; claim->ttl=(long)c->options.ttl;
        if (jash_copy_cstr(claim->note, sizeof(claim->note), c->options.value[M_NOTE]?c->options.value[M_NOTE]:"-")) { rc=mesh_fail(c,"field_byte_limit",75); goto done; } changed=1;
    }
    if(changed) rc=mesh_claims_receipt(c,claims,count,remove?"release_claim":"claim_path",1,&applied);
    if(!rc&&!c->options.mode) jash_writef(c->writer,c->context,"mesh: %s %s\n",remove?(changed?"released":"no claim by you on"):"claimed",normalized);
done:
    {
        jdv_record_input_t result[]={mesh_field(remove?"released":"claimed",jash_discovery_path(&c->rows,normalized)),mesh_field("owner",mesh_text(c,c->options.self)),mesh_field("changed",jdv_value_bool(applied))}; mesh_result(c,result,3);
    }
    free(claims); return rc;
}

#include "mesh_actions_extra.inc"
static int mesh_action(mesh_call_t* c) {
    switch(c->options.operation) {
        case M_REGISTER: return mesh_register(c);
        case M_HEARTBEAT: return mesh_heartbeat(c);
        case M_INIT: return mesh_init(c);
        case M_ADD: return mesh_add(c);
        case M_NEXT: return mesh_next(c);
        case M_DONE: case M_FAIL: return mesh_finish_task(c);
        case M_POST: return mesh_post(c);
        case M_SWEEP: return mesh_sweep(c);
        case M_CLAIM: return mesh_claim_action(c,0);
        case M_UNCLAIM: return mesh_claim_action(c,1);
        default: return mesh_fail(c,"invalid_operation",2);
    }
}
