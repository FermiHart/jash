// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Native speculative observations: a bounded directory copy, supervised exact
 * argv, a comparison against that copy, and the existing guarded cleanup. The
 * copy provides no kernel, network, environment, or pathname isolation. */
#include "analysis_output.h"
#include "semantic_events.h"
#include "memory_store.h"
#include <core/agent_process.h>
#include <core/execution_ir.h>

#define SP_ENTRIES 4096u
#define SP_DEPTH 64u
#define SP_PATH_BYTES (512u * 1024u)
#define SP_COPY_BYTES (64ull * 1024ull * 1024ull)
#define SP_CHANGES 1024u
#define SP_CHANGE_PATH_BYTES (64u * 1024u)
#define SP_LINK_BYTES (64u * 1024u)
#define SP_CAPTURE_MAX (256u * 1024u)
#define SP_SPOOL_LIMIT (1024u * 1024u)

typedef struct {
    char* path;
    char* link;
    struct stat metadata;
    uint8_t hash[32];
    int hashed, omitted;
    const char* omission;
} sp_entry_t;
typedef struct {
    sp_entry_t* entries;
    size_t count, path_bytes, link_bytes, omissions;
    uint64_t file_bytes;
    struct stat root;
    int complete;
} sp_snapshot_t;
typedef struct { const sp_entry_t* before; const sp_entry_t* after; const char* kind; } sp_change_t;
typedef struct {
    int keep, quiet, timeout, argc;
    char** argv;
    size_t capture;
} sp_options_t;
typedef struct {
    int attempted, complete, timed_out, number;
    const char* error;
    jash_agent_process_result_t process;
    uint8_t* bytes[2]; size_t kept[2]; uint64_t seen[2]; int truncated[2];
    int64_t elapsed_ms;
} sp_execution_t;
typedef struct {
    int root, tree, created, copied, kept, removed, cleanup_error, retained_verified, retained_tree;
    char path[SPEC_MAX_PATH], retained[SPEC_MAX_PATH];
    struct stat owned;
} sp_workspace_t;

static int sp_number(const char* text,unsigned max,unsigned* value) {
    if (!text || !*text) return -1;
    unsigned result = 0;
    for (const unsigned char* p=(const unsigned char*)text;*p;p++) {
        if (*p<'0'||*p>'9'||result>(max-(unsigned)(*p-'0'))/10u) return -1;
        result=result*10u+(unsigned)(*p-'0');
    }
    *value=result; return 0;
}
static int sp_parse(int argc,char** argv,sp_options_t* options) {
    *options=(sp_options_t){.timeout=5,.capture=SPEC_CAPTURE_SIZE};unsigned seen=0;int i=0;
    while(i<argc) {
        const char* a=argv[i];
        if(!strcmp(a,"--")){i++;break;}
        unsigned bit=!strcmp(a,"--keep")?1u:!strcmp(a,"--quiet")?2u:!strcmp(a,"--timeout")?4u:!strcmp(a,"--capture-bytes")?8u:0u;
        if(!bit) { if(a[0]=='-') return -1; break; }
        if(seen&bit)return -1;
        seen|=bit;
        if(bit==1)options->keep=1;
        else if(bit==2)options->quiet=1;
        else {
            unsigned value=0;
            if(++i==argc||sp_number(argv[i],bit==4?3600u:SP_CAPTURE_MAX,&value)||!value)return -1;
            if(bit==4)options->timeout=(int)value;else options->capture=value;
        }
        i++;
    }
    options->argc=argc-i;options->argv=argv+i;
    if(!options->argc||options->argc>(int)JASH_EXEC_IR_MAX_ARGV||!options->argv[0][0])return -1;
    for(int n=0;n<options->argc;n++) {
        const char* a=options->argv[n];size_t length=strlen(a);
        if(a[0]=='/'||!strcmp(a,"..")||!strncmp(a,"../",3)||strstr(a,"/../")||
           (length>=3&&!strcmp(a+length-3,"/..")))return -2;
    }
    return 0;
}
static int sp_same(const struct stat* a,const struct stat* b) {
    return memory_store_same(a,b) && a->st_mode==b->st_mode;
}
static int sp_path(char out[SPEC_MAX_PATH],const char* prefix,const char* name) {
    int size=snprintf(out,SPEC_MAX_PATH,"%s%s%s",prefix,prefix[0]?"/":"",name);
    if(size<0||(size_t)size>=SPEC_MAX_PATH){errno=ENAMETOOLONG;return -1;}return 0;
}
static void sp_snapshot_free(sp_snapshot_t* snapshot) {
    for(size_t i=0;i<snapshot->count;i++){free(snapshot->entries[i].path);free(snapshot->entries[i].link);}
    free(snapshot->entries);memset(snapshot,0,sizeof(*snapshot));
}
static int sp_entry_compare(const void* a,const void* b) {
    return strcmp(((const sp_entry_t*)a)->path,((const sp_entry_t*)b)->path);
}
static sp_entry_t* sp_append(sp_snapshot_t* snapshot,const char* path,const struct stat* metadata) {
    size_t length=strlen(path)+1u;
    if(snapshot->count==SP_ENTRIES||length>SP_PATH_BYTES-snapshot->path_bytes){errno=E2BIG;return NULL;}
    sp_entry_t* entry=&snapshot->entries[snapshot->count];
    entry->path=strdup(path);if(!entry->path)return NULL;
    entry->metadata=*metadata;snapshot->count++;snapshot->path_bytes+=length;return entry;
}
static int sp_parent(int root,const char* path,const char** name) {
    char parent[SPEC_MAX_PATH];size_t length=strlen(path);
    if(!length||length>=sizeof parent){errno=ENAMETOOLONG;return -1;}
    memcpy(parent,path,length+1u);char* slash=strrchr(parent,'/');
    *name=strrchr(path,'/');*name=*name?*name+1:path;
    int fd=fcntl(root,F_DUPFD_CLOEXEC,3);if(fd<0)return -1;
    if(!slash)return fd;
    *slash=0;
    char* cursor=parent;char* component;
    while((component=strsep(&cursor,"/"))) {
        if(!*component||!strcmp(component,".")||!strcmp(component,"..")){close(fd);errno=EINVAL;return -1;}
        int next=openat(fd,component,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        int error=errno;
        if(close(fd)&&next>=0){error=errno;close(next);next=-1;}
        if(next<0){errno=error;return -1;}fd=next;
    }
    return fd;
}
static int sp_hash_fd(int fd,int parent,const char* name,const struct stat* expected,uint8_t hash[32],
                      int target,uint64_t* total) {
    struct stat before,after,named;int error=0;
    if(fstat(fd,&before))error=errno;
    else if(!S_ISREG(before.st_mode)||before.st_size<0)error=EINVAL;
    else if(!sp_same(&before,expected))error=ESTALE;
    else if((uint64_t)before.st_size>SPEC_MAX_FILE_SIZE || (uint64_t)before.st_size>SP_COPY_BYTES-*total)error=E2BIG;
    struct blake3 digest;blake3_init(&digest);uint8_t bytes[16384];uint64_t copied=0;
    while(!error) {
        ssize_t size=read(fd,bytes,sizeof bytes);
        if(size<0){if(errno==EINTR)continue;error=errno;break;}if(!size)break;
        if((uint64_t)size>SPEC_MAX_FILE_SIZE-copied||(uint64_t)size>SP_COPY_BYTES-*total){error=E2BIG;break;}
        copied+=(uint64_t)size;*total+=(uint64_t)size;
        if(target>=0)for(size_t offset=0;offset<(size_t)size;) {
            ssize_t written=write(target,bytes+offset,(size_t)size-offset);
            if(written<0&&errno==EINTR)continue;
            if(written<=0){error=written<0?errno:EIO;break;}offset+=(size_t)written;
        }
        if(error)break;
        blake3_update(&digest,bytes,(uint64_t)size);
    }
    if(!error&&(fstat(fd,&after)||fstatat(parent,name,&named,AT_SYMLINK_NOFOLLOW)))error=errno;
    if(!error&&(!sp_same(&before,&after)||!sp_same(&before,&named)||copied!=(uint64_t)before.st_size))error=ESTALE;
    if(error){errno=error;return -1;}blake3_final(&digest,hash,32);return 0;
}
static int sp_scan_directory(int fd,const char* prefix,unsigned depth,int source,sp_snapshot_t* snapshot) {
    if(depth>SP_DEPTH){errno=E2BIG;return -1;}
    struct stat before,after;if(fstat(fd,&before))return -1;
    int duplicate=fcntl(fd,F_DUPFD_CLOEXEC,3);if(duplicate<0)return -1;
    DIR* directory=fdopendir(duplicate);if(!directory){int error=errno;close(duplicate);errno=error;return -1;}
    int error=0;
    while(!error) {
        errno=0;struct dirent* item=readdir(directory);
        if(!item){error=errno;break;}
        if(!strcmp(item->d_name,".")||!strcmp(item->d_name,".."))continue;
        char path[SPEC_MAX_PATH];struct stat metadata;
        if(sp_path(path,prefix,item->d_name)||fstatat(fd,item->d_name,&metadata,AT_SYMLINK_NOFOLLOW)){error=errno;break;}
        sp_entry_t* entry=sp_append(snapshot,path,&metadata);if(!entry){error=errno;break;}
        if(source&&(spec_skip_dir(item->d_name)||(!S_ISREG(metadata.st_mode)&&!S_ISDIR(metadata.st_mode)))) {
            entry->omitted=1;entry->omission=spec_skip_dir(item->d_name)?"configured_name_exclusion":
                S_ISLNK(metadata.st_mode)?"symlink_not_copied":"nonregular_not_copied";
            snapshot->omissions++;continue;
        }
        if(S_ISDIR(metadata.st_mode)) {
            int child=openat(fd,item->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat opened;
            if(child<0){error=errno;break;}
            if(fstat(child,&opened))error=errno;
            else if(!sp_same(&metadata,&opened))error=ESTALE;
            if(!error&&sp_scan_directory(child,path,depth+1u,source,snapshot))error=errno;
            if(close(child)&&!error)error=errno;
        } else if(S_ISREG(metadata.st_mode)) {
            if(metadata.st_size<0||(uint64_t)metadata.st_size>SPEC_MAX_FILE_SIZE){error=E2BIG;break;}
            if(source) {
                if((uint64_t)metadata.st_size>SP_COPY_BYTES-snapshot->file_bytes){error=E2BIG;break;}
                snapshot->file_bytes+=(uint64_t)metadata.st_size;
            } else {
                int input=openat(fd,item->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
                if(input<0){error=errno;break;}
                if(sp_hash_fd(input,fd,item->d_name,&metadata,entry->hash,-1,&snapshot->file_bytes))error=errno;
                else entry->hashed=1;
                if(close(input)&&!error)error=errno;
            }
        } else if(S_ISLNK(metadata.st_mode)) {
            char target[SPEC_MAX_PATH];ssize_t length=readlinkat(fd,item->d_name,target,sizeof target);
            if(length<0){error=errno;break;}
            if((size_t)length==sizeof target||(size_t)length+1u>SP_LINK_BYTES-snapshot->link_bytes){error=E2BIG;break;}
            target[length]=0;entry->link=strdup(target);if(!entry->link){error=errno;break;}
            snapshot->link_bytes+=(size_t)length+1u;
            struct stat named;if(fstatat(fd,item->d_name,&named,AT_SYMLINK_NOFOLLOW))error=errno;
            else if(!sp_same(&metadata,&named))error=ESTALE;
        }
    }
    if(!error&&fstat(fd,&after))error=errno;
    if(!error&&!sp_same(&before,&after))error=ESTALE;
    if(closedir(directory)&&!error)error=errno;
    if(error){errno=error;return -1;}return 0;
}
static int sp_scan(int fd,int source,sp_snapshot_t* snapshot) {
    snapshot->entries=calloc(SP_ENTRIES,sizeof(*snapshot->entries));if(!snapshot->entries)return -1;
    if(fstat(fd,&snapshot->root)||sp_scan_directory(fd,"",0,source,snapshot))return -1;
    qsort(snapshot->entries,snapshot->count,sizeof(*snapshot->entries),sp_entry_compare);snapshot->complete=1;return 0;
}
static int sp_copy(int source,int target,sp_snapshot_t* snapshot,uint64_t* written) {
    *written=0;int error=0;
    for(size_t i=0;i<snapshot->count&&!error;i++) {
        sp_entry_t* entry=&snapshot->entries[i];if(entry->omitted)continue;
        const char* source_name;const char* target_name;
        int input_parent=sp_parent(source,entry->path,&source_name);
        if(input_parent<0){error=errno;break;}
        int output_parent=sp_parent(target,entry->path,&target_name);
        if(output_parent<0){error=errno;close(input_parent);break;}
        struct stat named;
        if(fstatat(input_parent,source_name,&named,AT_SYMLINK_NOFOLLOW))error=errno;
        else if(!sp_same(&named,&entry->metadata))error=ESTALE;
        if(!error&&S_ISDIR(entry->metadata.st_mode)) {
            if(mkdirat(output_parent,target_name,0700))error=errno;
        } else if(!error) {
            int input=openat(input_parent,source_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
            int output=-1;
            if(input<0)error=errno;
            else if((output=openat(output_parent,target_name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600))<0)error=errno;
            else if(sp_hash_fd(input,input_parent,source_name,&entry->metadata,entry->hash,output,written))error=errno;
            else {entry->hashed=1;if(fchmod(output,entry->metadata.st_mode&0777))error=errno;}
            if(input>=0&&close(input)&&!error)error=errno;
            if(output>=0&&close(output)&&!error)error=errno;
        }
        if(close(input_parent)&&!error)error=errno;
        if(close(output_parent)&&!error)error=errno;
    }
    /* Samples must still refer to the source directories inventoried above.
     * A coherent content snapshot between these samples is not promised. */
    for(size_t n=snapshot->count;n&&!error;n--) {
        sp_entry_t* entry=&snapshot->entries[n-1u];if(entry->omitted)continue;
        if(S_ISDIR(entry->metadata.st_mode)) {
            const char* name;int parent=sp_parent(source,entry->path,&name);struct stat current;
            if(parent<0){error=errno;break;}
            if(fstatat(parent,name,&current,AT_SYMLINK_NOFOLLOW))error=errno;
            else if(!sp_same(&entry->metadata,&current))error=ESTALE;
            if(close(parent)&&!error)error=errno;
            parent=sp_parent(target,entry->path,&name);
            if(parent<0){error=errno;break;}
            int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(fd<0)error=errno;else if(fchmod(fd,entry->metadata.st_mode&0777))error=errno;
            if(fd>=0&&close(fd)&&!error)error=errno;
            if(close(parent)&&!error)error=errno;
        }
        entry->metadata.st_mode=(entry->metadata.st_mode&S_IFMT)|(entry->metadata.st_mode&0777);
    }
    struct stat current;if(!error&&fstat(source,&current))error=errno;
    if(!error&&!sp_same(&snapshot->root,&current))error=ESTALE;
    if(error){errno=error;return -1;}return 0;
}
static const char* sp_kind(mode_t mode) {
    return S_ISREG(mode)?"file":S_ISDIR(mode)?"directory":S_ISLNK(mode)?"symlink":S_ISFIFO(mode)?"fifo":
        S_ISSOCK(mode)?"socket":S_ISCHR(mode)?"character_device":S_ISBLK(mode)?"block_device":"unknown";
}
static int sp_equal(const sp_entry_t* a,const sp_entry_t* b) {
    if((a->metadata.st_mode&(S_IFMT|07777))!=(b->metadata.st_mode&(S_IFMT|07777)))return 0;
    if(S_ISREG(a->metadata.st_mode))return a->hashed&&b->hashed&&a->metadata.st_size==b->metadata.st_size&&!memcmp(a->hash,b->hash,32);
    if(S_ISLNK(a->metadata.st_mode))return a->link&&b->link&&!strcmp(a->link,b->link);
    if(S_ISCHR(a->metadata.st_mode)||S_ISBLK(a->metadata.st_mode))return a->metadata.st_rdev==b->metadata.st_rdev;
    return 1;
}
static int sp_changes(const sp_snapshot_t* before,const sp_snapshot_t* after,sp_change_t* changes,size_t* count) {
    size_t left=0,right=0,paths=0;*count=0;
    while(left<before->count||right<after->count) {
        while(left<before->count&&before->entries[left].omitted)left++;
        const sp_entry_t* a=left<before->count?&before->entries[left]:NULL;
        const sp_entry_t* b=right<after->count?&after->entries[right]:NULL;
        if(!a&&!b)break;
        int order=!a?1:!b?-1:strcmp(a->path,b->path);const char* kind;
        if(order<0){kind="deleted";b=NULL;left++;}
        else if(order>0){kind="created";a=NULL;right++;}
        else {left++;right++;if(sp_equal(a,b))continue;kind=(a->metadata.st_mode&S_IFMT)!=(b->metadata.st_mode&S_IFMT)?"type_changed":"modified";}
        const char* path=a?a->path:b->path;size_t length=strlen(path)+1u;
        if(*count==SP_CHANGES||length>SP_CHANGE_PATH_BYTES-paths){errno=E2BIG;return -1;}
        paths+=length;changes[(*count)++]=(sp_change_t){a,b,kind};
    }
    return 0;
}
static int sp_workspace_create(sp_workspace_t* work) {
    strcpy(work->path,"/tmp/jash-speculate-native-XXXXXX");
    if(!mkdtemp(work->path))return -1;
    work->created=1;
    work->root=open(work->path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(work->root<0)return -1;
    if(fstat(work->root,&work->owned)||memory_store_trusted(work->root,1))return -1;
    if(mkdirat(work->root,"tree",0700))return -1;
    work->tree=openat(work->root,"tree",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    return work->tree<0?-1:0;
}
static int sp_workspace_current(const sp_workspace_t* work) {
    struct stat named;
    if(lstat(work->path,&named))return -1;
    if(!spec_same_inode(&named,&work->owned)||!S_ISDIR(named.st_mode)){errno=ESTALE;return -1;}return 0;
}
static void sp_workspace_cleanup(sp_workspace_t* work,int keep,int may_clean) {
    if(!work->created)return;
    if(work->root<0||sp_workspace_current(work)) {
        work->cleanup_error=errno?errno:ESTALE;return;
    }
    if(keep||!may_clean) {
        strcpy(work->retained,work->path);work->retained_verified=1;work->kept=keep;
        if(!may_clean)work->cleanup_error=EBUSY;
        return;
    }
    int parent=open("/tmp",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(parent<0){work->cleanup_error=errno;strcpy(work->retained,work->path);work->retained_verified=1;return;}
    const char* name=strrchr(work->path,'/')+1;char hold[64]="";
    if(spec_capture_owned(parent,name,&work->owned,hold,sizeof hold)) {
        work->cleanup_error=errno;
        if(hold[0])spec_hold_path(work->path,hold,work->retained,sizeof work->retained);
        else strcpy(work->retained,work->path);
    } else {
        char captured[SPEC_MAX_PATH];spec_hold_path(work->path,hold,captured,sizeof captured);
        spec_cleanup_limits_t limits={SP_ENTRIES*2u+4u,SP_DEPTH+2u};
        if(spec_rm_rf_at_checked(parent,hold,captured,work->retained,sizeof work->retained,&limits,0))work->cleanup_error=errno?errno:EIO;
        else work->removed=1;
    }
    if(close(parent)&&!work->cleanup_error)work->cleanup_error=errno;
    if(work->retained[0]) {
        struct stat named;
        work->retained_verified=!lstat(work->retained,&named)&&spec_same_inode(&named,&work->owned);
    }
}
static int64_t sp_now(void) {
    struct timespec value;if(clock_gettime(CLOCK_MONOTONIC,&value))return -1;
    return (int64_t)value.tv_sec*1000+value.tv_nsec/1000000;
}
static int sp_read_spool(int fd,const sp_options_t* options,sp_execution_t* execution,int stream) {
    struct stat before,after;int error=0;
    if(fstat(fd,&before))return -1;
    if(!S_ISREG(before.st_mode)||before.st_size<0){errno=EINVAL;return -1;}
    execution->seen[stream]=(uint64_t)before.st_size;
    size_t needed=options->quiet?0:(uint64_t)before.st_size<options->capture?(size_t)before.st_size:options->capture;
    execution->bytes[stream]=malloc(needed?needed:1u);if(!execution->bytes[stream])return -1;
    while(execution->kept[stream]<needed) {
        ssize_t size=pread(fd,execution->bytes[stream]+execution->kept[stream],needed-execution->kept[stream],(off_t)execution->kept[stream]);
        if(size<0&&errno==EINTR)continue;
        if(size<=0){error=size<0?errno:ESTALE;break;}execution->kept[stream]+=(size_t)size;
    }
    if(!error&&fstat(fd,&after))error=errno;
    if(!error&&!sp_same(&before,&after))error=ESTALE;
    execution->truncated[stream]=execution->seen[stream]>execution->kept[stream];
    if(error){errno=error;return -1;}return 0;
}
static int sp_execute(const sp_options_t* options,sp_workspace_t* work,sp_execution_t* execution) {
    int descriptors[2]={-1,-1};char paths[2][SPEC_MAX_PATH];const char* names[]={"stdout","stderr"};
    int error=0;const char* reason=NULL;
    for(int i=0;i<2;i++) {
        if(sp_path(paths[i],work->path,names[i])){error=errno;reason="spool_path_failed";break;}
        descriptors[i]=openat(work->root,names[i],O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(descriptors[i]<0){error=errno;reason="spool_open_failed";break;}
    }
    jash_execution_ir_t ir;jash_execution_ir_init(&ir);ir.stage_count=1;ir.stages[0].argc=(size_t)options->argc;
    for(int i=0;i<options->argc;i++)ir.stages[0].argv[i]=options->argv[i];
    jash_agent_process_options_t config;jash_agent_process_options_init(&config);
    config.cwd_fd=work->tree;config.background=1;config.stdout_path=paths[0];config.stderr_path=paths[1];
    config.output_limit_bytes=SP_SPOOL_LIMIT;config.spool_limit_only=1;config.cancel_grace_ms=50;
    jash_agent_process_t* process=NULL;char diagnostic[256];int64_t start=sp_now();
    if(!error&&start<0){error=errno?errno:EIO;reason="clock_failed";}
    if(!error) {
        execution->attempted=1;
        if(jash_agent_process_start(&ir,&config,&process,diagnostic,sizeof diagnostic)) {
            error=errno?errno:EIO;reason="supervisor_start_failed";
        } else {
            int waited=jash_agent_process_wait(process,options->timeout*1000);
            if(waited) {
                execution->timed_out=waited==1;error=waited==1?ETIMEDOUT:errno?errno:EIO;
                reason=waited==1?"command_timeout":"supervisor_wait_failed";
                if(jash_agent_process_cancel(process,config.cancel_grace_ms)&&!error){error=errno;reason="supervisor_cancel_failed";}
            }
            if(jash_agent_process_destroy_checked(process,&execution->process)&&!error) {
                error=execution->process.cleanup_error_number?execution->process.cleanup_error_number:errno?errno:EIO;
                reason="supervisor_cleanup_failed";
            }
            const jash_agent_process_result_t* result=&execution->process;
            if(!error&&result&&result->output_limit_hit){error=E2BIG;reason="supervisor_output_limit";}
            if(!error&&(!result||!result->terminal_status_valid||result->indeterminate||!result->quiescent||
               !result->descendants_quiescent||result->quiescence_forced||result->descendants_forced)) {
                error=EUCLEAN;reason="execution_quiescence_incomplete";
            }
            if(result&&result->quiescent&&result->descendants_quiescent) {
                for(int i=0;i<2;i++)if(sp_read_spool(descriptors[i],options,execution,i)&&!error){error=errno;reason="spool_read_failed";}
            }
        }
    }
    int64_t end=sp_now();
    if(start<0||end<start){if(!error){error=errno?errno:EIO;reason="clock_failed";}execution->elapsed_ms=-1;}
    else execution->elapsed_ms=end-start;
    for(int i=0;i<2;i++)if(descriptors[i]>=0&&close(descriptors[i])&&!error){error=errno;reason="spool_close_failed";}
    execution->number=error;execution->error=reason;execution->complete=!error;
    return error?-1:0;
}
static jdv_value_t sp_entry_value(semantic_value_t* sem,const sp_entry_t* entry) {
    if(!entry)return jdv_value_nothing();
    char hash[65];
    for(size_t i=0;i<32;i++)snprintf(hash+2u*i,3,"%02x",entry->hash[i]);
    jdv_record_input_t fields[]={
        semantic_field("kind",semantic_symbol(sem,sp_kind(entry->metadata.st_mode))),
        semantic_field("mode",jdv_value_u64((uint64_t)(entry->metadata.st_mode&07777))),
        semantic_field("size",S_ISREG(entry->metadata.st_mode)?jdv_value_filesize((uint64_t)entry->metadata.st_size):jdv_value_nothing()),
        semantic_field("blake3",entry->hashed?semantic_text(sem,hash):jdv_value_nothing()),
        semantic_field("link_target",entry->link?semantic_path(sem,entry->link):jdv_value_nothing()),
        semantic_field("device",S_ISCHR(entry->metadata.st_mode)||S_ISBLK(entry->metadata.st_mode)?jdv_value_u64((uint64_t)entry->metadata.st_rdev):jdv_value_nothing())
    };return semantic_record(sem,fields,6);
}
static jdv_value_t sp_change_value(semantic_value_t* sem,const sp_change_t* change) {
    jdv_record_input_t fields[]={
        semantic_field("schema",semantic_symbol(sem,"jash.speculate.v1")),semantic_field("kind",semantic_symbol(sem,"change")),
        semantic_field("path",semantic_path(sem,change->before?change->before->path:change->after->path)),
        semantic_field("change",semantic_symbol(sem,change->kind)),
        semantic_field("before",sp_entry_value(sem,change->before)),semantic_field("after",sp_entry_value(sem,change->after))
    };return semantic_record(sem,fields,6);
}
static jdv_value_t sp_plan_value(semantic_value_t* sem,const sp_options_t* options,const char* cwd,const sp_snapshot_t* snapshot) {
    jdv_value_t args[64],exclusions[32];size_t exclude_count=0;
    for(int i=0;i<options->argc;i++)args[i]=semantic_text(sem,options->argv[i]);
    for(size_t i=0;spec_skip_dirs[i];i++)exclusions[exclude_count++]=semantic_text(sem,spec_skip_dirs[i]);
    jdv_value_t* omissions=calloc(snapshot->omissions?snapshot->omissions:1u,sizeof(*omissions));
    if(!omissions){sem->error=1;return jdv_value_nothing();}size_t used=0;
    for(size_t i=0;i<snapshot->count;i++)if(snapshot->entries[i].omitted) {
        jdv_record_input_t fields[]={semantic_field("path",semantic_path(sem,snapshot->entries[i].path)),
            semantic_field("reason",semantic_symbol(sem,snapshot->entries[i].omission))};
        omissions[used++]=semantic_record(sem,fields,2);
    }
    jdv_record_input_t limits[]={
        semantic_field("entries_per_snapshot",jdv_value_u64(SP_ENTRIES)),semantic_field("depth",jdv_value_u64(SP_DEPTH)),
        semantic_field("path_bytes_per_snapshot",jdv_value_filesize(SP_PATH_BYTES)),semantic_field("file_bytes",jdv_value_filesize(SPEC_MAX_FILE_SIZE)),
        semantic_field("aggregate_file_bytes",jdv_value_filesize(SP_COPY_BYTES)),semantic_field("changes",jdv_value_u64(SP_CHANGES)),
        semantic_field("change_path_bytes",jdv_value_filesize(SP_CHANGE_PATH_BYTES)),semantic_field("symlink_target_bytes",jdv_value_filesize(SP_LINK_BYTES)),
        semantic_field("capture_bytes_per_stream",jdv_value_filesize(options->capture)),semantic_field("spool_bytes_polled_per_stream",jdv_value_filesize(SP_SPOOL_LIMIT)),
        semantic_field("spool_overshoot_possible",jdv_value_bool(1)),semantic_field("command_timeout_seconds",jdv_value_u64((uint64_t)options->timeout))
    };
    jdv_record_input_t isolation[]={
        semantic_field("kind",semantic_symbol(sem,"directory_copy")),semantic_field("filesystem_namespace",jdv_value_bool(0)),
        semantic_field("network",jdv_value_nothing()),semantic_field("environment",semantic_symbol(sem,"inherited")),
        semantic_field("argument_scope_guard",semantic_symbol(sem,"absolute_and_parent_path_arguments_refused_embedded_code_not_confined")),
        semantic_field("stdin",semantic_symbol(sem,"dev_null"))
    };
    jdv_record_input_t fields[]={
        semantic_field("schema",semantic_symbol(sem,"jash.speculate.v1")),semantic_field("kind",semantic_symbol(sem,"plan")),
        semantic_field("cwd",semantic_path(sem,cwd)),semantic_field("argv",semantic_list(sem,args,(size_t)options->argc)),
        semantic_field("source_inventory_complete",jdv_value_bool(snapshot->complete)),semantic_field("source_entries",jdv_value_u64(snapshot->count)),
        semantic_field("source_file_bytes",jdv_value_filesize(snapshot->file_bytes)),semantic_field("omissions",semantic_list(sem,omissions,used)),
        semantic_field("excluded_names",semantic_list(sem,exclusions,exclude_count)),
        semantic_field("copy_metadata_scope",semantic_symbol(sem,"regular_file_bytes_and_permission_bits_0777_directories_no_symlink_following")),
        semantic_field("diff_scope",semantic_symbol(sem,"copy_before_vs_copy_after_content_type_permissions_link_target_device_not_ownership_times_xattrs_hardlink_topology")),
        semantic_field("consistency",semantic_symbol(sem,"stable_metadata_samples_per_source_entry_not_atomic_filesystem_snapshot")),
        semantic_field("isolation",semantic_record(sem,isolation,6)),semantic_field("limits",semantic_record(sem,limits,12))
    };jdv_value_t value=semantic_record(sem,fields,14);free(omissions);return value;
}
static jdv_value_t sp_copy_value(semantic_value_t* sem,const sp_workspace_t* work,uint64_t bytes,const char* error,int number) {
    char tree[SPEC_MAX_PATH];int have_tree=work->created&&!sp_path(tree,work->path,"tree");
    jdv_record_input_t fields[]={
        semantic_field("schema",semantic_symbol(sem,"jash.speculate.v1")),semantic_field("kind",semantic_symbol(sem,"copy")),
        semantic_field("complete",jdv_value_bool(work->copied)),semantic_field("bytes_written",jdv_value_filesize(bytes)),
        semantic_field("tree_path",have_tree?semantic_path(sem,tree):jdv_value_nothing()),
        semantic_field("error",semantic_symbol(sem,error)),semantic_field("errno",number?jdv_value_i64(number):jdv_value_nothing())
    };return semantic_record(sem,fields,7);
}
static jdv_value_t sp_execution_value(semantic_value_t* sem,const sp_options_t* options,const sp_execution_t* execution) {
    jdv_value_t streams[2];
    for(int i=0;i<2;i++) {
        jdv_record_input_t fields[]={
            semantic_field("bytes",options->quiet||!execution->bytes[i]?jdv_value_nothing():semantic_bytes(sem,execution->bytes[i],execution->kept[i])),
            semantic_field("retained_bytes",jdv_value_filesize(execution->kept[i])),semantic_field("observed_bytes",jdv_value_filesize(execution->seen[i])),
            semantic_field("truncated",jdv_value_bool(execution->truncated[i])),semantic_field("omitted_by_quiet",jdv_value_bool(options->quiet)),
            semantic_field("observed",jdv_value_bool(execution->bytes[i]!=NULL))
        };streams[i]=semantic_record(sem,fields,6);
    }
    jdv_record_input_t fields[]={
        semantic_field("schema",semantic_symbol(sem,"jash.speculate.v1")),semantic_field("kind",semantic_symbol(sem,"execution")),
        semantic_field("attempted",jdv_value_bool(execution->attempted)),semantic_field("complete",jdv_value_bool(execution->complete)),
        semantic_field("command_status",execution->process.terminal_status_valid?jdv_value_i64(execution->process.exit_code):jdv_value_nothing()),
        semantic_field("timed_out",jdv_value_bool(execution->timed_out)),semantic_field("output_limit_hit",jdv_value_bool(execution->process.output_limit_hit)),
        semantic_field("quiescent",jdv_value_bool(execution->process.quiescent)),semantic_field("descendants_quiescent",jdv_value_bool(execution->process.descendants_quiescent)),
        semantic_field("quiescence_forced",jdv_value_bool(execution->process.quiescence_forced)),semantic_field("descendants_forced",jdv_value_bool(execution->process.descendants_forced)),
        semantic_field("cleanup_errno",execution->process.cleanup_error_number?jdv_value_i64(execution->process.cleanup_error_number):jdv_value_nothing()),
        semantic_field("indeterminate",jdv_value_bool(execution->process.indeterminate)),semantic_field("capture_scope",semantic_symbol(sem,"pinned_spool_prefix_preview")),
        semantic_field("stdout",streams[0]),semantic_field("stderr",streams[1]),
        semantic_field("elapsed_ms",execution->elapsed_ms<0?jdv_value_nothing():jdv_value_u64((uint64_t)execution->elapsed_ms)),
        semantic_field("error",semantic_symbol(sem,execution->error)),semantic_field("errno",execution->number?jdv_value_i64(execution->number):jdv_value_nothing())
    };return semantic_record(sem,fields,19);
}
static jdv_value_t sp_summary_value(semantic_value_t* sem,const sp_options_t* options,const sp_workspace_t* work,
        const sp_execution_t* execution,const sp_change_t* changes,size_t count,int diff_complete,const char* error,int number,int status) {
    uint64_t created=0,modified=0,deleted=0,type_changed=0;
    for(size_t i=0;i<count;i++) {
        created+=!strcmp(changes[i].kind,"created");modified+=!strcmp(changes[i].kind,"modified");
        deleted+=!strcmp(changes[i].kind,"deleted");type_changed+=!strcmp(changes[i].kind,"type_changed");
    }
    char tree[SPEC_MAX_PATH];int tree_known=work->retained_tree&&!sp_path(tree,work->retained,"tree");
    jdv_record_input_t cleanup[]={
        semantic_field("requested",jdv_value_bool(!options->keep)),
        semantic_field("candidate_path",work->created?semantic_path(sem,work->path):jdv_value_nothing()),
        semantic_field("state",semantic_symbol(sem,!work->created?"not_created":work->kept?"kept":work->removed?"removed":"retained")),
        semantic_field("root_path",work->retained_verified?semantic_path(sem,work->retained):jdv_value_nothing()),
        semantic_field("tree_path",tree_known?semantic_path(sem,tree):jdv_value_nothing()),
        semantic_field("retained_path_verified",jdv_value_bool(work->retained_verified)),
        semantic_field("errno",work->cleanup_error?jdv_value_i64(work->cleanup_error):jdv_value_nothing())
    };
    jdv_record_input_t fields[]={
        semantic_field("schema",semantic_symbol(sem,"jash.speculate.v1")),semantic_field("kind",semantic_symbol(sem,"summary")),
        semantic_field("complete",jdv_value_bool(!error)),semantic_field("copied",jdv_value_bool(work->copied)),
        semantic_field("changes_complete",jdv_value_bool(diff_complete)),
        semantic_field("changes_count",diff_complete?jdv_value_u64(count):jdv_value_nothing()),
        semantic_field("created_count",diff_complete?jdv_value_u64(created):jdv_value_nothing()),
        semantic_field("modified_count",diff_complete?jdv_value_u64(modified):jdv_value_nothing()),
        semantic_field("deleted_count",diff_complete?jdv_value_u64(deleted):jdv_value_nothing()),
        semantic_field("type_changed_count",diff_complete?jdv_value_u64(type_changed):jdv_value_nothing()),
        semantic_field("command_status",execution->process.terminal_status_valid?jdv_value_i64(execution->process.exit_code):jdv_value_nothing()),
        semantic_field("status",jdv_value_i64(status)),semantic_field("cleanup",semantic_record(sem,cleanup,7)),
        semantic_field("error",semantic_symbol(sem,error)),semantic_field("errno",number?jdv_value_i64(number):jdv_value_nothing())
    };return semantic_record(sem,fields,15);
}
typedef struct { int plan, copy, execution, changes_open; size_t changes; } sp_output_t;
static int sp_field(semantic_builtin_t* call,const char* name,const jdv_value_t* value) {
    jash_jsonw_string(&call->output.json,name);jash_jsonw_raw(&call->output.json,": ");
    if(call->values.error||jdv_json_write(&call->output.json,value,&call->values.limits))return 125;
    jash_jsonw_raw(&call->output.json,",");return 0;
}
static int sp_phase(semantic_builtin_t* call,sp_output_t* output,const char* phase,jdv_value_t value) {
    if(call->mode==2)return semantic_events_emit(call,value);
    if(!strcmp(phase,"plan")) {
        jash_jsonw_raw(&call->output.json,"{\"schema\":\"jash.speculate.v1\",");output->plan=1;
    }
    if(sp_field(call,phase,&value))return 125;
    if(!strcmp(phase,"copy"))output->copy=1;
    if(!strcmp(phase,"execution"))output->execution=1;
    return jash_jsonw_finish(&call->output.json)?125:0;
}
static int sp_change_emit(semantic_builtin_t* call,sp_output_t* output,jdv_value_t value) {
    if(call->mode==2)return semantic_events_emit(call,value);
    if(!output->changes_open){jash_jsonw_raw(&call->output.json,"\"changes\":[");output->changes_open=1;}
    if(output->changes)jash_jsonw_raw(&call->output.json,",");
    if(call->values.error||jdv_json_write(&call->output.json,&value,&call->values.limits))return 125;
    output->changes++;return jash_jsonw_finish(&call->output.json)?125:0;
}
static jdv_value_t sp_legacy_capture(semantic_value_t* sem,const sp_options_t* options,const sp_execution_t* execution,int stream) {
    if(options->quiet||!execution->bytes[stream])return jdv_value_nothing();
    for(size_t i=0;i<execution->kept[stream];i++) {
        uint8_t byte=execution->bytes[stream][i];
        if(byte<32&&byte!='\n'&&byte!='\r'&&byte!='\t')return semantic_bytes(sem,execution->bytes[stream],execution->kept[stream]);
    }
    return semantic_text_n(sem,execution->bytes[stream],execution->kept[stream]);
}
static int sp_json_end(semantic_builtin_t* call,sp_output_t* output,jdv_value_t summary,
        const sp_options_t* options,const sp_workspace_t* work,const sp_execution_t* execution,
        const sp_change_t* changes,size_t count,int diff_complete,int status) {
    if(!output->copy)jash_jsonw_raw(&call->output.json,"\"copy\":null,");
    if(!output->execution)jash_jsonw_raw(&call->output.json,"\"execution\":null,");
    jash_jsonw_raw(&call->output.json,output->changes_open?"],":"\"changes\":[],");
    if(sp_field(call,"summary",&summary))return 125;
    jdv_value_t* paths=calloc((count?count:1u)*3u,sizeof(*paths));
    if(!paths)return 125;
    size_t counts[3]={0,0,0};
    for(size_t i=0;i<count;i++) {
        size_t group=!strcmp(changes[i].kind,"created")?0u:!strcmp(changes[i].kind,"deleted")?2u:1u;
        paths[group*count+counts[group]++]=semantic_path(&call->values,changes[i].before?changes[i].before->path:changes[i].after->path);
    }
    jdv_value_t lists[3];for(size_t i=0;i<3;i++)lists[i]=semantic_list(&call->values,paths+i*count,counts[i]);free(paths);
    char tree[SPEC_MAX_PATH];int tree_known=work->retained_tree&&!sp_path(tree,work->retained,"tree");
    jdv_record_input_t fields[]={
        semantic_field("rc",execution->process.terminal_status_valid?jdv_value_i64(execution->process.exit_code):jdv_value_nothing()),
        semantic_field("elapsed_ms",execution->elapsed_ms<0?jdv_value_nothing():jdv_value_u64((uint64_t)execution->elapsed_ms)),
        semantic_field("sandbox",tree_known?semantic_path(&call->values,tree):work->retained_verified?semantic_path(&call->values,work->retained):jdv_value_nothing()),
        semantic_field("cleanup",semantic_symbol(&call->values,!work->created?"not_created":work->kept?"kept":work->removed?"removed":"retained")),
        semantic_field("created",lists[0]),semantic_field("modified",lists[1]),semantic_field("deleted",lists[2]),
        semantic_field("created_count",diff_complete?jdv_value_u64(counts[0]):jdv_value_nothing()),
        semantic_field("modified_count",diff_complete?jdv_value_u64(counts[1]):jdv_value_nothing()),
        semantic_field("deleted_count",diff_complete?jdv_value_u64(counts[2]):jdv_value_nothing()),
        semantic_field("timeout",jdv_value_u64((uint64_t)options->timeout)),
        semantic_field("stdout",sp_legacy_capture(&call->values,options,execution,0)),
        semantic_field("stderr",sp_legacy_capture(&call->values,options,execution,1))
    };
    for(size_t i=0;i<sizeof fields/sizeof fields[0];i++) {
        /* Field names above are static symbols; native values remain lossless. */
        char name[32];size_t size=fields[i].key.len;
        if(size>=sizeof name)return 125;
        memcpy(name,fields[i].key.data,size);name[size]=0;
        if(sp_field(call,name,&fields[i].value))return 125;
    }
    jash_jsonw_raw(&call->output.json,"\"network\": null}\n");
    return jash_structured_finish(&call->output,status);
}
static int sp_admission(semantic_builtin_t* call,const sp_options_t* options,const jdv_value_t* plan) {
    int status=semantic_preflight(call,plan);if(status)return status;
    value_output_measure_t measured={0,SEMANTIC_OUTPUT_BYTES};jash_json_writer_t writer;
    jash_jsonw_init(&writer,semantic_measure_output,&measured);
    if(jdv_json_write(&writer,plan,&call->values.limits)||jash_jsonw_finish(&writer))return 125;
    /* Every possible later record is bounded: known plan, <=1024 changes,
     * aggregate change paths and symlink targets, two retained byte prefixes,
     * and fixed copy/execution/summary/terminal overhead. JSON's six-byte
     * escapes dominate native paths; numeric/field overhead is reserved too. */
    size_t reserved=measured.bytes+16384u+SP_CHANGES*768u+
        (SP_CHANGE_PATH_BYTES*(call->mode==1?2u:1u)+SP_LINK_BYTES)*6u+options->capture*(call->mode==1?8u:4u);
    if(reserved>SEMANTIC_OUTPUT_BYTES)return 125;
    long budget=jash_budget_limit();
    return call->mode==1&&budget>0&&(uint64_t)budget<reserved?75:0;
}
static int sp_provider(semantic_builtin_t* call,jash_state_t* state,int argc,char** argv) {
    (void)state;sp_options_t options;int parsed=sp_parse(argc,argv,&options);
    if(parsed)return semantic_error(call,parsed==-2?"argument_outside_copy_scope":"invalid_arguments",2);
    call->values.limits.max_record_fields=16384;
    sp_snapshot_t before={0},after={0};sp_workspace_t work={.root=-1,.tree=-1};
    sp_execution_t execution={.elapsed_ms=-1};sp_change_t* changes=NULL;sp_output_t output={0};
    int source=-1,status=0,number=0,diff_complete=0,delivery_failed=0;
    const char* error=NULL;size_t count=0;uint64_t written=0;char cwd[SPEC_MAX_PATH]="";
    if(!getcwd(cwd,sizeof cwd)){number=errno;status=1;error="cwd_unavailable";}
    else if((source=open(".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC))<0){number=errno;status=1;error="source_open_failed";}
    else if(sp_scan(source,1,&before)){number=errno;status=number==E2BIG?75:1;error="source_inventory_failed";}
    if(!status) {
        changes=calloc(SP_CHANGES,sizeof(*changes));if(!changes){status=125;error="allocation_failed";}
    }
    if(!status) {
        jdv_value_t plan=sp_plan_value(&call->values,&options,cwd,&before);
        status=sp_admission(call,&options,&plan);
        if(status){error="receipt_admission_failed";goto finish;}
        if(semantic_admit_rows(call,SP_CHANGES+4u)||sp_phase(call,&output,"plan",plan)){status=125;delivery_failed=1;goto finish;}
        call->values.arena.used=0;call->values.error=0;
        if(sp_workspace_create(&work)){number=errno;status=1;error="copy_workspace_failed";}
        else if(sp_copy(source,work.tree,&before,&written)){number=errno;status=number==E2BIG?75:1;error="source_copy_failed";}
        else work.copied=1;
        if(sp_phase(call,&output,"copy",sp_copy_value(&call->values,&work,written,error,number))){status=125;delivery_failed=1;goto finish;}
        call->values.arena.used=0;call->values.error=0;
    }
    if(!status) {
        if(sp_execute(&options,&work,&execution)) {
            error=execution.error;number=execution.number;status=execution.timed_out||number==E2BIG?75:1;
        }
        if(sp_phase(call,&output,"execution",sp_execution_value(&call->values,&options,&execution))){status=125;delivery_failed=1;goto finish;}
        call->values.arena.used=0;call->values.error=0;
        if(execution.process.quiescent&&execution.process.descendants_quiescent) {
            int diff_error=0;
            if(sp_scan(work.tree,0,&after)||sp_changes(&before,&after,changes,&count))diff_error=errno;
            if(diff_error){if(!error){number=diff_error;error="copy_diff_failed";status=number==E2BIG?75:1;}count=0;}
            else {
                diff_complete=1;
                for(size_t i=0;i<count;i++) {
                    if(sp_change_emit(call,&output,sp_change_value(&call->values,&changes[i]))){status=125;delivery_failed=1;goto finish;}
                    call->values.arena.used=0;call->values.error=0;
                }
            }
        }
        if(!error)status=execution.process.exit_code;
    }
finish:
    sp_workspace_cleanup(&work,options.keep,!execution.process.launcher_pid||
        (execution.process.quiescent&&execution.process.descendants_quiescent));
    if(work.cleanup_error&&!delivery_failed&&!error){number=work.cleanup_error;status=1;error="cleanup_failed";}
    if(work.retained_verified&&work.root>=0&&work.tree>=0) {
        struct stat current,pinned;
        work.retained_tree=!fstatat(work.root,"tree",&current,AT_SYMLINK_NOFOLLOW)&&
            !fstat(work.tree,&pinned)&&S_ISDIR(current.st_mode)&&spec_same_inode(&current,&pinned);
    }
    int* descriptors[]={&source,&work.tree,&work.root};
    for(size_t i=0;i<3;i++)if(*descriptors[i]>=0) {
        if(close(*descriptors[i])&&!delivery_failed&&!error){number=errno;status=1;error="descriptor_close_failed";}*descriptors[i]=-1;
    }
    if(!delivery_failed) {
        if(error&&!strcmp(error,"receipt_admission_failed"))status=semantic_error(call,error,status);
        else {
            jdv_value_t value=sp_summary_value(&call->values,&options,&work,&execution,changes,count,diff_complete,error,number,status);
            jdv_stream_end_t outcome;(void)jdv_stream_end_init(&outcome);
            outcome.state=error?JDV_STREAM_FAILED:JDV_STREAM_COMPLETE;
            outcome.completeness=error?JDV_COMPLETENESS_PARTIAL:JDV_COMPLETENESS_COMPLETE;
            outcome.producer_status=status;outcome.errors_seen=error?1u:0u;
            if(error)outcome.reason=(jdv_slice_t){(const uint8_t*)error,strlen(error)};
            if(call->mode==1&&call->admitted)status=sp_json_end(call,&output,value,&options,&work,&execution,changes,count,diff_complete,status);
            else status=semantic_finish_outcome(call,value,&outcome);
        }
    }
    sp_snapshot_free(&before);sp_snapshot_free(&after);free(changes);
    free(execution.bytes[0]);free(execution.bytes[1]);return status;
}

static int sp_format_index(int argc,char* const* argv) {
    for(int i=0;i<argc;i++) {
        const char* value=argv[i];
        if(!strcmp(value,"--json")||!strcmp(value,"--data")||!strcmp(value,"--data-v2"))return i;
        if(!strcmp(value,"--keep")||!strcmp(value,"--quiet"))continue;
        if(!strcmp(value,"--timeout")||!strcmp(value,"--capture-bytes")) {
            if(++i==argc)return -1;
            unsigned number;
            if(sp_number(argv[i],!strcmp(value,"--timeout")?3600u:SP_CAPTURE_MAX,&number)||!number)return -1;
            continue;
        }
        return -1;
    }
    return -1;
}
static int sp_dispatch_argv(jash_state_t* state,jash_writer_t writer,void* context,int argc,char** argv) {
    int index=sp_format_index(argc,argv);if(index<0)return -1;
    char** normalized=calloc((size_t)argc+1u,sizeof(*normalized));if(!normalized)return 125;
    normalized[0]=argv[index];int used=1;
    for(int i=0;i<argc;i++)if(i!=index)normalized[used++]=argv[i];
    int result=semantic_dispatch_argv(state,writer,context,"speculate","jash.speculate.v1",argc,normalized,sp_provider);
    free(normalized);return result;
}
static int sp_legacy_dispatch(jash_state_t* state,jash_writer_t writer,void* context,const char* arguments) {
    jash_argv_t parsed={0};
    if(jash_argv_build(arguments?arguments:"",&parsed)<0)return -1;
    int result=sp_dispatch_argv(state,writer,context,parsed.argc,parsed.argv);
    jash_argv_free(&parsed);return result;
}
int jash_b_speculate_argv(jash_state_t* state,jash_writer_t writer,void* context,int argc,char** argv) {
    int result=sp_dispatch_argv(state,writer,context,argc,argv);if(result>=0)return result;
    char* arguments=semantic_legacy_join(argc,argv);if(!arguments)return 125;
    result=jash_b_speculate(state,writer,context,arguments);free(arguments);return result;
}
