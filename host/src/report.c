#include "host.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
typedef struct {
    uint32_t id;
    int node;
    Task task;
    uint64_t releases,runs,completed,misses,ignored;
    uint64_t first_sum,finish_sum,first_n,finish_n,release;
    bool active,first,ambiguous,confirmed;
    uint32_t cost,job;
} Stat;
static FILE *open_output(const Config *c,const char *name) {
    char path[1100];
    snprintf(path,sizeof path,"%s/%s",c->output,name);
    return fopen(path,"w+");
}

static void csv_text(FILE *f,const char *s) {
    fputc('"',f);
    for(;*s;s++) {
        if(*s=='"')fputc('"',f);
        fputc(*s,f);
    }
    fputc('"',f);
}

static Stat *find_stat(Stat *stats,size_t *n,uint32_t id,int node) {
    for(size_t i=0;i<*n;i++)if(stats[i].id==id && stats[i].node==node)return &stats[i];
    if(*n>=TASK_CAP*NODES)return NULL;
    Stat *p=&stats[(*n)++];
    *p=(Stat) {
        .id=id,.node=node
    };
    return p;
}

/* SWITCH mantiene la selección hasta el siguiente cambio. COMPLETE/MISS
 * cierran el trabajo aunque el siguiente SWITCH todavía no haya llegado. */
typedef struct {
    uint64_t cursor;
    uint32_t selected;
    bool valid;
} Trace;

static void advance_trace(Trace *trace,uint64_t until,int node,Stat *stats,size_t *n,
                          FILE *gantt,uint64_t *busy,bool *incomplete) {
    if(until<=trace->cursor)return;
    if(trace->valid && trace->selected) {
        Stat *st=find_stat(stats,n,trace->selected,node);
        if(st && st->active) {
            fprintf(gantt,"%d,%"PRIu32",%"PRIu64",%"PRIu64"\n",
                    node,st->id,trace->cursor,until);
            uint64_t duration=until-trace->cursor;
            *busy+=duration;
            st->runs+=duration;
            if(!st->first && !st->ambiguous && trace->cursor>=st->release) {
                st->first_sum+=trace->cursor-st->release;
                st->first_n++;
                st->first=true;
            }
        } else if(!st)*incomplete=true;
    }
    trace->cursor=until;
}

int report_run(Shared *s,const Config *c) {
    if(mkdir(c->output,0755)<0 && errno!=EEXIST)return 1;
    FILE *events=open_output(c,"events.csv"),*gantt=open_output(c,"gantt.csv"),*statsfile=open_output(c,"tasks.csv"),*html=open_output(c,"report.html");
    if(!events || !gantt || !statsfile || !html) {
        if(events)fclose(events);
        if(gantt)fclose(gantt);
        if(statsfile)fclose(statsfile);
        if(html)fclose(html);
        return 1;
    }
    Stat *stats=calloc(TASK_CAP*NODES,sizeof *stats);
    if(!stats) {
        fclose(events);
        fclose(gantt);
        fclose(statsfile);
        fclose(html);
        return 1;
    }
    size_t n=0;
    uint64_t busy[NODES]={0},end_tick[NODES]={0},switches[NODES]={0};
    Trace traces[NODES]={{.valid=true},{.valid=true}};
    uint32_t counters[NODES][5]={{0}};
    bool incomplete[NODES]={false};
    long double theoretical[2]= {
        0
    };
    fprintf(events,"host_monotonic_ns,node,kind,code,tick,id,value,detail\n");
    fprintf(gantt,"node,id,start_tick,end_tick\n");
    pid_t parent=getppid();
    bool done=parent==1;
    while(!done) {
        Record r;
        if(queue_pop(&s->events,&r,500)<0) {
            if(getppid()!=parent)break;
            continue;
        }
        if(r.kind==R_END) {
            done=true;
            continue;
        }
        fprintf(events,"%"PRIu64",%d,%d,%d,%"PRIu32",%"PRIu32",%"PRIu32",",r.ns,r.node,r.kind,r.code,r.tick,r.id,r.value);
        csv_text(events,r.text);
        fputc('\n',events);
        if(r.node<0 || r.node>=NODES)continue;
        int node=r.node;
        Trace *trace=&traces[node];
        if(r.kind==R_TICK) {
            /* tick k se reporta después de procesar el tramo [k,k+1).
             * ESP32 puede intercalar este reporte entre lotes de eventos más
             * antiguos: no se avanza el cursor del Gantt hasta recibirlos. */
            uint64_t horizon=(uint64_t)r.tick+1;
            if(horizon>end_tick[node])end_tick[node]=horizon;
            for(size_t i=0;i<5;i++) {
                const uint8_t *p=r.frame.data+4+2*i;
                uint32_t value=(uint32_t)p[0]|((uint32_t)p[1]<<8);
                if(value>counters[node][i]) {
                    if(i>=2)incomplete[node]=true;
                    if(i==2 || i==3)trace->valid=false;
                    counters[node][i]=value;
                }
            }
            continue;
        }
        if(r.kind==R_STOP) {
            /* ACK de STOP llega después de todos los eventos pendientes.
             * Cerrar solo hasta un límite efectivamente observado. */
            advance_trace(trace,end_tick[node],node,stats,&n,gantt,&busy[node],&incomplete[node]);
            trace->selected=0;
            continue;
        }
        if(r.kind==R_NOTE) {
            if(strstr(r.text,"corrupt") || strstr(r.text,"descartad") || strstr(r.text,"timeout") || strstr(r.text,"desconexión") || strstr(r.text,"inválida") || strstr(r.text,"inesperada") || strstr(r.text,"FALLA")) {
                incomplete[node]=true;
                trace->valid=false;
            }
            continue;
        }
        if(r.kind==R_TASK) {
            Stat *st=find_stat(stats,&n,r.task.id,node);
            if(!st) {
                incomplete[node]=true;
                continue;
            }
            st->task=r.task;
            st->cost=r.value;
            /* code 1: confirmada; code 2: retirada al perder nodo. */
            if(r.code==1 && !st->confirmed) {
                theoretical[node]+=(long double)r.value/r.task.T;
                st->confirmed=true;
            }
            if(r.code==2 && st->confirmed) {
                theoretical[node]-=(long double)r.value/r.task.T;
                st->confirmed=false;
            }
            continue;
        }
        if(r.kind!=R_NODE)continue;
        if(r.tick>end_tick[node])end_tick[node]=r.tick;
        if(r.code==RT_TICK_LOST) {
            uint64_t gap_start=r.tick>=r.value?r.tick-r.value:0;
            advance_trace(trace,gap_start,node,stats,&n,gantt,&busy[node],&incomplete[node]);
            if(r.tick>trace->cursor)trace->cursor=r.tick;
            trace->valid=false;
            incomplete[node]=true;
            continue;
        }
        if(r.tick<trace->cursor) {
            incomplete[node]=true;
            trace->valid=false;
            continue;
        }
        advance_trace(trace,r.tick,node,stats,&n,gantt,&busy[node],&incomplete[node]);
        if(r.code==RT_SWITCH) {
            if(r.id && r.id!=trace->selected)switches[node]++;
            trace->selected=r.id;
            trace->valid=true;
            continue;
        }
        if(r.code==RT_BAD_FRAME) {
            incomplete[node]=true;
            continue;
        }
        if(!r.id || r.code==RT_TASK_ADDED)continue;
        Stat *st=find_stat(stats,&n,r.id,node);
        if(!st) {
            incomplete[node]=true;
            continue;
        }
        if(r.code==RT_RELEASE) {
            st->releases++;
            st->ambiguous=st->active;
            st->release=r.tick;
            st->job=r.value;
            st->active=true;
            st->first=false;
        } else if(r.code==RT_COMPLETE) {
            st->completed++;
            if(st->active && st->job==r.value && !st->ambiguous && r.tick>=st->release) {
                st->finish_sum+=r.tick-st->release;
                st->finish_n++;
            }
            st->active=false;
        } else if(r.code==RT_MISS) {
            st->misses++;
            st->active=false;
        } else if(r.code==RT_SPORADIC_IGNORED)st->ignored++;
    }
    uint64_t dropped[3]= {
        0
    };
    if(sem_wait_ms(&s->lock,20)==0) {
        memcpy(dropped,s->dropped,sizeof dropped);
        sem_post(&s->lock);
    } else {
        incomplete[0]=true;
        incomplete[1]=true;
    }
    for(int i=0;i<2;i++)if(dropped[i] || dropped[2])incomplete[i]=true;
    fprintf(statsfile,"node,id,type,W,T,D,criticality,C,releases,run_ticks,completed,deadline_misses,sporadic_ignored,first_samples,mean_first_ticks,completion_samples,mean_completion_ticks\n");
    for(size_t i=0;i<n;i++) {
        Stat *p=&stats[i];
        fprintf(statsfile,"%d,%"PRIu32",%c,%"PRIu32",%"PRIu32",%"PRIu32",%"PRId32",%"PRIu32",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",",p->node,p->id,p->task.type?p->task.type:'?',p->task.W,p->task.T,p->task.D,p->task.criticality,p->cost,p->releases,p->runs,p->completed,p->misses,p->ignored,p->first_n);
        if(p->first_n)fprintf(statsfile,"%.3Lf",(long double)p->first_sum/p->first_n);
        fprintf(statsfile,",%"PRIu64",",p->finish_n);
        if(p->finish_n)fprintf(statsfile,"%.3Lf",(long double)p->finish_sum/p->finish_n);
        fputc('\n',statsfile);
    }
    fprintf(html,"<!doctype html><html lang='es'><meta charset='utf-8'><title>Reporte del host</title><style>body{font:16px sans-serif;max-width:1100px;margin:2em auto}svg{width:100%%}td,th{padding:.5em;border:1px solid #ccc}table{border-collapse:collapse}</style><h1>Calendarizador heterogéneo</h1><p>Gantt reconstruido de SWITCH, RELEASE, COMPLETE y MISS recibidos. Las zonas sin bloques no prueban inactividad. Cada nodo usa su tick local. Los intervalos con pérdidas se consideran incompletos.</p><p><a href='events.csv'>Eventos y decisiones</a> · <a href='gantt.csv'>Intervalos</a> · <a href='tasks.csv'>Estadísticas por tarea y latencias</a></p><table><tr><th>Nodo</th><th>U teórica final</th><th>U observada</th><th>Cambios/entradas en ejecución</th><th>Telemetría</th></tr>");
    for(int i=0;i<2;i++) {
        fprintf(html,"<tr><td>%s</td><td>%.6Lf</td><td>",i==0?"ESP32":"Pico",theoretical[i]);
        if(end_tick[i])fprintf(html,"%.6Lf%s",(long double)busy[i]/end_tick[i],incomplete[i]?" (parcial)":"");
        else fprintf(html,"sin datos");
        fprintf(html,"</td><td>%"PRIu64"</td><td>%s; descartes de cola: %"PRIu64"</td></tr>",switches[i],incomplete[i]?"INCOMPLETA":"sin pérdidas detectadas",dropped[i]);
        fprintf(html,"<tr><td colspan=5>Contadores del nodo %d: misses=%u, esporádicas ignoradas=%u, ticks perdidos=%u, eventos descartados=%u, tramas malas=%u (saturan en 65535).</td></tr>",i,counters[i][0],counters[i][1],counters[i][2],counters[i][3],counters[i][4]);
    }
    fprintf(html,"</table><p>Descartes del coordinador: %"PRIu64". U observada = ticks reconstruidos / ticks observados desde START; con pérdidas es parcial. Las latencias primera ejecución y finalización están separadas en tasks.csv; campos vacíos indican ausencia de muestras correlacionables. U teórica cambia con cada confirmación/retiro registrado en events.csv.</p>",dropped[2]);
    uint64_t max=end_tick[0]>end_tick[1]?end_tick[0]:end_tick[1];
    if(!max)max=1;
    fprintf(html,"<svg viewBox='0 0 1100 180' role='img' aria-label='Gantt por nodo'><text x='0' y='25'>ESP32</text><text x='0' y='95'>Pico</text>");
    rewind(gantt);
    char line[128];
    if(fgets(line,sizeof line,gantt)) {
    };
    int node;
    uint32_t id;
    uint64_t begin,end;
    while(fgets(line,sizeof line,gantt)) if(sscanf(line,"%d,%"SCNu32",%"SCNu64",%"SCNu64,&node,&id,&begin,&end)==4) {
        double x=80+1000.0*(double)begin/(double)max,width=1000.0*(double)(end-begin)/(double)max;
        fprintf(html,"<rect x='%.4f' y='%d' width='%.4f' height='30' fill='hsl(%u 65%% 45%%)'><title>Nodo %d, tarea %"PRIu32", [ %"PRIu64", %"PRIu64" )</title></rect>",x,node*70+30,width,(unsigned)((id%360)*137%360),node,id,begin,end);
    }
    fprintf(html,"<text x='80' y='170'>0</text><text x='990' y='170'>%"PRIu64" ticks</text></svg></html>\n",max);
    bool bad=ferror(events)||ferror(gantt)||ferror(statsfile)||ferror(html);
    if(fclose(events)!=0)bad=true;
    if(fclose(gantt)!=0)bad=true;
    if(fclose(statsfile)!=0)bad=true;
    if(fclose(html)!=0)bad=true;
    free(stats);
    return bad?1:0;
}
