#include "host.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>
static void reply(Shared *s,int n,int code,const Frame *f) {
    Record r= {
        .kind=R_REPLY,.node=n,.code=code,.ns=monotonic_ns()
    };
    if(f)r.frame=*f;
    if(queue_push(&s->replies[n],&r,20)<0)note(s,n,"pérdida de respuesta IPC");
}

static void io_failure(Shared *s,int n) {
    if(sem_wait_ms(&s->lock,20)==0) {
        s->io_failed[n]=true;
        sem_post(&s->lock);
    }
}

void driver_run(Shared *s,int n,int wake,const Config *cfg) {
    /* Un huérfano cierra su puerto sin STOP; la ejecución local sigue. */
    pid_t parent=getppid();
    prctl(PR_SET_PDEATHSIG,SIGTERM);
    if(parent==1 || getppid()!=parent)return;
    int fd=serial_open(cfg->ports[n]);
    if(fd<0) {
        note(s,n,"puerto %s: %s",cfg->ports[n],strerror(errno));
        io_failure(s,n);
    }
    Parser parser= {
        0
    };
    Record pending= {
        0
    };
    bool active=false,done=false;
    uint64_t deadline=0,last_byte=0,reported_errors=0;
    while(!done) {
        uint64_t now=monotonic_ns();
        if(active && now>=deadline) {
            reply(s,n,REPLY_UNCERTAIN,NULL);
            active=false;
            io_failure(s,n);
            note(s,n,"timeout: resultado incierto; nodo en cuarentena");
        }
        if(parser.used && now-last_byte>=200000000) {
            parser_expire(&parser);
        }
        if(parser.errors!=reported_errors) {
            note(s,n,"tramas corruptas/incompletas descartadas: %llu",(unsigned long long)(parser.errors-reported_errors));
            reported_errors=parser.errors;
        }
        int timeout=-1;
        if(active) timeout=(int)((deadline>now?deadline-now:0)/1000000+1);
        if(parser.used && (timeout<0 || timeout>200)) timeout=200;
        struct pollfd f[2]= {
            {
                .fd=wake,.events=POLLIN
            }, {
                .fd=fd,.events=POLLIN
            }
        };
        int result=poll(f,2,timeout);
        if(result<0) {
            if(errno==EINTR)continue;
            break;
        }
        if(f[0].revents&POLLIN) {
            eventfd_t v;
            if(eventfd_read(wake,&v)<0 && errno!=EAGAIN)break;
            Record c;
            while(queue_pop(&s->commands[n],&c,0)==0) {
                if(c.kind==R_EXIT) {
                    done=true;
                    break;
                }
                if(c.kind!=R_COMMAND)continue;
                if(active) {
                    reply(s,n,REPLY_REJECT,NULL);
                    continue;
                }
                if(fd<0) {
                    reply(s,n,REPLY_IO,NULL);
                    continue;
                }
                pending=c;
                uint8_t bytes[FRAME_SIZE];
                size_t len=frame_encode(&c.frame,bytes);
                if(!len || serial_write(fd,bytes,len,cfg->timeout_ms)<0) {
                    reply(s,n,REPLY_UNCERTAIN,NULL);
                    io_failure(s,n);
                    close(fd);
                    fd=-1;
                    continue;
                }
                active=true;
                deadline=monotonic_ns()+(uint64_t)cfg->timeout_ms*1000000;
                note(s,n,"envío tipo %u terminado a %llu ns (recepción ACK pendiente)",c.frame.type,(unsigned long long)monotonic_ns());
            }
        }
        if(done)break;
        if(fd>=0 && f[1].revents&POLLIN) {
            uint8_t bytes[1024];
            ssize_t got=read(fd,bytes,sizeof bytes);
            if(got>0) {
                last_byte=monotonic_ns();
                for(ssize_t i=0;i<got;i++) {
                    Frame frame;
                    if(!parser_byte(&parser,bytes[i],&frame))continue;
                    if(frame.type==ACK && frame.length==2 && active && frame.data[0]==pending.frame.type) {
                        reply(s,n,frame.data[1]?REPLY_REJECT:REPLY_OK,&frame);
                        active=false;
                    } else if(frame.type==CAL_RESP && frame.length==4 && active && pending.frame.type==CAL_REQ) {
                        reply(s,n,get32(frame.data)?REPLY_OK:REPLY_REJECT,&frame);
                        active=false;
                    } else if(frame.type==TICK && frame.length==4) {
                        Record r= {
                            .kind=R_TICK,.node=n,.tick=get32(frame.data)
                        };
                        if(sem_wait_ms(&s->lock,20)==0) {
                            s->last_tick_ns[n]=monotonic_ns();
                            s->last_tick[n]=r.tick;
                            sem_post(&s->lock);
                        }
                        emit(s,&r);
                    } else if(frame.type==EVENT && frame.length==13 && frame.data[4]>=1 && frame.data[4]<=8) {
                        Record r= {
                            .kind=R_NODE,.node=n,.tick=get32(frame.data),.code=frame.data[4],.id=get32(frame.data+5),.value=get32(frame.data+9)
                        };
                        emit(s,&r);
                    } else note(s,n,"trama inesperada o carga inválida tipo=%u len=%u",frame.type,frame.length);
                }
            } else if(got==0 || (errno!=EAGAIN && errno!=EINTR)) {
                io_failure(s,n);
                close(fd);
                fd=-1;
            }
        }
        if(fd>=0 && (f[1].revents&(POLLERR|POLLHUP|POLLNVAL))) {
            note(s,n,"desconexión serial");
            io_failure(s,n);
            close(fd);
            fd=-1;
            if(active) {
                reply(s,n,REPLY_UNCERTAIN,NULL);
                active=false;
            }
        }
    }
    if(fd>=0)close(fd);
    close(wake);
}
