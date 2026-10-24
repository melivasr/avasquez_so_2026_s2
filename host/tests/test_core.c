#include "host.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void tasks_and_admission(void) {
    Task t;
    char error[256];
    assert(task_parse("65535,P,4294967295,20,10,-2147483648",&t,error,sizeof error));
    assert(t.id==65535 && t.W==UINT32_MAX && t.criticality==INT32_MIN);
    const char *bad[]={"65536,P,1,1,1,0","0,P,1,1,1,0","1,P,1,1,1,2147483648",
                       "1,P,1,1,1,-2147483649","1,P,,1,1,0","1,P,1,1,1,+1"};
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++)assert(!task_parse(bad[i],&t,error,sizeof error));
    assert(task_cost(UINT32_MAX,2)==2147483648U);
    Node node={.operational=true,.speed=10,.count=1,
               .tasks={{.id=1,.W=50,.T=10,.D=10,.type='P'}}};
    t=(Task){.id=2,.W=40,.T=10,.D=10,.type='P'};
    assert(admission(&node,&t,EDF)==NULL);
    assert(admission(&node,&t,RMS)!=NULL);
    Node nodes[2]={node,{.operational=true,.speed=5}};
    assert(place(nodes,&t,EDF,WORST_FIT,error,sizeof error)==1);
    nodes[0].tasks[0].criticality=9;
    nodes[0].tasks[1]=(Task){.id=3,.W=10,.T=10,.D=10,.type='S'};
    nodes[0].count=2;
    Task lost[NODE_CAP]; bool keep[NODE_CAP];
    assert(migrate_plan(nodes,0,EDF,FIRST_FIT,lost,keep)==2);
    assert(!keep[1]); /* Una esporádica no migra. */
    Node empty={.operational=true,.speed=1};
    t=(Task){.id=10,.type='P',.W=3,.T=UINT32_MAX,.D=UINT32_MAX};
    assert(admission(&empty,&t,EDF)==NULL); /* Plazo grande representable en uint64. */
    t.T=10;
    assert(strstr(admission(&empty,&t,EDF),"trabajos pendientes"));
}

static void wire_and_telemetry(void) {
    assert(crc16((const uint8_t *)"123456789",9)==0x29B1);
    Config cfg={.algorithm=RMS,.report_ticks=65535,.timeout_ms=100,.tick_us=1000000};
    Frame start=start_frame(&cfg);
    assert(start.type==4 && start.length==3 && start.data[0]==0);
    assert(start.data[1]==255 && start.data[2]==255);
    cfg.algorithm=EDF; assert(start_frame(&cfg).data[0]==1);
    assert(command_timeout_ms(&cfg,CAL_REQ)==51000);
    assert(command_timeout_ms(&cfg,STOP)==2000);
    assert(ADD_TASK==10);
    uint8_t bytes[FRAME_SIZE];
    size_t length=frame_encode(&start,bytes);
    Parser parser={0}; Frame decoded={0};
    for(size_t i=0;i<length;i++)assert(parser_byte(&parser,bytes[i],&decoded)==(i==length-1));
    assert(decoded.length==3 && !memcmp(decoded.data,start.data,3));
    bytes[length-1]^=1;
    for(size_t i=0;i<length;i++)assert(!parser_byte(&parser,bytes[i],&decoded));
    assert(parser.errors>0);

    Shared *s=calloc(1,sizeof *s); assert(s);
    assert(sem_init(&s->lock,1,1)==0 && queue_init(&s->events)==0);
    Frame batch={.type=EVENT,.length=19,.data={2,RT_RELEASE}};
    put32(batch.data+2,0x12345678);
    batch.data[6]=255; batch.data[7]=255;
    batch.data[8]=52; batch.data[9]=18;
    batch.data[10]=RT_SWITCH;
    put32(batch.data+11,0x12345678);
    batch.data[15]=255; batch.data[16]=255;
    assert(receive_telemetry(s,0,&batch));
    Record r;
    assert(queue_pop(&s->events,&r,0)==0);
    assert(r.code==RT_RELEASE && r.tick==0x12345678 && r.id==65535 && r.value==0x1234);
    assert(queue_pop(&s->events,&r,0)==0 && r.code==RT_SWITCH);
    batch.data[10]=99;
    assert(!receive_telemetry(s,0,&batch));
    assert(queue_pop(&s->events,&r,0)<0); /* Nada del lote inválido se publica. */
    batch.data[10]=RT_SWITCH; batch.length=18;
    assert(!receive_telemetry(s,0,&batch));
    Frame report={.type=TICK,.length=RT_TICK_REPORT_SIZE};
    put32(report.data,123); report.data[10]=7; put32(report.data+14,8000);
    assert(receive_telemetry(s,1,&report));
    assert(s->last_tick_ns[1]>0 && s->last_tick[1]==123);
    assert(queue_pop(&s->events,&r,0)==0 && r.kind==R_TICK && r.tick==123);
    assert(strstr(r.text,"events_dropped=7") && strstr(r.text,"max_busy_us=8000"));
    report.length=4;
    assert(!receive_telemetry(s,1,&report));
    queue_destroy(&s->events); sem_destroy(&s->lock); free(s);
}

int main(void) {
    tasks_and_admission();
    wire_and_telemetry();
    puts("Host: entradas, admisión, protocolo y telemetría correctos");
    return 0;
}
