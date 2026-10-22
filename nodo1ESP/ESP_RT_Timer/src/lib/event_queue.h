#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

// Búfer circular de eventos y empaquetado de la carga útil de la trama Event.
// Lógica pura (sin ESP-IDF): NO es seguro entre tareas por sí mismo; quien lo
// use debe protegerlo con una sección crítica (ver event_tx.cpp).

#include <stddef.h>
#include <stdint.h>

constexpr size_t EVENT_RECORD_SIZE = 9;       // [tipo 1B][tick 4B LE][id 2B LE][arg 2B LE]
constexpr size_t EVENT_QUEUE_CAPACITY = 256;  // registros
constexpr size_t EVENT_MAX_PER_FRAME = 56;    // 1 + 56 * 9 = 505 <= 512

// Valores de la tabla de eventos de README_NODO.md (9.5).
enum class NodeEventType : uint8_t {
    Release = 0x01,
    Switch = 0x02,
    Complete = 0x03,
    Miss = 0x04,
    SporadicIgnored = 0x05,
    TaskAdded = 0x06,
    TickLost = 0x07,
    BadFrame = 0x08
};

struct EventRecord {
    uint8_t type;
    uint32_t tick;
    uint16_t id;
    uint16_t arg;
};

struct EventRing {
    EventRecord items[EVENT_QUEUE_CAPACITY];
    size_t head;      // próxima escritura
    size_t tail;      // próxima lectura
    size_t count;
    uint32_t dropped; // registros descartados por búfer lleno
};

void event_ring_init(EventRing *ring);

// false (y dropped++) si el búfer está lleno: se descarta el registro nuevo.
bool event_ring_push(EventRing *ring, const EventRecord &record);

// Escribe [cantidad: 1 B][registros...] y los saca del búfer. Máximo
// EVENT_MAX_PER_FRAME registros y lo que quepa en out_capacity.
// Devuelve la longitud escrita, o 0 si no hay registros o no cabe uno.
size_t event_ring_pop_payload(EventRing *ring, uint8_t *out, size_t out_capacity);

size_t event_ring_count(const EventRing *ring);

#endif