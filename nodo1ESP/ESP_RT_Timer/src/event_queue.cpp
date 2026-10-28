#include "lib/event_queue.h"

// Anillo (búfer circular) de eventos en RAM. No tiene lock propio: quien lo
// usa (event_tx.cpp) es responsable de protegerlo.
//   head = donde se escribe el próximo evento
//   tail = donde se lee el próximo evento
//   count = cuántos hay guardados
//   dropped = cuántos se perdieron por estar lleno

void event_ring_init(EventRing *ring)
{
    if (ring == nullptr) {
        return;
    }
    ring->head = 0;
    ring->tail = 0;
    ring->count = 0;
    ring->dropped = 0;
}

// Si el anillo está lleno se descarta el evento nuevo (se conservan los más
// viejos) y se cuenta en 'dropped'. Así el Gantt nunca tiene huecos en el
// medio; en el peor caso se corta al final.
bool event_ring_push(EventRing *ring, const EventRecord &record)
{
    if (ring == nullptr) {
        return false;
    }
    if (ring->count >= EVENT_QUEUE_CAPACITY) {
        ring->dropped++;
        return false;
    }

    ring->items[ring->head] = record;
    ring->head = (ring->head + 1) % EVENT_QUEUE_CAPACITY;
    ring->count++;
    return true;
}

// Saca hasta EVENT_MAX_PER_FRAME eventos y los deja serializados en 'out' con
// el formato de la carga de Event: [cantidad][registro x cantidad].
// Cada registro mide 9 bytes: tipo(1) tick(4) id(2) arg(2), little endian.
// Devuelve cuántos bytes escribió, o 0 si no hay nada o 'out' es muy chico.
size_t event_ring_pop_payload(EventRing *ring, uint8_t *out, size_t out_capacity)
{
    if (ring == nullptr || out == nullptr || ring->count == 0 ||
        out_capacity < 1 + EVENT_RECORD_SIZE) {
        return 0;
    }

    size_t n = ring->count;
    if (n > EVENT_MAX_PER_FRAME) {
        n = EVENT_MAX_PER_FRAME;
    }
    // También se limita a lo que quepa en el búfer de salida.
    const size_t fit = (out_capacity - 1) / EVENT_RECORD_SIZE;
    if (n > fit) {
        n = fit;
    }

    out[0] = static_cast<uint8_t>(n);
    size_t pos = 1;

    for (size_t i = 0; i < n; ++i) {
        const EventRecord &e = ring->items[ring->tail];

        out[pos + 0] = e.type;
        out[pos + 1] = static_cast<uint8_t>(e.tick & 0xFFU);
        out[pos + 2] = static_cast<uint8_t>((e.tick >> 8) & 0xFFU);
        out[pos + 3] = static_cast<uint8_t>((e.tick >> 16) & 0xFFU);
        out[pos + 4] = static_cast<uint8_t>((e.tick >> 24) & 0xFFU);
        out[pos + 5] = static_cast<uint8_t>(e.id & 0xFFU);
        out[pos + 6] = static_cast<uint8_t>((e.id >> 8) & 0xFFU);
        out[pos + 7] = static_cast<uint8_t>(e.arg & 0xFFU);
        out[pos + 8] = static_cast<uint8_t>((e.arg >> 8) & 0xFFU);
        pos += EVENT_RECORD_SIZE;

        ring->tail = (ring->tail + 1) % EVENT_QUEUE_CAPACITY;
        ring->count--;
    }

    return pos;
}

size_t event_ring_count(const EventRing *ring)
{
    return (ring != nullptr) ? ring->count : 0;
}