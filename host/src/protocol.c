// Codifica tramas y reconstruye mensajes recibidos con verificación CRC.
#include "host.h"
#include <string.h>

// Los enteros multibyte se leen y escriben en little-endian.
uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}

// Extrae un byte por vez, empezando por los ocho bits menos significativos.
void put32(uint8_t *p,uint32_t x) {
    for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(x>>(8*i));
}

// CRC-16 CCITT-FALSE: inicio 0xFFFF y polinomio 0x1021.
uint16_t crc16(const uint8_t *p,size_t n) {
    uint16_t crc=0xffff;
    // Cada byte se combina con el acumulador y se procesa bit a bit.
    for(size_t i=0;i<n;i++) {
        crc^=(uint16_t)((uint16_t)p[i]<<8);
        for(int j=0;j<8;j++) crc=(uint16_t)((crc&0x8000)?(crc<<1)^0x1021:crc<<1);
    }
    return crc;
}

// Trama: A5 5A, tipo, longitud, carga y CRC; la marca inicial queda fuera del cálculo.
size_t frame_encode(const Frame *f,uint8_t *out) {
    if(f->length>PAYLOAD) return 0;
    out[0]=0xa5;
    out[1]=0x5a;
    out[2]=f->type;
    out[3]=(uint8_t)f->length;
    out[4]=(uint8_t)(f->length>>8);
    memcpy(out+5,f->data,f->length);
    uint16_t crc=crc16(out+2,3+f->length);
    out[5+f->length]=(uint8_t)crc;
    out[6+f->length]=(uint8_t)(crc>>8);
    return f->length+7u;
}

// memmove conserva los bytes pendientes aunque origen y destino se solapen.
static void discard(Parser *p,size_t n) {
    memmove(p->bytes,p->bytes+n,p->used-n);
    p->used-=n;
}

// Conserva tramas parciales; ante longitud o CRC inválido busca otra cabecera byte a byte.
bool parser_byte(Parser *p,uint8_t b,Frame *f) {
    if(p->used==sizeof p->bytes) {
        p->errors++;
        discard(p,1);
    }
    p->bytes[p->used++]=b;
    for(;;) {
        if(!p->used) return false;
        if(p->bytes[0]!=0xa5) {
            discard(p,1);
            continue;
        }
        if(p->used<2) return false;
        if(p->bytes[1]!=0x5a) {
            discard(p,1);
            continue;
        }
        if(p->used<5) return false;
        // Con cinco bytes ya puede leer la longitud y decidir cuántos bytes faltan.
        size_t n=(size_t)p->bytes[3]|(size_t)p->bytes[4]<<8;
        if(n>PAYLOAD) {
            p->errors++;
            discard(p,1);
            continue;
        }
        // Una trama completa ocupa n bytes de carga y siete entre cabecera y CRC.
        if(p->used<n+7) return false;
        uint16_t expected=(uint16_t)(p->bytes[n+5]|(unsigned)p->bytes[n+6]<<8);
        // Si no coincide el CRC, no entrega la trama y vuelve a buscar una marca de inicio.
        if(expected!=crc16(p->bytes+2,n+3)) {
            p->errors++;
            discard(p,1);
            continue;
        }
        // Solo copia el resultado al driver después de validar longitud y CRC.
        f->type=p->bytes[2];
        f->length=(uint16_t)n;
        memcpy(f->data,p->bytes+5,n);
        discard(p,n+7);
        return true;
    }
}

// Descarta y cuenta la trama que quedó incompleta más tiempo del permitido.
void parser_expire(Parser *p) {
    if(p->used) {
        p->errors++;
        p->used=0;
    }
}
