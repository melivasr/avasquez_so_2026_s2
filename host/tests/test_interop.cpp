/* Se enlaza el codec original de cada nodo con el codec C del host. */
#define Frame HostFrame
#define Task HostTask
#define Node HostNode
#define Algorithm HostAlgorithm
extern "C" {
#include "host.h"
}
#undef Frame
#undef Task
#undef Node
#undef Algorithm
#undef NO_TASK
#ifdef TEST_PICO
#include "protocol.h"
#include "messages.h"
#else
#include "lib/protocol.h"
#include "lib/event_queue.h"
#include "lib/tick_report.h"
#endif
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static void telemetry() {
    Shared *shared=(Shared *)calloc(1,sizeof(Shared)); assert(shared);
    assert(sem_init(&shared->lock,1,1)==0 && queue_init(&shared->events)==0);
    Frame report={};
    TickReportData data={};
    data.tick=1234; data.deadline_misses=2; data.events_dropped=3;
    data.max_busy_us=8123; data.max_wake_latency_us=17;
#ifdef TEST_PICO
    build_tick_report(&report,&data);
    EventQueue queue;
    assert(queue.push({1230,EventType::Release,65535,15}));
    assert(queue.push({1230,EventType::Switch,65535,0}));
    Frame batch={}; build_event_batch(&batch,&queue,56);
#else
    report.type=MessageType::TickReport;
    report.length=(uint16_t)tick_report_encode(data,report.payload,sizeof report.payload);
    EventRing queue; event_ring_init(&queue);
    assert(event_ring_push(&queue,{RT_RELEASE,1230,65535,15}));
    assert(event_ring_push(&queue,{RT_SWITCH,1230,65535,0}));
    Frame batch={}; batch.type=MessageType::Event;
    batch.length=(uint16_t)event_ring_pop_payload(&queue,batch.payload,sizeof batch.payload);
#endif
    for(const Frame *node:{&report,&batch}) {
        HostFrame host={}; host.type=(uint8_t)node->type; host.length=node->length;
        memcpy(host.data,node->payload,node->length);
        assert(receive_telemetry(shared,1,&host));
    }
    Record record={};
    assert(queue_pop(&shared->events,&record,0)==0 && record.kind==R_TICK && record.tick==1234);
    assert(strstr(record.text,"max_busy_us=8123") && strstr(record.text,"events_dropped=3"));
    assert(queue_pop(&shared->events,&record,0)==0 && record.code==RT_RELEASE && record.id==65535 && record.value==15);
    assert(queue_pop(&shared->events,&record,0)==0 && record.code==RT_SWITCH && record.id==65535);
    queue_destroy(&shared->events); sem_destroy(&shared->lock); free(shared);
#ifdef TEST_PICO
    for(HostAlgorithm algorithm:{RMS,EDF}) {
        Config cfg={}; cfg.algorithm=algorithm; cfg.report_ticks=65535;
        HostFrame host=start_frame(&cfg); Frame node={};
        node.type=host.type; node.length=host.length; memcpy(node.payload,host.data,host.length);
        Algorithm parsed; uint16_t ticks=0;
        assert(parse_start(&node,&parsed,&ticks) && ticks==65535);
        assert(parsed==(algorithm==RMS?Algorithm::RMS:Algorithm::EDF));
    }
#endif
}

int main() {
    const uint16_t lengths[]={0,3,4,22,505,512};
    for(uint8_t type=1;type<=RT_ADD_TASK;++type)for(uint16_t length:lengths) {
        HostFrame host={}; host.type=type; host.length=length;
        for(size_t i=0;i<length;++i)host.data[i]=(uint8_t)((i*137+type)%256);
        uint8_t encoded[519],returned[519];
        const size_t size=frame_encode(&host,encoded);
        Frame node={};
        assert(decode_frame(encoded,size,&node));
        assert(node.length==length && !memcmp(node.payload,host.data,length));
#ifdef TEST_PICO
        assert(encode_frame(&node,returned,sizeof returned)==size);
#else
        size_t written=0;
        assert(encode_frame(node.type,node.payload,node.length,returned,sizeof returned,&written));
        assert(written==size);
#endif
        assert(!memcmp(returned,encoded,size));
        Parser parser={}; HostFrame parsed={};
        for(size_t i=0;i<size;++i)assert(parser_byte(&parser,returned[i],&parsed)==(i+1==size));
        assert(parsed.type==type && parsed.length==length);
        encoded[size-1]^=1;
        assert(!decode_frame(encoded,size,&node));
    }
    assert((uint8_t)MessageType::AddTask==RT_ADD_TASK);
    telemetry();
    puts("Interoperabilidad host/nodo: 60 tramas, CRC y telemetría original correctos");
}
