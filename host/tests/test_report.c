#include "host.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void event(Shared *s,int code,uint32_t tick,uint32_t id,uint32_t arg) {
    Record r={.kind=R_NODE,.node=0,.code=code,.tick=tick,.id=id,.value=arg};
    assert(queue_push(&s->events,&r,0)==0);
}

int main(void) {
    char directory[]="/tmp/so-report-XXXXXX";
    assert(mkdtemp(directory));
    Config config={0}; snprintf(config.output,sizeof config.output,"%s",directory);
    Shared *s=calloc(1,sizeof *s); assert(s);
    assert(queue_init(&s->events)==0 && sem_init(&s->lock,1,1)==0);
    Record task={.kind=R_TASK,.node=0,.code=1,.value=2,
                 .task={.id=1,.type='P',.W=10,.T=2,.D=2}};
    assert(queue_push(&s->events,&task,0)==0);
    event(s,RT_RELEASE,0,1,1); event(s,RT_SWITCH,0,1,0);
    /* En ESP32 un heartbeat puede adelantarse a lotes viejos. */
    Record tick={.kind=R_TICK,.node=0,.tick=5};
    assert(queue_push(&s->events,&tick,0)==0);
    event(s,RT_COMPLETE,2,1,1);
    /* Dos trabajos consecutivos de la misma tarea no requieren SWITCH. */
    event(s,RT_RELEASE,2,1,2); event(s,RT_COMPLETE,4,1,2);
    event(s,RT_SWITCH,4,0,1);
    event(s,RT_SPORADIC_IGNORED,5,1,0); /* arg=0 es motivo, no contador. */
    event(s,RT_TASK_ADDED,6,1,1);       /* No es SporadicIgnored. */
    event(s,RT_TICK_LOST,9,0,2);
    event(s,RT_RELEASE,9,1,3); event(s,RT_SWITCH,9,1,0); event(s,RT_COMPLETE,10,1,3);
    Record overlap={.kind=R_TASK,.node=0,.code=1,.value=4,
                    .task={.id=2,.type='P',.W=40,.T=3,.D=10}};
    assert(queue_push(&s->events,&overlap,0)==0);
    event(s,RT_RELEASE,12,2,1); event(s,RT_SWITCH,12,2,1);
    event(s,RT_RELEASE,14,2,2); /* El trabajo 1 todavía está ejecutándose. */
    Record duplicate={.kind=R_TASK,.node=-1,.code=TASK_ATTEMPT_REJECTED,
                      .task={.id=2,.type='P',.W=999,.T=1,.D=1}};
    assert(queue_push(&s->events,&duplicate,0)==0);
    event(s,RT_COMPLETE,16,2,1); event(s,RT_COMPLETE,20,2,2);
    Record stop={.kind=R_STOP,.node=0},end={.kind=R_END};
    assert(queue_push(&s->events,&stop,0)==0 && queue_push(&s->events,&end,0)==0);
    assert(report_run(s,&config)==0);
    char path[1024],line[1024];
    snprintf(path,sizeof path,"%s/gantt.csv",directory);
    FILE *f=fopen(path,"r"); assert(f);
    assert(fgets(line,sizeof line,f));
    const char *expected[]={"0,1,0,2\n","0,1,2,4\n","0,1,9,10\n",
                            "0,2,12,14\n","0,2,14,16\n","0,2,16,20\n"};
    for(size_t i=0;i<sizeof expected/sizeof *expected;i++)assert(fgets(line,sizeof line,f) && !strcmp(line,expected[i]));
    assert(!fgets(line,sizeof line,f)); fclose(f);
    snprintf(path,sizeof path,"%s/tasks.csv",directory);
    f=fopen(path,"r"); assert(f); assert(fgets(line,sizeof line,f));
    assert(fgets(line,sizeof line,f));
    assert(strstr(line,",3,5,3,0,1,3,0.000,3,1.667"));
    assert(fgets(line,sizeof line,f));
    assert(!strcmp(line,"0,2,P,40,3,10,0,4,2,8,2,0,0,2,1.000,2,5.000,6\n"));
    fclose(f);
    const char *files[]={"gantt.csv","events.csv","tasks.csv","report.html"};
    for(size_t i=0;i<4;i++) { snprintf(path,sizeof path,"%s/%s",directory,files[i]); unlink(path); }
    rmdir(directory); queue_destroy(&s->events); sem_destroy(&s->lock); free(s);
    puts("Reporte: Gantt, trabajos solapados, latencias, duplicados y ticks perdidos correctos");
    return 0;
}
