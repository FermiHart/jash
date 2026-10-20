/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#ifndef JASH_MINIX_PTHREAD_H
#define JASH_MINIX_PTHREAD_H

/* MINIX's public pthread compatibility types over mthread. These layouts
 * mirror minix/include/minix/mthread.h's _MTHREADIFY_PTHREADS declarations. */
typedef int pthread_t;
typedef int pthread_once_t;
typedef int pthread_key_t;
struct __mthread_mutex;
struct __mthread_cond;
struct __mthread_attr;
typedef struct __mthread_mutex *pthread_mutex_t;
typedef struct __mthread_cond *pthread_cond_t;
typedef struct __mthread_attr *pthread_attr_t;
typedef void *pthread_mutexattr_t;
typedef void *pthread_condattr_t;
typedef struct {
    unsigned int readers;
    pthread_t writer;
    pthread_mutex_t queue;
    struct { pthread_mutex_t mutex; pthread_cond_t cond; } drain;
} pthread_rwlock_t;
typedef void *pthread_rwlockattr_t;

#define PTHREAD_ONCE_INIT 0
#define PTHREAD_MUTEX_INITIALIZER ((pthread_mutex_t)-1)
#define PTHREAD_COND_INITIALIZER ((pthread_cond_t)-1)

int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
void pthread_exit(void *);
pthread_t pthread_self(void);
int pthread_once(pthread_once_t *, void (*)(void));
int pthread_mutex_init(pthread_mutex_t *, pthread_mutexattr_t *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_mutex_destroy(pthread_mutex_t *);
int pthread_cond_init(pthread_cond_t *, pthread_condattr_t *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int pthread_cond_signal(pthread_cond_t *);
int pthread_cond_broadcast(pthread_cond_t *);
int pthread_cond_destroy(pthread_cond_t *);

#endif
