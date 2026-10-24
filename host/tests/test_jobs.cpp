#include <cassert>
#include <cstdio>
#include <vector>
#ifdef TEST_ESP
#include "lib/sched_core.h"
#else
#include "scheduler/scheduler_edf.h"
#include "scheduler/scheduler_rms.h"
#endif

struct Event { unsigned type; uint32_t tick,id,job; };
struct Harness {
    std::vector<Event> events;
#ifdef TEST_ESP
    Scheduler scheduler;
    static void sink(const SchedEvent *e,void *ctx) {
        static_cast<Harness *>(ctx)->events.push_back({(unsigned)e->type,e->tick,e->id,e->arg});
    }
    explicit Harness(bool edf=true) {
        assert(sched_init(&scheduler,edf?Algorithm::EDF:Algorithm::RMS,1,sink,nullptr,this));
    }
#else
    SchedulerEdf edf;
    SchedulerRms rms;
    Scheduler *scheduler;
    explicit Harness(bool use_edf=true):scheduler(use_edf?static_cast<Scheduler *>(&edf):&rms) {
        assert(scheduler->configure(nullptr,0,1));
    }
#endif
    bool add(uint32_t id,char type,uint32_t w,uint32_t t,uint32_t d,uint32_t first=0) {
        Task task={};task.id=id;task.type=(TaskType)type;task.W=w;task.T=t;task.D=d;
#ifdef TEST_ESP
        scheduler.next_tick=first;
        return sched_add_task(&scheduler,&task,nullptr);
#else
        return scheduler->add_task(task,first);
#endif
    }
    void tick(uint32_t k,unsigned presses=0) {
#ifdef TEST_ESP
        scheduler.next_tick=k;sched_sporadic_press(&scheduler,presses);sched_tick(&scheduler);
#else
        scheduler->sporadic_press(presses);
        auto decision=scheduler->tick(k);
        assert(decision.missed_count<=RT_MAX_JOBS);
        assert(decision.event_count<=SCHED_MAX_EVENTS_PER_TICK);
        for(size_t i=0;i<decision.event_count;i++) {
            const auto &e=decision.events[i];events.push_back({(unsigned)e.type,e.tick,e.task_id,e.value});
        }
#endif
    }
    std::vector<Event> select(unsigned type,uint32_t id=0) const {
        std::vector<Event> result;
        for(auto e:events)if(e.type==type&&(!id||e.id==id))result.push_back(e);
        return result;
    }
};

static void periodic_overlap() {
    Harness h;
    assert(h.add(1,'P',2,3,10));assert(h.add(2,'P',2,6,2));
    for(unsigned k=0;k<24;k++)h.tick(k);
    assert(h.select(RT_MISS).empty());
    auto done=h.select(RT_COMPLETE,1);
    assert(done.size()==8&&done[0].tick==4&&done[0].job==1&&done[1].tick==6&&done[1].job==2);
    for(bool edf:{false,true}) {
        Harness overloaded(edf);
        assert(overloaded.add(1,'P',3,2,7));assert(overloaded.add(2,'P',1,1,1));
        for(unsigned k=0;k<8;k++)overloaded.tick(k);
        auto miss=overloaded.select(RT_MISS,1);
        assert(miss.size()==1&&miss[0].tick==7&&miss[0].job==1);
    }
}

static void sporadic_overlap() {
    for(bool edf:{false,true}) {
        Harness h(edf);assert(h.add(1,'S',4,2,10));
        for(unsigned k=0;k<8;k++)h.tick(k,k==0||k==2);
        auto done=h.select(RT_COMPLETE);
        assert(h.select(RT_MISS).empty());
        assert(done.size()==2&&done[0].tick==4&&done[0].job==1&&done[1].tick==8&&done[1].job==2);
    }
}

static void large_times_and_capacity() {
    Harness h;h.tick(0);
    assert(h.add(1,'P',3,UINT32_MAX,UINT32_MAX,1));
    assert(h.add(2,'P',1,100,2,1));
    for(unsigned k=1;k<6;k++)h.tick(k);
    assert(h.select(RT_MISS).empty());
    assert(h.select(RT_COMPLETE,2)[0].tick==2);
    assert(h.select(RT_COMPLETE,1)[0].tick==5);
    Harness wrap;wrap.tick(UINT32_MAX-1);
    assert(wrap.add(1,'P',3,10,5,UINT32_MAX));
    wrap.tick(UINT32_MAX);wrap.tick(0);wrap.tick(1);
    assert(wrap.select(RT_MISS).empty());
    auto done=wrap.select(RT_COMPLETE);
    assert(done.size()==1&&done[0].tick==2);
    Harness capacity;
    assert(capacity.add(1,'P',1,1,RT_MAX_JOBS-1));
    assert(!capacity.add(2,'P',1,10,10));
    assert(!capacity.add(3,'P',3,10,UINT32_MAX));
    Harness limits;
    assert(!limits.add(3,'P',3,10,UINT32_MAX));
    assert(limits.add(3,'P',3,UINT32_MAX,UINT32_MAX)); // Rechazo anterior no ocupa ID/ranura.
    Harness burst;
    for(unsigned i=1;i<=8;i++)assert(burst.add(i,'P',10000,1,7));
    for(unsigned k=0;k<20;k++)burst.tick(k);
    const size_t old_misses=burst.select(RT_MISS).size();
    burst.tick(50);
    assert(burst.select(RT_MISS).size()-old_misses==56);
    for(unsigned k=51;k<100;k++)burst.tick(k);
}

int main() {
    periodic_overlap();sporadic_overlap();large_times_and_capacity();
    puts("Trabajos: EDF/RMS, solapamiento periódico/esporádico, plazos uint64, vuelta de tick y capacidad correctos");
}
