#include "host.h"
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
typedef struct {
    Config cfg;
    Shared *shared;
    Node nodes[NODES];
    pid_t drivers[NODES],reporter;
    int wake[NODES],signals;
    bool started[NODES],failed[NODES],expected_exit[NODES],quit,stop_nodes,fatal;
    uint64_t start_ns[NODES];
    uint32_t pending_timeout[NODES];
    Task ids[TASK_CAP];
    size_t ids_count;
} Host;
static void inspect(Host *h) {
    struct signalfd_siginfo si;
    if(read(h->signals,&si,sizeof si)==sizeof si) {
        h->quit=true;
        h->stop_nodes=false;
    }
    uint64_t now=monotonic_ns(),last[NODES]= {
        0
    };
    bool io[NODES]= {
        0
    };
    if(sem_wait_ms(&h->shared->lock,20)==0) {
        memcpy(last,h->shared->last_tick_ns,sizeof last);
        memcpy(io,h->shared->io_failed,sizeof io);
        sem_post(&h->shared->lock);
    }
    uint64_t window=(uint64_t)h->cfg.tick_us*h->cfg.report_ticks*h->cfg.missed_reports*1000;
    for(int n=0;n<NODES;n++) {
        if(h->drivers[n]>0) {
            int status;
            pid_t p=waitpid(h->drivers[n],&status,WNOHANG);
            if(p>0) {
                h->drivers[n]=0;
                if(!h->expected_exit[n])io[n]=true;
            }
        }
        uint64_t base=last[n]>h->start_ns[n]?last[n]:h->start_ns[n];
        if(h->nodes[n].operational && (io[n] || (h->started[n] && now-base>=window))) {
            h->nodes[n].operational=false;
            h->failed[n]=true;
            note(h->shared,n,"FALLA: %s; tiempo monotónico=%"PRIu64,io[n]?"error de comunicación/controlador":"reportes de tick ausentes",now);
            fprintf(stderr,"%s caído: %s\n",n?"Pico":"ESP32",io[n]?"comunicación":"tick watchdog");
        }
    }
    if(h->reporter>0) {
        int status;
        pid_t p=waitpid(h->reporter,&status,WNOHANG);
        if(p>0) {
            h->reporter=0;
            h->fatal=true;
            h->quit=true;
            fprintf(stderr,"El proceso de reporte terminó antes de tiempo (%d).\n",status);
        }
    }
}

static bool send_command(Host *h,int n,const Frame *f) {
    if(h->drivers[n]<=0)return false;
    Record r= {
        .kind=R_COMMAND,.node=n,.frame=*f
    };
    if(queue_push(&h->shared->commands[n],&r,20)<0)return false;
    h->pending_timeout[n]=command_timeout_ms(&h->cfg,f->type);
    return eventfd_write(h->wake[n],1)==0;
}

static Record await_reply(Host *h,int n) {
    Record r= {
        .code=REPLY_IO
    };
    uint64_t end=monotonic_ns()+((uint64_t)h->pending_timeout[n]*2+1500)*1000000;
    while(monotonic_ns()<end && h->drivers[n]>0) {
        if(queue_pop(&h->shared->replies[n],&r,50)==0) {
            inspect(h);
            return r;
        }
        inspect(h);
    }
    r.code=REPLY_UNCERTAIN;
    return r;
}

static Record transaction(Host *h,int n,const Frame *f) {
    Record r= {
        .code=REPLY_IO
    };
    if(send_command(h,n,f))r=await_reply(h,n);
    return r;
}

static void log_task(Host *h,int n,const Task *t,int code,const char *why) {
    Record r= {
        .kind=R_TASK,.node=n,.id=t->id,.task=*t,.code=code
    };
    if(n>=0)r.value=task_cost(t->W,h->nodes[n].speed);
    snprintf(r.text,sizeof r.text,"%s",why);
    emit(h->shared,&r);
    printf("Tarea %"PRIu32": %s%s\n",t->id,why,n<0?"":n?" [Pico]":" [ESP32]");
    fflush(stdout);
}

static void quarantine(Host *h,int n) {
    if(h->nodes[n].operational) {
        h->nodes[n].operational=false;
        h->failed[n]=true;
    }
}

static Frame task_frame(uint8_t type,const Task *tasks,size_t count) {
    Frame f= {
        .type=type
    };
    size_t used=0;
    for(size_t i=0;i<count;i++) {
        const Task *t=&tasks[i];
        int k=snprintf((char *)f.data+used,PAYLOAD-used,"%"PRIu32",%c,%"PRIu32",%"PRIu32",%"PRIu32",%"PRId32"\n",t->id,t->type,t->W,t->T,t->D,t->criticality);
        if(k<0 || (size_t)k>=PAYLOAD-used) {
            f.length=0;
            return f;
        }
        used+=(size_t)k;
    }
    f.length=(uint16_t)used;
    return f;
}

static void remove_last(Node *n) {
    if(n->count)n->count--;
}

static bool deploy_add(Host *h,int n,const Task *t,const char *label) {
    log_task(h,n,t,0,"ADMITIDA por análisis; pendiente de ACK");
    Frame f=task_frame(ADD_TASK,t,1);
    Record r=transaction(h,n,&f);
    if(r.code==REPLY_OK) {
        log_task(h,n,t,1,label);
        return true;
    }
    if(r.code==REPLY_REJECT) {
        remove_last(&h->nodes[n]);
        log_task(h,n,t,3,"RECHAZADA por firmware");
    } else {
        log_task(h,n,t,4,"INCIERTA por comunicación; reserva conservada hasta declarar caída");
        quarantine(h,n);
    }
    return false;
}

static void relocate(Host *h) {
    for(int failed=0;failed<NODES;failed++) if(h->failed[failed]) {
        h->failed[failed]=false;
        Node plan[NODES];
        memcpy(plan,h->nodes,sizeof plan);
        Task lost[NODE_CAP];
        bool keep[NODE_CAP];
        size_t count=migrate_plan(plan,failed,h->cfg.algorithm,h->cfg.heuristic,lost,keep);
        for(size_t i=0;i<count;i++)log_task(h,failed,&lost[i],2,"RETIRADA del nodo caído; progreso descartado");
        h->nodes[failed].count=0;
        int dest=1-failed;
        Task moving[NODE_CAP];
        size_t m=0;
        for(size_t i=0;i<count;i++) {
            if(keep[i])moving[m++]=lost[i];
            else if(lost[i].type=='S')log_task(h,-1,&lost[i],3,"RECHAZADA: esporádica no migra tras caída");
            else {
                const char *why=admission(&plan[1-failed],&lost[i],h->cfg.algorithm);
                char reason[192];
                snprintf(reason,sizeof reason,"RECHAZADA: caída; %s; política de criticidad",why?why:"conjunto original no admisible");
                log_task(h,-1,&lost[i],3,reason);
            }
        }
        uint32_t fastest=h->nodes[0].speed>h->nodes[1].speed?h->nodes[0].speed:h->nodes[1].speed;
        sort_tasks(moving,m,fastest);
        for(size_t i=0;i<m;i++) {
            const char *why=admission(&h->nodes[dest],&moving[i],h->cfg.algorithm);
            if(why) {
                log_task(h,-1,&moving[i],3,why);
                continue;
            }
            h->nodes[dest].tasks[h->nodes[dest].count++]=moving[i];
            deploy_add(h,dest,&moving[i],"REUBICADA y confirmada; trabajos nuevos en destino");
        }
    }
}

static void manual_add(Host *h,const char *line) {
    Task t;
    char err[256];
    if(!task_parse(line,&t,err,sizeof err)) {
        fprintf(stderr,"Tarea inválida: %s\n",err);
        note(h->shared,-1,"entrada inválida: %s",err);
        return;
    }
    for(size_t i=0;i<h->ids_count;i++)if(h->ids[i].id==t.id) {
        log_task(h,-1,&t,3,"RECHAZADA: id duplicado en la sesión");
        return;
    }
    if(h->ids_count==TASK_CAP) {
        log_task(h,-1,&t,3,"RECHAZADA: límite de 512 IDs por sesión");
        return;
    }
    h->ids[h->ids_count++]=t;
    int n=place(h->nodes,&t,h->cfg.algorithm,h->cfg.heuristic,err,sizeof err);
    if(n<0)log_task(h,-1,&t,3,err);
    else deploy_add(h,n,&t,"DESPLEGADA y confirmada sin reiniciar tick");
}

static void command(Host *h,const char *line) {
    if(!strcmp(line,"q")) {
        h->quit=true;
        h->stop_nodes=true;
    } else if(!strncmp(line,"add ",4))manual_add(h,line+4);
    else if(!strncmp(line,"trigger ",8)) {
        uint32_t id;
        int node=-1;
        if(parse_u32(line+8,&id))for(int n=0;n<NODES;n++)if(h->nodes[n].operational) for(size_t i=0;i<h->nodes[n].count;i++)if(h->nodes[n].tasks[i].id==id && h->nodes[n].tasks[i].type=='S')node=n;
        if(node<0) {
            fprintf(stderr,"trigger requiere ID de una esporádica desplegada en un nodo operativo\n");
            return;
        }
        Frame f= {
            .type=SPORADIC
        };
        Record r=transaction(h,node,&f);
        note(h->shared,node,"solicitud esporádica id=%"PRIu32": %s; el nodo decide liberación/intervalo mínimo",id,r.code==REPLY_OK?"confirmada":r.code==REPLY_REJECT?"rechazada":"incierta");
        if(r.code==REPLY_UNCERTAIN || r.code==REPLY_IO)quarantine(h,node);
    } else if(!strcmp(line,"status")) {
        for(int n=0;n<2;n++)printf("%s: %s s=%"PRIu32" tareas=%zu U=%.6Lf\n",n?"Pico":"ESP32",h->nodes[n].operational?"operativo":"caído/no iniciado",h->nodes[n].speed,h->nodes[n].count,node_util(&h->nodes[n]));
        fflush(stdout);
    } else if(!strcmp(line,"fail esp") || !strcmp(line,"fail pico")) {
        int n=!strcmp(line,"fail pico");
        quarantine(h,n);
        Record r= {
            .kind=R_EXIT
        };
        queue_push(&h->shared->commands[n],&r,20);
        eventfd_write(h->wake[n],1);
        h->expected_exit[n]=true;
        note(h->shared,n,"desconexión simulada mediante comando fail; sin STOP");
    } else if(*line)fprintf(stderr,"Comandos: add id,P|S,W,T,D,criticidad | trigger id | status | fail esp | fail pico | q\n");
}

static void bootstrap(Host *h,Task *tasks,size_t count) {
    Frame cal= {
        .type=CAL_REQ,.length=4
    };
    put32(cal.data,h->cfg.tick_us);
    bool sent[NODES];
    for(int n=0;n<NODES;n++)sent[n]=send_command(h,n,&cal);
    for(int n=0;n<NODES;n++) {
        Record r= {
            .code=REPLY_IO
        };
        if(sent[n])r=await_reply(h,n);
        if(r.code==REPLY_OK && r.frame.type==CAL_RESP && get32(r.frame.data)) {
            h->nodes[n].speed=get32(r.frame.data);
            h->nodes[n].operational=true;
            note(h->shared,n,"calibración real recibida: s=%"PRIu32" UT/tick, tick_us=%"PRIu32,h->nodes[n].speed,h->cfg.tick_us);
            printf("%s: calibración s=%"PRIu32" UT/tick\n",n?"Pico":"ESP32",h->nodes[n].speed);
        } else note(h->shared,n,"calibración inválida o sin respuesta; nodo excluido");
    }
    if(h->quit)return;
    uint32_t fastest=h->nodes[0].speed>h->nodes[1].speed?h->nodes[0].speed:h->nodes[1].speed;
    if(!fastest)fastest=1;
    sort_tasks(tasks,count,fastest);
    for(size_t i=0;i<count;i++) {
        char err[256];
        int n=place(h->nodes,&tasks[i],h->cfg.algorithm,h->cfg.heuristic,err,sizeof err);
        log_task(h,n,&tasks[i],n<0?3:0,n<0?err:"ADMITIDA por análisis; pendiente de despliegue");
    }
    for(int n=0;n<NODES;n++) if(h->nodes[n].operational && !h->quit) {
        Frame f=task_frame(DEPLOY,h->nodes[n].tasks,h->nodes[n].count);
        Record r=transaction(h,n,&f);
        if(r.code==REPLY_OK) {
            for(size_t i=0;i<h->nodes[n].count;i++)log_task(h,n,&h->nodes[n].tasks[i],1,"DESPLEGADA y confirmada");
        } else {
            for(size_t i=0;i<h->nodes[n].count;i++)log_task(h,n,&h->nodes[n].tasks[i],r.code==REPLY_REJECT?3:4,r.code==REPLY_REJECT?"RECHAZADA por firmware":"INCIERTA: despliegue sin confirmación");
            quarantine(h,n);
        }
    }
    if(h->quit)return;
    Frame start=start_frame(&h->cfg);
    uint64_t begin[2]= {
        0
    },ack[2]= {
        0
    };
    for(int n=0;n<NODES;n++) {
        sent[n]=false;
        if(h->nodes[n].operational) {
            begin[n]=monotonic_ns();
            sent[n]=send_command(h,n,&start);
        }
    }
    for(int n=0;n<NODES;n++) if(sent[n]) {
        Record r=await_reply(h,n);
        if(r.code==REPLY_OK) {
            h->started[n]=true;
            h->start_ns[n]=r.ns;
            ack[n]=r.ns;
            note(h->shared,n,"START confirmado; intervalo host de tick cero=[%"PRIu64",%"PRIu64"] ns",begin[n],ack[n]);
        } else quarantine(h,n);
    } else if(h->nodes[n].operational)quarantine(h,n);
    if(ack[0] && ack[1])note(h->shared,-1,"desfase cero ESP32-Pico dentro de [%.3Lf, %.3Lf] ms según envío/ACK; NO es medición exacta de ejecución",((long double)begin[0]-ack[1])/1000000,((long double)ack[0]-begin[1])/1000000);
    fflush(stdout);
}

static bool wait_bounded(pid_t *pid,unsigned ms) {
    if(*pid<=0)return false;
    int status=0;
    uint64_t end=monotonic_ns()+(uint64_t)ms*1000000;
    while(monotonic_ns()<end) {
        pid_t r=waitpid(*pid,&status,WNOHANG);
        if(r==*pid || (r<0 && errno==ECHILD)) {
            *pid=0;
            return r>0 && WIFEXITED(status) && WEXITSTATUS(status)==0;
        }
        poll(NULL,0,20);
    }
    kill(*pid,SIGTERM);
    end=monotonic_ns()+500000000;
    while(monotonic_ns()<end) {
        pid_t r=waitpid(*pid,&status,WNOHANG);
        if(r==*pid || (r<0 && errno==ECHILD)) {
            *pid=0;
            return r>0 && WIFEXITED(status) && WEXITSTATUS(status)==0;
        }
        poll(NULL,0,20);
    }
    kill(*pid,SIGKILL);
    while(waitpid(*pid,NULL,0)<0 && errno==EINTR) {
    }
    *pid=0;
    return false;
}

static void shutdown_host(Host *h) {
    if(h->stop_nodes) {
        for(int n=0;n<NODES;n++)if(h->drivers[n]>0 && !h->expected_exit[n]) {
            Frame f= {
                .type=STOP
            };
            Record r=transaction(h,n,&f);
            note(h->shared,n,"STOP: %s",r.code==REPLY_OK?"confirmado":"sin confirmación/rechazado");
        }
        /* Da a los controladores una ventana acotada para eventos finales. */
        poll(NULL,0,100);
    }
    for(int n=0;n<NODES;n++)if(h->drivers[n]>0) {
        Record r= {
            .kind=R_EXIT
        };
        queue_push(&h->shared->commands[n],&r,20);
        eventfd_write(h->wake[n],1);
        h->expected_exit[n]=true;
        wait_bounded(&h->drivers[n],h->cfg.timeout_ms*2+500);
    }
    if(h->reporter>0) {
        Record end= {
            .kind=R_END
        };
        if(queue_push(&h->shared->events,&end,1000)<0){h->fatal=true;fprintf(stderr,"Cola bloqueada: no se pudo solicitar cierre del reporte.\n");}
        if(!wait_bounded(&h->reporter,5000)){h->fatal=true;fprintf(stderr,"No se pudo finalizar el reporte; salida parcial.\n");}
    }
}

static void child_signals(void) {
    signal(SIGINT,SIG_IGN);
    signal(SIGTERM,SIG_DFL);
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK,&empty,NULL);
}

int main(int argc,char **argv) {
    if(argc!=2) {
        fprintf(stderr,"Uso: %s configuración.ini\n",argv[0]);
        return 2;
    }
    Host h= {
        .wake= {
            -1,-1
        },.signals=-1
    };
    char err[512];
    Task tasks[TASK_CAP];
    size_t count;
    if(!config_load(argv[1],&h.cfg,err,sizeof err) || !task_load(h.cfg.tasks,tasks,&count,err,sizeof err)) {
        fprintf(stderr,"%s\n",err);
        return 2;
    }
    memcpy(h.ids,tasks,count*sizeof *tasks);
    h.ids_count=count;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask,SIGINT);
    sigaddset(&mask,SIGTERM);
    sigprocmask(SIG_BLOCK,&mask,NULL);
    h.signals=signalfd(-1,&mask,SFD_NONBLOCK|SFD_CLOEXEC);
    if(h.signals<0) {
        perror("signalfd");
        return 1;
    }
    char shm_name[128];
    int shm_fd=-1;
    h.shared=ipc_create(shm_name,sizeof shm_name,&shm_fd);
    if(!h.shared) {
        perror("shm_open/mmap/semaphores");
        close(h.signals);
        return 1;
    }
    /* mmap sigue válido tras unlink; no queda un nombre huérfano si el host muere. */
    shm_unlink(shm_name);
    for(int n=0;n<NODES;n++) {
        h.wake[n]=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
        if(h.wake[n]<0)goto init_error;
    }
    h.reporter=fork();
    if(h.reporter<0)goto init_error;
    if(h.reporter==0) {
        child_signals();
        close(h.signals);
        for(int n=0;n<NODES;n++)close(h.wake[n]);
        int rc=report_run(h.shared,&h.cfg);
        _exit(rc);
    }
    for(int n=0;n<NODES;n++) {
        h.drivers[n]=fork();
        if(h.drivers[n]<0) {
            h.drivers[n]=0;
            goto init_error;
        }
        if(h.drivers[n]==0) {
            child_signals();
            close(h.signals);
            close(h.wake[1-n]);
            driver_run(h.shared,n,h.wake[n],&h.cfg);
            _exit(0);
        }
    }
    note(h.shared,-1,"inicio: coordinador=%ld, ESP32=%ld, Pico=%ld, reporte=%ld",(long)getpid(),(long)h.drivers[0],(long)h.drivers[1],(long)h.reporter);
    bootstrap(&h,tasks,count);
    struct termios saved;
    bool terminal=isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO,&saved)==0;
    if(terminal) {
        struct termios t=saved;
        t.c_lflag&=~(tcflag_t)ICANON;
        t.c_cc[VMIN]=1;
        t.c_cc[VTIME]=0;
        tcsetattr(STDIN_FILENO,TCSANOW,&t);
    }
    printf("Comandos: add id,P|S,W,T,D,criticidad | trigger id | status | fail esp | fail pico | q\n");
    fflush(stdout);
    char input[256];
    size_t used=0;
    bool discard=false;
    int input_fd=STDIN_FILENO;
    while(!h.quit) {
        inspect(&h);
        if(h.quit)break;
        relocate(&h);
        struct pollfd f[2]= {
            {
                .fd=input_fd,.events=POLLIN
            }, {
                .fd=h.signals,.events=POLLIN
            }
        };
        int r=poll(f,2,100);
        if(r<0 && errno!=EINTR) {
            h.fatal=true;
            break;
        }
        if(f[0].revents&(POLLIN|POLLHUP)) {
            char bytes[256];
            ssize_t got=read(input_fd,bytes,sizeof bytes);
            if(got<=0)input_fd=-1;
            for(ssize_t i=0;i<got && !h.quit;i++) {
                unsigned char ch=(unsigned char)bytes[i];
                if(ch=='\r')continue;
                if(ch=='q' && used==0 && !discard) {
                    command(&h,"q");
                    break;
                }
                if(ch=='\n') {
                    if(!discard) {
                        input[used]=0;
                        command(&h,input);
                    } else fprintf(stderr,"Comando demasiado largo/inválido\n");
                    used=0;
                    discard=false;
                } else if(ch==127 || ch==8) {
                    if(used)used--;
                } else if(ch<32 || used>=sizeof input-1)discard=true;
                else if(!discard)input[used++]=(char)ch;
            }
        }
    }
    if(terminal)tcsetattr(STDIN_FILENO,TCSANOW,&saved);
    shutdown_host(&h);
    for(int n=0;n<NODES;n++)close(h.wake[n]);
    close(h.signals);
    ipc_destroy(h.shared,shm_fd,shm_name);
    return h.fatal?1:0;
    init_error: perror("inicialización");
    h.fatal=true;
    shutdown_host(&h);
    for(int n=0;n<NODES;n++)if(h.wake[n]>=0)close(h.wake[n]);
    close(h.signals);
    ipc_destroy(h.shared,shm_fd,shm_name);
    return 1;
}
