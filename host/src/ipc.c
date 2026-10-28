// Colas y semáforos POSIX para comunicar los cuatro procesos del host.
#include "host.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// Mide duraciones con un reloj que no cambia al ajustar la hora del sistema.
uint64_t monotonic_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
}

// Con ms=0 intenta sin esperar; en otro caso bloquea hasta una fecha límite monotónica.
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
    // Si una señal interrumpe la espera, reintenta sin reiniciar su fecha límite.
    do {
        r=sem_clockwait(s,CLOCK_MONOTONIC,&t);
    } while(r<0 && errno==EINTR);
    return r;
}

// Inicializa una cola vacía con semáforos compartidos entre procesos (pshared=1).
int queue_init(Queue *q) {
    memset(q,0,sizeof *q);
    if(sem_init(&q->spaces,1,QUEUE_CAP)<0) return -1; // Espacios libres: al inicio caben QUEUE_CAP mensajes.
    if(sem_init(&q->items,1,0)<0) { // Mensajes listos: la cola empieza vacía.
        sem_destroy(&q->spaces);
        return -1;
    }
    if(sem_init(&q->mutex,1,1)<0) { // Exclusión mutua: uno permite entrar a la sección crítica.
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

// Productor: reserva espacio, copia bajo mutex y publica un mensaje disponible.
int queue_push(Queue *q,const Record *r,int ms) {
    if(sem_wait_ms(&q->spaces,ms)<0) return -1;
    if(sem_wait_ms(&q->mutex,20)<0) {
        sem_post(&q->spaces); // Devuelve la reserva si no pudo tomar el mutex.
        return -1;
    }

    // La sección crítica solo copia y mueve índices; no hace serial ni espera un ACK.
    q->data[q->head]=*r;
    // El módulo devuelve el índice al inicio cuando alcanza el final del arreglo.
    q->head=(q->head+1)%QUEUE_CAP;
    sem_post(&q->mutex);
    // Publica el mensaje después de soltar el mutex para que el consumidor pueda tomarlo.
    sem_post(&q->items);
    return 0;
}

// Consumidor: reserva un mensaje, lo copia bajo mutex y devuelve el espacio libre.
int queue_pop(Queue *q,Record *r,int ms) {
    if(sem_wait_ms(&q->items,ms)<0) return -1;
    if(sem_wait_ms(&q->mutex,20)<0) {
        sem_post(&q->items); // Devuelve el mensaje reservado porque no se consumió.
        return -1;
    }
    *r=q->data[q->tail];
    q->tail=(q->tail+1)%QUEUE_CAP;
    sem_post(&q->mutex);
    sem_post(&q->spaces);
    return 0;
}

// shm_open crea el objeto, ftruncate fija el tamaño y mmap comparte sus datos entre procesos.
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
    // Se crean dos colas de órdenes, dos de respuestas y una cola común de eventos.
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

// Se destruyen semáforos y mapeo después de terminar los hijos que los usan.
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

// No espera espacio para eventos; si la cola se llena, intenta contar el descarte bajo lock.
void emit(Shared *s,Record *r) {
    r->ns=monotonic_ns();
    if(queue_push(&s->events,r,0)<0 && sem_wait_ms(&s->lock,20)==0) {
        s->dropped[r->node>=0 && r->node<NODES?r->node:NODES]++;
        sem_post(&s->lock);
    }
}

// Forma una nota de texto y la publica por la misma cola de eventos.
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
