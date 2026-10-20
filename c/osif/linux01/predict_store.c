/* Author: F E R M I ∞ H A R T <contact@fermihart.com>
 * SPDX-License-Identifier: LicenseRef-SPL-1.0
 * The canonical predictor's text loader splits at the first space, so it
 * cannot roundtrip multiword commands. This OSIF supplies a bounded ILP32
 * persistence envelope; learning/scoring remain canonical core/predict.c.
 */
#include <port.h>
#include "predict_store.h"
struct header { uint32_t magic,version,count,checksum; };
static uint32_t digest(const void *p,size_t n)
{
    const unsigned char *s=p; uint32_t h=2166136261u;
    for(size_t i=0;i<n;i++) h=(h^s[i])*16777619u;
    return h;
}
int linux01_predict_save(predict_table_t *p)
{
    if(!p->dirty) return 0;
    size_t size=(size_t)p->count*sizeof(p->entries[0]);
    struct header h={0x4a505231,1,p->count,digest(p->entries,size)};
    FILE *f=fopen(p->persist_path,"w"); if(!f) return -1;
    int ok=fwrite(&h,sizeof(h),1,f)==1 && fwrite(p->entries,1,size,f)==size;
    if(fclose(f)) ok=0;
    if(ok) p->dirty=0;
    return ok?0:-1;
}
int linux01_predict_load(predict_table_t *p)
{
    FILE *f=fopen(p->persist_path,"r"); if(!f) return errno==ENOENT?0:-1;
    struct header h; int ok=fread(&h,sizeof(h),1,f)==1;
    if(!ok || h.magic!=0x4a505231 || h.version!=1 || h.count>PREDICT_MAX) {fclose(f);return -1;}
    size_t size=h.count*sizeof(p->entries[0]);
    predict_entry_t *entries=malloc(size?size:1);
    if(!entries) {fclose(f);return -1;}
    char extra;
    ok=fread(entries,1,size,f)==size && !fread(&extra,1,1,f) && !f->error && digest(entries,size)==h.checksum;
    for(unsigned i=0;ok && i<h.count;i++)
        ok=memchr(entries[i].cmd,0,sizeof(entries[i].cmd)) &&
           memchr(entries[i].context,0,sizeof(entries[i].context)) && entries[i].count>0;
    if(ok){memcpy(p->entries,entries,size);p->count=h.count;p->dirty=0;}
    free(entries);fclose(f);return ok?0:-1;
}
