#include "lib/event_queue.h"

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