/* Author: F E R M I ∞ H A R T <contact@fermihart.com>
 * SPDX-License-Identifier: LicenseRef-SPL-1.0
 * Linux01 platform half of libblibc-linux01.a. Portable string/memory and
 * formatting engines are compiled from the content-pinned Bear sources.
 */
#include "include/port.h"
#include "linux01_syscall.h"
#include <sys/utsname.h>
#undef printf
#include "io/fmt/fmt.h"
#define printf bear_printf

int bear_errno;
static char **environment;
static FILE standard_files[3] = {{0,0,0},{1,0,0},{2,0,0}};
FILE *stdin = standard_files, *stdout = standard_files + 1, *stderr = standard_files + 2;

void bear_port_init(char **envp) { environment = envp; }
char *getenv(const char *key)
{
    size_t n = strlen(key);
    for (char **p = environment; p && *p; p++)
        if (!strncmp(*p,key,n) && (*p)[n]=='=') return *p+n+1;
    return NULL;
}
static long result(long r) { if (r < 0) { errno = -r; return -1; } return r; }
static int valid_path(const char *p)
{
    size_t n=0;
    if(!p) {errno=14;return 0;}
    for(;*p;p++) {
        if(*p=='/') n=0;
        else if(++n>14) {errno=ENAMETOOLONG;return 0;}
    }
    return 1;
}
ssize_t read(int fd, void *p, size_t n) { return result(l01_read(fd,p,n)); }
ssize_t write(int fd, const void *p, size_t n) { return result(l01_write(fd,p,n)); }
int close(int fd) { return result(l01_close(fd)); }
int open(const char *p, int f, ...)
{
    if(!valid_path(p)) return -1;
    int mode = 0;
    if (f & O_CREAT) { va_list ap; va_start(ap,f); mode=va_arg(ap,int); va_end(ap); }
    int fd = result(l01_open(p,f & ~O_CLOEXEC,mode));
    if (fd >= 0 && (f & O_CLOEXEC) && l01_fcntl(fd,F_SETFD,FD_CLOEXEC)<0) {
        close(fd); errno=EIO; return -1;
    }
    return fd;
}
int stat(const char *p, struct stat *s) { return valid_path(p)?result(l01_stat(p,s)):-1; }
int fstat(int fd, struct stat *s) { return result(l01_trap2(28,fd,(long)s)); }
int chdir(const char *p) { return valid_path(p)?result(l01_chdir(p)):-1; }
int mkdir(const char *p, mode_t mode) { return valid_path(p)?result(l01_mkdir(p,mode)):-1; }
int unlink(const char *p) { return valid_path(p)?result(l01_unlink(p)):-1; }
off_t lseek(int fd, off_t o, int w) { return result(l01_lseek(fd,o,w)); }
int ioctl(int fd, unsigned long op, void *p) { return result(l01_ioctl(fd,op,p)); }
int fcntl(int fd, int op, ...)
{
    long arg=0;
    if (op != F_GETFD && op != F_GETFL) {
        va_list ap; va_start(ap,op); arg=va_arg(ap,int); va_end(ap);
    }
    return result(l01_fcntl(fd,op,arg));
}
pid_t getpid(void) { return l01_getpid(); }
int uname(struct utsname *u) { return result(l01_uname(u)); }
int isatty(int fd) { unsigned char buf[36]; return l01_ioctl(fd,0x5401,buf)==0; }
void exit(int code) { l01_exit(code); for (;;) ; }

/* A bounded brk arena with reusable/coalesced blocks. No mmap emulation.
 * Header and returned pointers are 16-byte aligned. Overflow rejects before
 * touching brk; allocation failure leaves the previous block valid. */
typedef struct block { size_t size; struct block *next; unsigned magic, used; } block;
static block *blocks;
static uintptr_t heap_begin, heap_end;
#define HEAP_LIMIT (2UL * 1024 * 1024)
#define BLOCK_MAGIC 0xB1BC0032u
void *malloc(size_t n)
{
    if (!n) n=1;
    if (n > HEAP_LIMIT-sizeof(block)-15) { errno=ENOMEM; return NULL; }
    n=(n+15)&~(size_t)15;
    block *last=NULL;
    for (block *b=blocks; b; last=b,b=b->next) {
        if (!b->used && b->size >= n) {
            if (b->size >= n+sizeof(block)+16) {
                block *tail=(block *)((char *)(b+1)+n);
                *tail=(block){b->size-n-sizeof(block), b->next, BLOCK_MAGIC,0};
                b->next=tail; b->size=n;
            }
            b->used=1; return b+1;
        }
    }
    if (!heap_begin) heap_begin=heap_end=((uintptr_t)l01_brk(NULL)+15)&~15UL;
    size_t amount=sizeof(block)+n;
    if (heap_end < heap_begin || heap_end-heap_begin > HEAP_LIMIT-amount) {
        errno=ENOMEM; return NULL;
    }
    uintptr_t next=heap_end+amount;
    if ((uintptr_t)l01_brk((void *)next)!=next) { errno=ENOMEM; return NULL; }
    block *b=(block *)heap_end;
    *b=(block){n,NULL,BLOCK_MAGIC,1};
    if (last) last->next=b; else blocks=b;
    heap_end=next;
    return b+1;
}
void free(void *p)
{
    if (!p) return;
    block *b=blocks;
    while (b && b+1!=p) b=b->next;
    if (!b || b->magic!=BLOCK_MAGIC || !b->used) exit(134);
    b->used=0;
    for (b=blocks; b && b->next;) {
        if (!b->used && !b->next->used) {
            b->size+=sizeof(block)+b->next->size;
            b->next=b->next->next;
        } else b=b->next;
    }
}
void *calloc(size_t n,size_t size)
{
    if (size && n > (size_t)-1/size) { errno=ENOMEM; return NULL; }
    void *p=malloc(n*size); if (p) memset(p,0,n*size); return p;
}
void *realloc(void *p,size_t n)
{
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    block *b=blocks;
    while (b && b+1!=p) b=b->next;
    if (!b || !b->used) exit(134);
    if (n<=b->size) return p;
    void *q=malloc(n);
    if (q) { memcpy(q,p,b->size); free(p); }
    return q;
}

DIR *opendir(const char *path)
{
    struct stat st;
    if (stat(path,&st)<0) return NULL;
    if (!S_ISDIR(st.st_mode)) { errno=20; return NULL; }
    int fd=open(path,O_RDONLY);
    if (fd<0) return NULL;
    DIR *dir=calloc(1,sizeof(*dir));
    if (!dir) { close(fd); return NULL; }
    dir->fd=fd; return dir;
}
struct dirent *readdir(DIR *dir)
{
    struct { unsigned short ino; char name[14]; } raw;
    for (;;) {
        long n=read(dir->fd,&raw,sizeof(raw));
        if (!n) return NULL;
        if (n<0 && errno==EINTR) continue;
        if (n!=sizeof(raw)) { errno=EIO; return NULL; }
        if (!raw.ino) continue;
        dir->entry.d_ino=raw.ino;
        memcpy(dir->entry.d_name,raw.name,14); dir->entry.d_name[14]=0;
        return &dir->entry;
    }
}
int closedir(DIR *d) { int r=close(d->fd); free(d); return r; }
char *getcwd(char *out,size_t cap)
{
    char path[512]; size_t pos=sizeof(path)-1; path[pos]=0;
    int failed=0;
    for (unsigned depth=0; depth<32; depth++) {
        struct stat here,parent;
        if (stat(".",&here)<0 || stat("..",&parent)<0) { failed=1; break; }
        if (here.st_ino==parent.st_ino && here.st_dev==parent.st_dev) break;
        DIR *dir=opendir("..");
        if (!dir) { failed=1; break; }
        char name[15]={0}; struct dirent *de;
        while ((de=readdir(dir)))
            if (de->d_ino==here.st_ino && strcmp(de->d_name,".") && strcmp(de->d_name,"..")) {
                strlcpy(name,de->d_name,sizeof(name)); break;
            }
        closedir(dir);
        size_t n=strlen(name);
        if (!n || n+1>pos || chdir("..")<0) { failed=1; break; }
        pos-=n; memcpy(path+pos,name,n); path[--pos]='/';
        if (depth==31) failed=1;
    }
    if (pos==sizeof(path)-1) path[--pos]='/';
    /* At root on success; from a partial ancestor on failure. */
    const char *restore=failed ? path+pos+1 : path+pos;
    if (*restore && chdir(restore)<0) { errno=EIO; return NULL; }
    if (failed || !out || strlen(path+pos)>=cap) { errno=ENAMETOOLONG; return NULL; }
    strlcpy(out,path+pos,cap); return out;
}

static int fd_sink(void *ctx,const char *p,size_t n)
{
    FILE *f=ctx;
    while (n) {
        ssize_t w=write(f->fd,p,n);
        if (w<0 && errno==EINTR) continue;
        if (w<=0) { f->error=1; return -1; }
        p+=w; n-=w;
    }
    return 0;
}
typedef struct { char *p; size_t cap, length; } text_sink;
static int buffer_sink(void *ctx,const char *p,size_t n)
{
    text_sink *b=ctx;
    size_t room=b->cap && b->length<b->cap-1 ? b->cap-1-b->length : 0;
    size_t copy=room<n?room:n;
    if (copy) memcpy(b->p+b->length,p,copy);
    b->length+=n; return 0;
}
int vsnprintf(char *p,size_t cap,const char *fmt,va_list ap)
{
    text_sink b={p,cap,0};
    int r=bear_vformat(buffer_sink,&b,fmt,ap);
    if (cap) p[b.length<cap?b.length:cap-1]=0;
    return r;
}
int snprintf(char *p,size_t cap,const char *fmt,...)
{
    va_list ap; va_start(ap,fmt); int r=vsnprintf(p,cap,fmt,ap); va_end(ap); return r;
}
int printf(const char *fmt,...)
{
    va_list ap; va_start(ap,fmt); int r=bear_vformat(fd_sink,stdout,fmt,ap); va_end(ap); return r;
}
int fprintf(FILE *f,const char *fmt,...)
{
    va_list ap; va_start(ap,fmt); int r=bear_vformat(fd_sink,f,fmt,ap); va_end(ap); return r;
}
FILE *fopen(const char *path,const char *mode)
{
    int flags;
    if (!strcmp(mode,"r")) flags=O_RDONLY;
    else if (!strcmp(mode,"w")) flags=O_WRONLY|O_CREAT|O_TRUNC;
    else if (!strcmp(mode,"a")) flags=O_WRONLY|O_CREAT|O_APPEND;
    else { errno=EINVAL; return NULL; }
    int fd=open(path,flags,0600);
    if (fd<0) return NULL;
    FILE *f=calloc(1,sizeof(*f));
    if (!f) { close(fd); return NULL; }
    f->fd=fd; return f;
}
int fclose(FILE *f) { int r=close(f->fd); free(f); return r; }
int fflush(FILE *f) { return f && f->error ? -1 : 0; }
char *fgets(char *p,int cap,FILE *f)
{
    if (cap<2) { errno=EINVAL; return NULL; }
    int i=0;
    while (i<cap-1) {
        ssize_t n=read(f->fd,p+i,1);
        if (n<0 && errno==EINTR) continue;
        if (n<0) { f->error=1; return NULL; }
        if (!n) { f->eof=1; break; }
        if (p[i++]=='\n') break;
    }
    p[i]=0; return i?p:NULL;
}
size_t fwrite(const void *p,size_t size,size_t n,FILE *f)
{
    if (!size || n>(size_t)-1/size) return 0;
    return fd_sink(f,p,size*n) ? 0 : n;
}
size_t fread(void *p,size_t size,size_t n,FILE *f)
{
    if (!size || n>(size_t)-1/size) return 0;
    size_t off=0,total=size*n;
    while (off<total) {
        ssize_t r=read(f->fd,(char *)p+off,total-off);
        if (r<0 && errno==EINTR) continue;
        if (r<=0) { f->error=r<0; f->eof=r==0; break; }
        off+=r;
    }
    return off/size;
}

time_t time(time_t *t) { return result(l01_time(t)); }
double difftime(time_t a,time_t b) { return (double)a-(double)b; }
struct tm *localtime(const time_t *value)
{
    static struct tm t;
    static const int months[]={31,28,31,30,31,30,31,31,30,31,30,31};
    long days=*value/86400, rem=*value%86400;
    if (rem<0) { rem+=86400; days--; }
    t.tm_sec=rem%60; t.tm_min=rem/60%60; t.tm_hour=rem/3600;
    t.tm_wday=(days+4)%7; if (t.tm_wday<0) t.tm_wday+=7;
    int y=1970;
    while (days<0) { y--; days+=365+(y%4==0 && (y%100!=0 || y%400==0)); }
    for (;;) {
        int n=365+(y%4==0 && (y%100!=0 || y%400==0));
        if (days<n) break;
        days-=n; y++;
    }
    t.tm_year=y-1900; t.tm_yday=days; t.tm_mon=0; t.tm_isdst=0;
    for (int m=0;m<12;m++) {
        int n=months[m]+(m==1 && y%4==0 && (y%100!=0 || y%400==0));
        if (days<n) { t.tm_mon=m; break; }
        days-=n;
    }
    t.tm_mday=days+1; return &t; /* UTC: Linux01 has no zone database */
}

/* Compiler runtime for ILP32 64-bit integer formatting. Restoring division
 * uses shifts/subtractions only, so it does not recursively call libgcc. */
unsigned long long __udivmoddi4(unsigned long long n,unsigned long long d,unsigned long long *rem)
{
    unsigned long long q=0,r=0;
    if (!d) exit(136);
    for (int bit=63;bit>=0;bit--) {
        unsigned carry=(unsigned)(r>>63);
        r=(r<<1)|((n>>bit)&1);
        if (carry || r>=d) { r-=d; q|=1ULL<<bit; }
    }
    if (rem) *rem=r;
    return q;
}
unsigned long long __udivdi3(unsigned long long n,unsigned long long d)
{ return __udivmoddi4(n,d,NULL); }
unsigned long long __umoddi3(unsigned long long n,unsigned long long d)
{ unsigned long long r; __udivmoddi4(n,d,&r); return r; }
