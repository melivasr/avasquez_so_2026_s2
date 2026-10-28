#include "task_parser.h"
#include "scheduler/scheduler_rms.h"
#include "scheduler/scheduler_edf.h"
#include <cassert>
#include <cstring>
#include <cstdio>

static Task parse(const char *line) {
    Task task={}; assert(parse_task_line(line,strlen(line),&task)); return task;
}

int main() {
    Task original=parse("1,P,20,20,20,-2147483648");
    assert(original.criticality==INT32_MIN);
    Task added=parse("65535,P,5,3,3,2147483647");
    const char *bad[]={"1,P,1,1,1,2147483648","1,P,1,1,1,-2147483649",
                       "65536,P,1,1,1,0","1,P,1,1,1,+1","1,P,1,1,1,-"};
    for(const char *line:bad) {
        Task task={}; assert(!parse_task_line(line,strlen(line),&task));
    }
    const char embedded[]="1,P,1,1,1,2\0junk";
    Task task={}; assert(!parse_task_line(embedded,sizeof embedded-1,&task));
    SchedulerRms rms;
    assert(rms.configure(&original,1,5));
    TickDecision first=rms.tick(0);
    assert(first.task_id==1 && first.units==5 && !first.completed);
    assert(rms.add_task(added,1));
    assert(!rms.add_task(added,1));
    auto next=rms.tick(1);
    assert(next.task_id==65535 && next.completed);
    unsigned original_units=5;
    for(uint32_t k=2;k<6;++k) {
        auto d=rms.tick(k);
        if(d.task_id==1)original_units+=d.units;
    }
    assert(original_units==20); /* Se conserva el progreso del trabajo anterior. */
    Task sporadic=parse("2,S,1,10,10,-1");
    assert(rms.add_task(sporadic,6));
    sporadic.id=3; assert(!rms.add_task(sporadic,6));
    SchedulerEdf edf;
    assert(edf.configure(nullptr,0,5));
    assert(edf.add_task(added,7));
    assert(edf.tick(6).task_id==NO_TASK);
    assert(edf.tick(7).task_id==65535);
    puts("Pico: parser firmado e inserción sin reiniciar trabajos correctos");
}
