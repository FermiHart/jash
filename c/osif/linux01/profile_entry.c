/* Author: F E R M I ∞ H A R T <contact@fermihart.com>
 * SPDX-License-Identifier: LicenseRef-SPL-1.0
 * Linux01 profile entry: canonical Jash editor, key decoder, renderer,
 * prompt/themes, tokenizer, history, completion and prediction. Platform
 * execution and the minimal registry are deliberately local to this OSIF.
 */
#include <port.h>
#include <readline/edit.h>
#include <readline/keymap.h>
#include <readline/render.h>
#include <theme/prompt.h>
#include <theme/theme.h>
#include <theme/banner.h>
#include <core/argv.h>
#include <core/history.h>
#include <core/autocomplete.h>
#include <core/predict.h>
#include <core/env.h>
#include <builtins/builtin.h>
#include "linux01_syscall.h"
#include "surface_native.h"
#include "predict_store.h"

static jash_state_t state;
static predict_table_t predictions;
static char **child_env;
static int want_exit, last_status;
static const char *const names[]={"cd","exit","pwd","echo","history","help","clear","true","false","version"};

/* Explicit profile capability providers: network AI/statusbar are unselected;
 * local prediction is the canonical core/predict implementation below. */
int jash_ai_active(void) { return 0; }
int jash_statusbar_enabled(const jash_state_t *s) { (void)s; return 0; }
void render_ai_mode_indicator(void) {}
const char *jash_env_get(const jash_state_t *s,const char *key)
{
    (void)s;
    if (!strcmp(key,"JASH_PROMPT_SPINNER")) return "0";
    if (!strcmp(key,"USER")) return "root";
    return getenv(key);
}
const jash_builtin_t *jash_builtin_lookup(const char *name)
{
    static const jash_builtin_t entries[]={
        {"cd",NULL,"directory",NULL},{"exit",NULL,"exit",NULL},
        {"pwd",NULL,"directory",NULL},{"echo",NULL,"text",NULL},
        {"history",NULL,"history",NULL},{"help",NULL,"help",NULL},
        {"clear",NULL,"clear",NULL},{"true",NULL,"success",NULL},
        {"false",NULL,"failure",NULL},{"version",jash_b_version,"version",NULL}};
    for (unsigned i=0;i<sizeof(entries)/sizeof(entries[0]);i++)
        if (!strcmp(name,entries[i].name)) return &entries[i];
    return NULL;
}
/* Raw input in this profile never contains expansion sentinels. Preserve the
 * canonical tokenizer's late-unshield contract for its six quote markers. */
void jash_quote_unshield(char *s)
{
    for (;s && *s;s++) {
        if (*s==3) *s='"'; else if (*s==4) *s='\'';
        else if (*s==5) *s='\\'; else if (*s==6) *s=' ';
    }
}
static long writer(void *ctx,const char *p,size_t n)
{
    int fd=ctx ? *(int *)ctx : 1; size_t done=0;
    while (done<n) {
        ssize_t r=write(fd,p+done,n-done);
        if (r<0 && errno==EINTR) continue;
        if (r<=0) return -1;
        done+=r;
    }
    return done;
}

struct termio01 { unsigned short i,o,c,l; unsigned char line,cc[8]; };
static struct termio01 saved;
static int raw;
static int begin_input(void)
{
    if(jash_linux01_surface_native_input()) {
        raw=jash_linux01_surface_editor_begin()==0;
        return raw?0:-1;
    }
    struct termio01 t;
    if (l01_ioctl(0,0x5405,&t)<0) return -1;
    saved=t; t.l &= ~(2|8|1); /* ICANON, ECHO, ISIG: editor owns control keys */
    t.cc[6]=1; t.cc[5]=0;
    raw=l01_ioctl(0,0x5406,&t)==0;
    return raw?0:-1;
}
static void end_input(void)
{
    if(raw) {
        if(jash_linux01_surface_native_input()) jash_linux01_surface_editor_end();
        else l01_ioctl(0,0x5406,&saved);
    }
    raw=0;
}

static const char *suggestion(const jash_line_t *line)
{
    if (!line->len || line->cur!=line->len) return NULL;
    const char *p=history_search(line->buf);
    if (!p) {
        char context[64]; predict_make_context(context,sizeof(context),state.cwd);
        p=predict_best(&predictions,context);
    }
    return p && !strncmp(p,line->buf,line->len) && p[line->len] ? p+line->len : NULL;
}
static void complete(jash_line_t *line)
{
    char partial[JASH_LINE_MAX];
    memcpy(partial,line->buf,line->cur); partial[line->cur]=0;
    unsigned start=line->cur;
    while (start && partial[start-1]!=' ' && partial[start-1]!='\t') start--;
    char *matches[64]; int n=0;
    if (!start) {
        for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)
            if (!strncmp(names[i],partial,line->cur)) matches[n++]=strdup(names[i]);
        n+=autocomplete_command(partial,matches+n,64-n);
        if (!n && strchr(partial,'/')) n=autocomplete_path(partial,matches,64);
    } else n=autocomplete_path(partial+start,matches,64);
    if (n==1 && matches[0]) {
        size_t prefix=line->cur-start;
        for (const char *p=matches[0]+prefix;*p;p++) jash_line_insert_byte(line,*p);
        size_t len=strlen(matches[0]);
        if (len && matches[0][len-1]!='/') jash_line_insert_byte(line,' ');
    } else if (n>1) {
        writer(NULL,"\n",1);
        for (int i=0;i<n;i++) if (matches[i]) { writer(NULL,matches[i],strlen(matches[i])); writer(NULL,"  ",2); }
        writer(NULL,"\n",1);
    }
    autocomplete_free(matches,n);
}
static int readline_profile(char *out)
{
    jash_line_t line; jash_line_reset(&line);
    jash_keydec_t keys; jash_keydec_init(&keys);
    if (begin_input()<0) {
        jash_prompt_render(writer,NULL,&state);
        char *r=fgets(out,JASH_LINE_MAX,stdin);
        if (!r) return -1;
        size_t n=strlen(out); if (n && out[n-1]=='\n') out[n-1]=0;
        return 0;
    }
    jash_prompt_render(writer,NULL,&state);
    for (;;) {
        unsigned char ch; ssize_t n=read(0,&ch,1);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { end_input(); return -1; }
        int key=jash_keydec_feed(&keys,ch);
        if (!key) continue;
        const char *text;
        switch (key) {
        case JK_ENTER:
            jash_render_line(writer,NULL,&state,&line);
            writer(NULL,"\n",1); end_input();
            memcpy(out,line.buf,line.len+1); return 0;
        case 3: jash_line_reset(&line); writer(NULL,"^C\n",3); break;
        case 4: if (!line.len) { end_input(); return -1; } jash_line_delete(&line); break;
        case JK_BACKSPACE: jash_line_backspace(&line); break;
        case JK_DELETE: jash_line_delete(&line); break;
        case JK_LEFT: jash_line_left(&line); break;
        case JK_RIGHT:
            text=suggestion(&line);
            if (text && line.cur==line.len) { char copy[JASH_LINE_MAX]; strlcpy(copy,text,sizeof(copy)); for (char *p=copy;*p;p++) jash_line_insert_byte(&line,*p); }
            else jash_line_right(&line);
            break;
        case JK_HOME: case 1: jash_line_home(&line); break;
        case JK_END: case 5: jash_line_end(&line); break;
        case JK_UP: text=history_prev(); if (text) jash_line_replace(&line,text); break;
        case JK_DOWN: text=history_next(); if (text) jash_line_replace(&line,text); break;
        case 11: jash_line_kill_eol(&line); break;
        case 21: jash_line_kill_bol(&line); break;
        case 23: jash_line_kill_word(&line); break;
        case 25: jash_line_yank(&line); break;
        case 9: complete(&line); break;
        default: if (key>=32 && key<127) jash_line_insert_byte(&line,(char)key); break;
        }
        jash_render_line_suggest(writer,NULL,&state,&line,suggestion(&line));
    }
}

/* Operators are recognized before canonical argv unquoting; quoted |<> are
 * data. Eight stages, <=30 arguments, explicit syntax errors. */
typedef struct { jash_argv_t words; char *input,*output; int append; } command;
static void release_commands(command *c,int n)
{ for(int i=0;i<n;i++){ jash_argv_free(&c[i].words); free(c[i].input); free(c[i].output); } }
static int word_end(const char *s,int start)
{
    char quote=0; int i=start;
    for (;s[i];i++) {
        char c=s[i];
        if (c=='\\' && quote!='\'' && s[i+1]) { i++; continue; }
        if (quote) { if (c==quote) quote=0; continue; }
        if (c=='\'' || c=='"') { quote=c; continue; }
        if (c==' ' || c=='\t' || c=='|' || c=='<' || c=='>' || c==';' || c=='&') break;
    }
    return quote?-1:i;
}
static int parse_line(const char *s,command commands[8])
{
    char text[JASH_LINE_MAX]; int stage=0,len=0,i=0;
    memset(commands,0,8*sizeof(*commands));
    for (;;) {
        while(s[i]==' ' || s[i]=='\t') i++;
        char c=s[i];
        if (!c || c=='|') {
            text[len]=0;
            if (jash_argv_build(text,&commands[stage].words)<=0 || commands[stage].words.argc>30) goto error;
            if (!c) return stage+1;
            if (++stage==8) goto error;
            len=0; i++; continue;
        }
        if (c==';' || c=='&') goto error;
        if (c=='<' || c=='>') {
            int append=c=='>' && s[i+1]=='>';
            i+=1+append; while(s[i]==' ' || s[i]=='\t') i++;
            int end=word_end(s,i);
            if (end<=i) goto error;
            char token[JASH_LINE_MAX]; memcpy(token,s+i,end-i); token[end-i]=0;
            jash_argv_t arg;
            if (jash_argv_build(token,&arg)!=1) { jash_argv_free(&arg); goto error; }
            char **dst=c=='<'?&commands[stage].input:&commands[stage].output;
            free(*dst); *dst=strdup(arg.argv[0]); jash_argv_free(&arg);
            if (!*dst) goto error;
            if (c=='>') commands[stage].append=append;
            i=end; continue;
        }
        int end=word_end(s,i);
        if (end<=i || len+end-i+2>JASH_LINE_MAX) goto error;
        if (len) text[len++]=' ';
        memcpy(text+len,s+i,end-i); len+=end-i; i=end;
    }
error:
    release_commands(commands,8); return -1;
}
static int redirect(command *c)
{
    for (int target=0;target<2;target++) {
        const char *p=target?c->output:c->input;
        if (!p) continue;
        int fd=open(p,target?(O_WRONLY|O_CREAT|(c->append?O_APPEND:O_TRUNC)):O_RDONLY,0644);
        if (fd<0) { fprintf(stderr,"jash: cannot open %s\n",p); return -1; }
        if (fd!=target) { long r=l01_dup2(fd,target); close(fd); if (r<0) return -1; }
    }
    return 0;
}
static int builtin(int argc,char **argv)
{
    const char *name=argv[0];
    if (!strcmp(name,"cd")) {
        const char *p=argc>1?argv[1]:getenv("HOME"); if (!p) p="/";
        if(chdir(p)<0 || !getcwd(state.cwd,sizeof(state.cwd))) { fprintf(stderr,"cd: %s: failed\n",p); return 1; }
        return 0;
    }
    if (!strcmp(name,"pwd")) { printf("%s\n",state.cwd); return 0; }
    if (!strcmp(name,"echo")) {
        for (int i=1;i<argc;i++) printf("%s%s",i>1?" ":"",argv[i]);
        printf("\n"); return 0;
    }
    if (!strcmp(name,"history")) {
        for (int i=0;i<history_count();i++) printf("%d  %s\n",i+1,history_get(i));
        return 0;
    }
    if (!strcmp(name,"help")) {
        printf("Jash Linux01: 10 builtins: cd exit pwd echo history help clear true false version\n");
        printf("Canonical editor, prompt, history, completion and local predictions; Bear libc i386.\n");
        printf("External commands: /bin via PATH; 8 pipeline stages; < > >>; quoted arguments.\n");
        return 0;
    }
    if (!strcmp(name,"clear")) { writer(NULL,"\033[2J\033[H",7); return 0; }
    if (!strcmp(name,"true")) return 0;
    if (!strcmp(name,"false")) return 1;
    if (!strcmp(name,"version")) return jash_b_version(&state,writer,NULL,"");
    if (!strcmp(name,"exit")) {
        int code=last_status;
        if (argc>1) { code=0; for(const char*p=argv[1];*p;p++) { if(*p<'0'||*p>'9') return 2; code=(code*10+*p-'0')&255; } }
        want_exit=1; return code;
    }
    return -1;
}
static void execute_external(char **argv)
{
    long r;
    if (strchr(argv[0],'/')) r=l01_execve(argv[0],argv,child_env);
    else {
        const char *path=getenv("PATH"); if(!path) path="/bin";
        r=-2;
        while (*path) {
            const char *end=strchr(path,':'); size_t n=end?(size_t)(end-path):strlen(path);
            char full[512];
            if (n+strlen(argv[0])+2<sizeof(full)) {
                memcpy(full,path,n); full[n]=0;
                if (n) strlcat(full,"/",sizeof(full));
                strlcat(full,argv[0],sizeof(full));
                r=l01_execve(full,argv,child_env);
                if(r!=-2 && r!=-20) break;
            }
            if (!end) break;
            path=end+1;
        }
    }
    fprintf(stderr,"%s: %s\n",argv[0],r==-2?"not found":"cannot execute");
    exit(r==-2?127:126);
}
static int execute(command *c,int n)
{
    if(n==1 && jash_builtin_lookup(c[0].words.argv[0])) {
        int backup[2]={-1,-1},rc=1;
        if(c[0].input) backup[0]=l01_dup(0);
        if(c[0].output) backup[1]=l01_dup(1);
        if((!c[0].input || backup[0]>=0) && (!c[0].output || backup[1]>=0) && !redirect(c))
            rc=builtin(c[0].words.argc,c[0].words.argv);
        for(int i=0;i<2;i++) if(backup[i]>=0){l01_dup2(backup[i],i);close(backup[i]);}
        return rc;
    }
    int previous=-1,pids[8],count=0,rc=1,group=0;
    for(int i=0;i<n;i++) {
        int fds[2]={-1,-1};
        int ready[2]={-1,-1};
        if(i+1<n && l01_pipe(fds)<0) break;
        if(jash_linux01_surface_native_output() && l01_pipe(ready)<0) {
            if(fds[0]>=0) close(fds[0]);
            if(fds[1]>=0) close(fds[1]);
            break;
        }
        long pid=l01_fork();
        if(pid<0){ if(fds[0]>=0) close(fds[0]); if(fds[1]>=0) close(fds[1]); if(ready[0]>=0) close(ready[0]); if(ready[1]>=0) close(ready[1]); break; }
        if(!pid) {
            if(ready[0]>=0) {
                char go;
                close(ready[1]); l01_setpgid(0,group);
                if(l01_read(ready[0],&go,1)!=1) exit(1);
                close(ready[0]);
            }
            if(previous>=0 && previous!=0 && l01_dup2(previous,0)<0) exit(1);
            if(fds[1]>=0 && fds[1]!=1 && l01_dup2(fds[1],1)<0) exit(1);
            if(previous>1) close(previous);
            if(fds[0]>1) close(fds[0]);
            if(fds[1]>1) close(fds[1]);
            if(redirect(&c[i])) exit(1);
            int r=builtin(c[i].words.argc,c[i].words.argv);
            if(r>=0) exit(r);
            execute_external(c[i].words.argv);
        }
        pids[count++]=pid;
        if(jash_linux01_surface_native_output()) {
            if(!group) group=pid;
            l01_setpgid(pid,group);
            jash_linux01_surface_foreground(group);
            close(ready[0]); l01_write(ready[1],"g",1); close(ready[1]);
        }
        if(previous>=0) close(previous);
        if(fds[1]>=0) close(fds[1]);
        previous=fds[0];
    }
    if(previous>=0) close(previous);
    for(int i=0;i<count;i++) {
        int status; long r;
        do {r=l01_waitpid(pids[i],&status,0);} while(r==-EINTR);
        if(i==n-1 && r>=0) rc=(status&127)?128+(status&127):(status>>8)&255;
    }
    if(count<n) fprintf(stderr,"jash: process/pipe allocation failed\n");
    if(jash_linux01_surface_native_output()) jash_linux01_surface_foreground((int)l01_trap0(65));
    return rc;
}
static void login_message(void)
{
    FILE *f=fopen("/etc/motd","r");
    if(!f) return;
    char buf[512]; size_t n;
    while((n=fread(buf,1,sizeof(buf),f)))
        if(writer(NULL,buf,n)<0) break;
    fclose(f);
    writer(NULL,"\n",1);
}

int main(int argc,char **argv,char **envp)
{
    child_env=envp; bear_port_init(envp);
    if(argc<2 || strcmp(argv[1],"--tty")) jash_linux01_surface_attach();
    if(!getcwd(state.cwd,sizeof(state.cwd))) strlcpy(state.cwd,"/",sizeof(state.cwd));
    jash_theme_init(&state); history_init();
    predict_init(&predictions,NULL);
    snprintf(predictions.persist_path,sizeof(predictions.persist_path),"%s/.jash_predict",getenv("HOME")?getenv("HOME"):"/tmp");
    if(linux01_predict_load(&predictions)<0) fprintf(stderr,"jash: invalid prediction file ignored\n");
    if(argc>0 && argv[0] && argv[0][0]=='-') login_message();
    jash_banner_short(writer,NULL);
    printf("Jash Linux01 — canonical profile / Bear libc i386 / %s\n",
           jash_linux01_surface_native_output()?"native surface":"TTY");
    unsigned command_index=0;
    while(!want_exit) {
        char line[JASH_LINE_MAX];
        state.last_status.code=last_status;
        if(readline_profile(line)<0) break;
        if(!line[0]) continue;
        history_add(line); history_save();
        command commands[8]; int n=parse_line(line,commands);
        if(n<0) { fprintf(stderr,"jash: syntax error\n"); last_status=2; continue; }
        last_status=execute(commands,n); release_commands(commands,n);
        jash_linux01_surface_receipt(++command_index,last_status);
        if(!last_status) {char context[64]; predict_make_context(context,sizeof(context),state.cwd); predict_record(&predictions,line,context); if(linux01_predict_save(&predictions)<0) fprintf(stderr,"jash: cannot persist predictions\n");}
    }
    history_save(); linux01_predict_save(&predictions); l01_sync();
    return last_status;
}
