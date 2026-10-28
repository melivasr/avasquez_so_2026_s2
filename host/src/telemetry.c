// Valida telemetría serial y la publica como estado o eventos internos.
#include "host.h"
#include <inttypes.h>

static uint32_t get16(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8);
}

// Calibrar requiere 50 ticks; STOP y ADD_TASK reservan un tick más margen.
uint32_t command_timeout_ms(const Config *cfg,uint8_t type) {
    uint64_t needed=cfg->timeout_ms;

    uint64_t minimum=type==CAL_REQ?
        ((uint64_t)cfg->tick_us*RT_CAL_WINDOW_TICKS+999)/1000+1000:
        (type==STOP || type==ADD_TASK)?((uint64_t)cfg->tick_us+999)/1000+1000:0;
    if(minimum>needed)needed=minimum;
    return (uint32_t)needed;
}

// START lleva algoritmo e intervalo de reportes usando los valores del contrato común.
Frame start_frame(const Config *cfg) {
    Frame f={.type=START,.length=RT_START_SIZE};
    f.data[0]=(uint8_t)(cfg->algorithm==RMS?RT_RMS:RT_EDF);
    f.data[1]=(uint8_t)cfg->report_ticks;
    f.data[2]=(uint8_t)(cfg->report_ticks>>8);
    return f;
}

// TICK mantiene la vigilancia del nodo; EVENT contiene la ejecución detallada.
bool receive_telemetry(Shared *s,int node,const Frame *f) {
    if(f->type==TICK && f->length==RT_TICK_REPORT_SIZE) {
        Record r={.kind=R_TICK,.node=node,.tick=get32(f->data),.frame=*f};

        // Intenta actualizar el heartbeat bajo lock antes de encolar su copia para el reporte.
        if(sem_wait_ms(&s->lock,20)==0) {
            s->last_tick_ns[node]=monotonic_ns();
            s->last_tick[node]=r.tick;
            sem_post(&s->lock);
        }
        // max_wake_us mide retraso al atender el timer; no mide latencia desde la pulsación del botón.
        snprintf(r.text,sizeof r.text,
                 "misses=%"PRIu32" ignored=%"PRIu32" ticks_lost=%"PRIu32
                 " events_dropped=%"PRIu32" bad_frames=%"PRIu32
                 " max_busy_us=%"PRIu32" max_wake_us=%"PRIu32,
                 get16(f->data+4),get16(f->data+6),get16(f->data+8),
                 get16(f->data+10),get16(f->data+12),get32(f->data+14),get32(f->data+18));
        emit(s,&r);
        return true;
    }
    if(f->type!=EVENT || f->length<1)return false;
    // El primer byte indica cuántos eventos vienen; cada registro debe ocupar nueve bytes.
    size_t count=f->data[0];
    if(!count || count>RT_EVENT_MAX_BATCH ||
       f->length!=1+count*RT_EVENT_RECORD_SIZE)return false;

    // Valida el lote completo antes de publicar eventos para no aplicar solo una parte.
    for(size_t i=0;i<count;i++) {
        uint8_t code=f->data[1+i*RT_EVENT_RECORD_SIZE];
        if(code<RT_RELEASE || code>RT_BAD_FRAME)return false;
    }
    for(size_t i=0;i<count;i++) {
        const uint8_t *p=f->data+1+i*RT_EVENT_RECORD_SIZE;
        Record r={.kind=R_NODE,.node=node,.code=p[0],.tick=get32(p+1),
                  .id=get16(p+5),.value=get16(p+7)};
        emit(s,&r);
    }
    return true;
}
