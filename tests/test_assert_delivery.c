// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* The actual assertion provider, supervisor and encoders with a deterministic
 * short-writing, interrupted, then broken sink. Only presentation hooks stubbed. */
#include <builtins/analysis_output.h>
#include <core/output_budget.h>
#include <parser/builtin_redir.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#condition); exit(1); \
} } while (0)
void jash_budget_unwrap(jash_writer_t* writer, void** context) { (void)writer; (void)context; }
long jash_budget_limit(void) { return 0; }
void jash_redir_stream_writer(jash_writer_t writer, void* context) { (void)writer; (void)context; }

typedef struct { size_t bytes; int deny, after_first, interrupted; } delivery_t;
static long deliver(void* context,const char* data,size_t size) {
    delivery_t* output = context; (void)data;
    if (!output->interrupted) { output->interrupted=1;errno=EINTR;return -1; }
    if (output->deny || (output->after_first && access("first",F_OK)==0)) { errno=EPIPE;return -1; }
    if (size>7) size=7;
    output->bytes+=size; return (long)size;
}
static size_t stored(char* bytes,size_t limit) {
    const char* home=getenv("HOME");char path[4096];
    CHECK(home&&snprintf(path,sizeof path,"%s/.jash/assertions.tsv",home)>0);
    FILE* file=fopen(path,"rb");CHECK(file!=NULL);
    size_t size=fread(bytes,1,limit,file);CHECK(!ferror(file)&&feof(file));CHECK(fclose(file)==0);
    return size;
}
int main(void) {
    char* first[]={"--json","first check","--check","touch first"};
    char* second[]={"--json","second check","--check","touch second"};
    delivery_t output={0};
    CHECK(jash_b_assert_structured_argv(NULL,deliver,&output,4,first)==0);
    CHECK(jash_b_assert_structured_argv(NULL,deliver,&output,4,second)==0);
    CHECK(unlink("first")==0&&unlink("second")==0);
    char before[65536],after[65536];size_t length=stored(before,sizeof before);
    const char* formats[]={"--json","--data","--data-v2"};
    for (size_t i=0;i<3;i++) {
        char* args[]={(char*)formats[i]};
        output=(delivery_t){.deny=1};
        CHECK(jash_b_assert_structured_argv(NULL,deliver,&output,1,args)==125);
        CHECK(access("first",F_OK)<0&&access("second",F_OK)<0);
        output=(delivery_t){.after_first=1};
        CHECK(jash_b_assert_structured_argv(NULL,deliver,&output,1,args)==125);
        CHECK(output.bytes>0&&access("first",F_OK)==0&&access("second",F_OK)<0);
        CHECK(stored(after,sizeof after)==length&&!memcmp(before,after,length));
        CHECK(unlink("first")==0);
    }
    return 0;
}
