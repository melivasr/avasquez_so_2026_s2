// Cada driver administra un puerto y atiende órdenes y telemetría.
#include "host.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>

// Devuelve el resultado al coordinador por la cola replies de este nodo.
static void reply(Shared *s,int n,int code,const Frame *f) {
    Record r= {
        .kind=R_REPLY,.node=n,.code=code,.ns=monotonic_ns()
    };
    if(f)r.frame=*f;
    if(queue_push(&s->replies[n],&r,20)<0)note(s,n,"pérdida de respuesta IPC");
}

// Actualiza la bandera de error de comunicación bajo el semáforo de estado.
static void io_failure(Shared *s,int n) {
    if(sem_wait_ms(&s->lock,20)==0) {
        s->io_failed[n]=true;
        sem_post(&s->lock);
    }
}

// Mantiene una sola orden pendiente de respuesta por puerto.
void driver_run(Shared *s,int n,int wake,const Config *cfg) {

    pid_t parent=getppid();

    // Si muere el coordinador, el driver termina sin enviar STOP a la placa.
    prctl(PR_SET_PDEATHSIG,SIGTERM);
    if(parent==1 || getppid()!=parent)return;
    int fd=serial_open(cfg->ports[n]);
    if(fd<0) {
        note(s,n,"puerto %s: %s",cfg->ports[n],strerror(errno));
        io_failure(s,n);
    }

    // Deja arrancar las placas que se reinician al abrir su puerto USB.
    if(fd>=0)poll(NULL,0,1000);
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

        // Sin ACK el resultado es incierto; no reenvía automáticamente una orden que pudo aplicarse.
        if(active && now>=deadline) {
            reply(s,n,REPLY_UNCERTAIN,NULL);
            active=false;
            io_failure(s,n);
            note(s,n,"timeout: resultado incierto; nodo en cuarentena");
        }
        // Tras 200 ms sin nuevos bytes descarta una trama parcial para no quedar esperando su final.
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

        // Espera avisos de órdenes en eventfd o bytes del nodo, sin dejar de controlar los timeouts.
        int result=poll(f,2,timeout);
        if(result<0) {
            if(errno==EINTR)continue;
            break;
        }
        if(f[0].revents&POLLIN) {
            eventfd_t v;

            if(eventfd_read(wake,&v)<0 && errno!=EAGAIN)break;
            Record c;
            // eventfd solo avisa; las órdenes completas se retiran de la cola compartida.
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
                uint32_t command_ms=command_timeout_ms(cfg,c.frame.type);
                if(!len || serial_write(fd,bytes,len,command_ms)<0) {
                    reply(s,n,REPLY_UNCERTAIN,NULL);
                    io_failure(s,n);
                    close(fd);
                    fd=-1;
                    continue;
                }
                active=true;
                deadline=monotonic_ns()+(uint64_t)command_ms*1000000;
                note(s,n,"envío tipo %u terminado a %llu ns (recepción ACK pendiente)",c.frame.type,(unsigned long long)monotonic_ns());
            }
        }
        if(done)break;
        if(fd>=0 && f[1].revents&POLLIN) {
            uint8_t bytes[1024];
            // Una lectura puede traer media trama o varias; el parser conserva el avance entre lecturas.
            ssize_t got=read(fd,bytes,sizeof bytes);
            if(got>0) {
                last_byte=monotonic_ns();
                for(ssize_t i=0;i<got;i++) {
                    Frame frame;
                    if(!parser_byte(&parser,bytes[i],&frame))continue;

                    // Acepta el ACK solo si corresponde a la orden pendiente; la telemetría se procesa aparte.
                    if(frame.type==ACK && frame.length==2 && active && frame.data[0]==pending.frame.type) {
                        if(pending.frame.type==STOP && frame.data[1]==RT_OK) {
                            // El ACK de STOP llega tras vaciar eventos y permite cerrar el último tramo observado del Gantt.
                            Record stop={.kind=R_STOP,.node=n};
                            emit(s,&stop);
                        }
                        reply(s,n,frame.data[1]?REPLY_REJECT:REPLY_OK,&frame);
                        active=false;
                    // La calibración responde con la velocidad medida en CAL_RESP, no con un ACK de éxito.
                    } else if(frame.type==CAL_RESP && frame.length==4 && active && pending.frame.type==CAL_REQ) {
                        reply(s,n,get32(frame.data)?REPLY_OK:REPLY_REJECT,&frame);
                        active=false;
                    } else if(receive_telemetry(s,n,&frame)) {

                    } else note(s,n,"trama inesperada o carga inválida tipo=%u len=%u",frame.type,frame.length);
                }
            } else if(got==0 || (errno!=EAGAIN && errno!=EINTR)) {
                io_failure(s,n);
                close(fd);
                fd=-1;
            }
        }

        // Al desconectar el puerto, informa el fallo y resuelve como incierta cualquier orden sin respuesta.
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
