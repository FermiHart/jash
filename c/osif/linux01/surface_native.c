/* Author: F E R M I ∞ H A R T <contact@fermihart.com>
 * SPDX-License-Identifier: LicenseRef-SPL-1.0
 * Adapted from canonical Jash c/osif/tina/surface.c. Keeps the actual
 * TinaLinux probe, policy, presentation, endpoint, viewport and cell API.
 * Differences: i386 runtime; no x86-64 ucontext crash hook; endpoint attach;
 * bounded progress length; native status receipts.
 */
#include "surface_native.h"
#include <linux/tina_surface.h>
#include <port.h>
#include "linux01_syscall.h"

static int native_output,foreground_owner;
static unsigned seat;
int jash_linux01_surface_probe(void)
{
    struct surface_info_t info={0};
    native_output=foreground_owner=0;
    if(ioctl(1,SURF_GET_INFO,&info) || info.magic!=SURF_MAGIC || info.version!=2) return 0;
    seat=(unsigned)info.surf_id;
    native_output=(info.capabilities & (SURF_CAP_NATIVE_CELLS|SURF_CAP_PRESENTATION|SURF_CAP_INPUT_POLICY)) ==
        (SURF_CAP_NATIVE_CELLS|SURF_CAP_PRESENTATION|SURF_CAP_INPUT_POLICY);
    if(native_output) {
        struct surf_stat_t st={0};
        if(!ioctl(1,SURF_GET_STAT,&st)) foreground_owner=st.owner_pid==(int)getpid();
    }
    return native_output;
}
int jash_linux01_surface_native_output(void) { return native_output; }
int jash_linux01_surface_foreground_owner(void) { return foreground_owner; }
int jash_linux01_surface_native_input(void)
{
    struct surf_endpoint_t endpoint={0};
    if(!native_output || ioctl(0,SURF_GET_ENDPOINT,&endpoint)) return 0;
    return endpoint.version==SURF_ENDPOINT_VERSION && endpoint.size==sizeof(endpoint) &&
        endpoint.surf_id==seat && endpoint.ring==SURF_PROV_RING_IN;
}
static int input_actions(unsigned flags)
{
    struct surf_input_policy_v1 policy={.version=SURF_NATIVE_VERSION,.size=sizeof(policy)};
    if(!jash_linux01_surface_native_input() || ioctl(0,SURF_GET_INPUT_POLICY,&policy) ||
       policy.version!=SURF_NATIVE_VERSION || policy.size!=sizeof(policy)) return -1;
    if(policy.flags==flags) return 0;
    policy.flags=flags;
    return ioctl(0,SURF_SET_INPUT_POLICY,&policy);
}
int jash_linux01_surface_editor_end(void) { return input_actions(SURF_INPUT_ACTION_KEYS); }
int jash_linux01_surface_foreground(int pgid)
{
    struct surf_foreground_t fg={.pgid=pgid,.reserved=0};
    if(!native_output || pgid<=0) return -1;
    return ioctl(1,SURF_SET_FOREGROUND,&fg);
}
int jash_linux01_surface_editor_begin(void)
{
    struct surf_presentation_v1 p={.version=SURF_NATIVE_VERSION,.size=sizeof(p)};
    if(input_actions(0) || !native_output || ioctl(1,SURF_GET_PRESENTATION,&p) ||
       p.version!=SURF_NATIVE_VERSION || p.size!=sizeof(p)) return -1;
    if(p.flags==SURF_PRESENT_NL_CRLF) return 0;
    p.flags=SURF_PRESENT_NL_CRLF;
    return ioctl(1,SURF_SET_PRESENTATION,&p);
}
int jash_linux01_surface_viewport(unsigned *columns,unsigned *rows)
{
    struct surf_viewport_t v={0};
    if(!native_output || (!columns && !rows) || ioctl(1,SURF_GET_VIEWPORT,&v) ||
       v.version!=SURF_VIEWPORT_VERSION || v.size!=sizeof(v) || !v.columns || !v.rows) return -1;
    if(columns) *columns=v.columns;
    if(rows) *rows=v.rows;
    return 0;
}
static void emit(unsigned type,const char *data,size_t len)
{
    if(!native_output || !data) return;
    struct bloom_cell_t cell={0}; cell.type=type;
    if(len>BLOOM_CELL_DATA_MAX) len=BLOOM_CELL_DATA_MAX;
    cell.len=len; memcpy(cell.data,data,len);
    ioctl(1,SURF_EMIT_CELL,&cell);
}
void jash_linux01_surface_error(const char *data,size_t len) { emit(BLOOM_CELL_ERROR,data,len); }
void jash_linux01_surface_progress(unsigned current,unsigned total,const char *label)
{
    char buf[128]; int n=snprintf(buf,sizeof(buf),"%u/%u %s",current,total,label?label:"");
    if(n>0) emit(BLOOM_CELL_PROGRESS,buf,(size_t)n<sizeof(buf)?(size_t)n:sizeof(buf)-1);
}
void jash_linux01_surface_receipt(unsigned index,int status)
{
    char buf[64]; int n=snprintf(buf,sizeof(buf),"jash.status index=%u status=%d",index,status);
    if(n>0) emit(BLOOM_CELL_KV,buf,(size_t)n<sizeof(buf)?(size_t)n:sizeof(buf)-1);
}
int jash_linux01_surface_attach(void)
{
    if(!isatty(0) || !isatty(1)) return jash_linux01_surface_probe();
    int out=open("/dev/surface0",O_WRONLY);
    if(out<0) return 0;
    struct surface_info_t info={0};
    if(ioctl(out,SURF_GET_INFO,&info) || info.magic!=SURF_MAGIC) {close(out); return 0;}
    int in=open("/dev/surfin0",O_RDONLY);
    if(in<0) {close(out); return 0;}
    int backup[3];
    for(int i=0;i<3;i++) {
        backup[i]=l01_dup(i);
        if(backup[i]<0) {while(i--) close(backup[i]); close(in); close(out); return 0;}
    }
    int ok=l01_dup2(in,0)>=0 && l01_dup2(out,1)>=0 && l01_dup2(out,2)>=0;
    if(!ok) for(int i=0;i<3;i++) l01_dup2(backup[i],i);
    for(int i=0;i<3;i++) close(backup[i]);
    close(in); close(out);
    if(!ok) return 0;
    l01_setsid(); /* private foreground group: Ctrl-C must not hit parent shell */
    int found=jash_linux01_surface_probe();
    if(found) jash_linux01_surface_foreground((int)l01_trap0(65));
    return found;
}
