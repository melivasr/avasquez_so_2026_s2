#include "host.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
uint64_t monotonic_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
}

int sem_wait_ms(sem_t *s,int ms) {
    if(ms==0) {
        int r;
        do {
            r=sem_trywait(s);
        } while(r<0 && errno==EINTR);
        return r;
    }
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    t.tv_sec+=ms/1000;
    t.tv_nsec+=(long)(ms%1000)*1000000;
    if(t.tv_nsec>=1000000000) {
        t.tv_sec++;
        t.tv_nsec-=1000000000;
    }
    int r;
    do {
        r=sem_clockwait(s,CLOCK_MONOTONIC,&t);
    } while(r<0 && errno==EINTR);
    return r;
}

int queue_init(Queue *q) {
    memset(q,0,sizeof *q);
    if(sem_init(&q->spaces,1,QUEUE_CAP)<0) return -1;
    if(sem_init(&q->items,1,0)<0) {
        sem_destroy(&q->spaces);
        return -1;
    }
    if(sem_init(&q->mutex,1,1)<0) {
        sem_destroy(&q->items);
        sem_destroy(&q->spaces);
        return -1;
    }
    return 0;
}

void queue_destroy(Queue *q) {
    sem_destroy(&q->spaces);
    sem_destroy(&q->items);
    sem_destroy(&q->mutex);
}

int queue_push(Queue *q,const Record *r,int ms) {
    if(sem_wait_ms(&q->spaces,ms)<0) return -1;
    if(sem_wait_ms(&q->mutex,20)<0) {
        sem_post(&q->spaces);
        return -1;
    }
    q->data[q->head]=*r;
    q->head=(q->head+1)%QUEUE_CAP;
    sem_post(&q->mutex);
    sem_post(&q->items);
    return 0;
}

int queue_pop(Queue *q,Record *r,int ms) {
    if(sem_wait_ms(&q->items,ms)<0) return -1;
    if(sem_wait_ms(&q->mutex,20)<0) {
        sem_post(&q->items);
        return -1;
    }
    *r=q->data[q->tail];
    q->tail=(q->tail+1)%QUEUE_CAP;
    sem_post(&q->mutex);
    sem_post(&q->spaces);
    return 0;
}

Shared *ipc_create(char *name,size_t len,int *fd) {
    snprintf(name,len,"/rt_host_%ld_%llu",(long)getpid(),(unsigned long long)monotonic_ns());
    *fd=shm_open(name,O_CREAT|O_EXCL|O_RDWR,0600);
    if(*fd<0) return NULL;
    if(ftruncate(*fd,sizeof(Shared))<0) goto bad;
    Shared *s=mmap(NULL,sizeof *s,PROT_READ|PROT_WRITE,MAP_SHARED,*fd,0);
    if(s==MAP_FAILED) goto bad;
    memset(s,0,sizeof *s);
    if(sem_init(&s->lock,1,1)<0) {
        munmap(s,sizeof *s);
        goto bad;
    }
    Queue *q[]= {
        &s->commands[0],&s->commands[1],&s->replies[0],&s->replies[1],&s->events
    };
    for(size_t i=0;i<5;i++) if(queue_init(q[i])<0) {
        for(size_t j=0;j<i;j++) queue_destroy(q[j]);
        sem_destroy(&s->lock);
        munmap(s,sizeof *s);
        goto bad;
    }
    return s;
    bad: {
        int saved=errno;
        close(*fd);
        shm_unlink(name);
        *fd=-1;
        errno=saved;
        return NULL;
    }
}

void ipc_destroy(Shared *s,int fd,const char *name) {
    if(s) {
        for(int i=0;i<NODES;i++) {
            queue_destroy(&s->commands[i]);
            queue_destroy(&s->replies[i]);
        }
        queue_destroy(&s->events);
        sem_destroy(&s->lock);
        munmap(s,sizeof *s);
    }
    if(fd>=0)close(fd);
    shm_unlink(name);
}

void emit(Shared *s,Record *r) {
    r->ns=monotonic_ns();
    if(queue_push(&s->events,r,0)<0 && sem_wait_ms(&s->lock,20)==0) {
        s->dropped[r->node>=0 && r->node<NODES?r->node:NODES]++;
        sem_post(&s->lock);
    }
}

void note(Shared *s,int node,const char *fmt,...) {
    Record r= {
        .kind=R_NOTE,.node=node
    };
    va_list ap;
    va_start(ap,fmt);
    vsnprintf(r.text,sizeof r.text,fmt,ap);
    va_end(ap);
    emit(s,&r);
}
