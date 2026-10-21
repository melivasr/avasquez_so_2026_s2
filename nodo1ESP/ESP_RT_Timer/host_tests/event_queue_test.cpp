// Pruebas del búfer circular de eventos (sin ESP32).
// Compilar y ejecutar (desde la raíz del proyecto):
//   g++ -std=c++17 -Wall -Wextra -Isrc host_tests/event_queue_test.cpp src/event_queue.cpp -o /tmp/event_queue_test && /tmp/event_queue_test

#include <stdio.h>
#include <stdlib.h>
#include <deque>
#include <vector>

#include "lib/event_queue.h"

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failed;                                                    \
            printf("  FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                  \
    } while (0)

static EventRecord rec(uint8_t type, uint32_t tick, uint16_t id, uint16_t arg)
{
    return EventRecord{type, tick, id, arg};
}

static void test_layout()
{
    printf("formato de la carga\n");
    static EventRing r;
    event_ring_init(&r);
    CHECK(event_ring_push(&r, rec(0x02, 0x01020304u, 0x0A0B, 0x0C0D)));
    CHECK(event_ring_push(&r, rec(0x04, 7, 2, 1)));

    uint8_t out[512];
    const size_t n = event_ring_pop_payload(&r, out, sizeof(out));
    CHECK(n == 1 + 2 * EVENT_RECORD_SIZE);
    CHECK(out[0] == 2);
    const uint8_t first[9] = {0x02, 0x04, 0x03, 0x02, 0x01, 0x0B, 0x0A, 0x0D, 0x0C};
    for (int i = 0; i < 9; ++i) {
        CHECK(out[1 + i] == first[i]);
    }
    const uint8_t second[9] = {0x04, 7, 0, 0, 0, 2, 0, 1, 0};
    for (int i = 0; i < 9; ++i) {
        CHECK(out[10 + i] == second[i]);
    }
    CHECK(event_ring_count(&r) == 0);
    CHECK(event_ring_pop_payload(&r, out, sizeof(out)) == 0);
}

static void test_batching_and_capacity()
{
    printf("lotes y capacidad de salida\n");
    static EventRing r;
    event_ring_init(&r);
    for (uint32_t i = 0; i < 100; ++i) {
        CHECK(event_ring_push(&r, rec(1, i, 1, 0)));
    }

    uint8_t out[512];
    size_t n = event_ring_pop_payload(&r, out, sizeof(out));
    CHECK(n == 1 + EVENT_MAX_PER_FRAME * EVENT_RECORD_SIZE);
    CHECK(n <= 512);
    CHECK(out[0] == EVENT_MAX_PER_FRAME);
    n = event_ring_pop_payload(&r, out, sizeof(out));
    CHECK(out[0] == 100 - EVENT_MAX_PER_FRAME);
    CHECK(n == 1 + (100 - EVENT_MAX_PER_FRAME) * EVENT_RECORD_SIZE);

    // Capacidad de salida pequeña
    CHECK(event_ring_push(&r, rec(1, 1, 1, 0)));
    CHECK(event_ring_push(&r, rec(1, 2, 1, 0)));
    CHECK(event_ring_pop_payload(&r, out, 0) == 0);
    CHECK(event_ring_pop_payload(&r, out, 9) == 0);          // no cabe uno
    CHECK(event_ring_count(&r) == 2);                        // no se perdió nada
    CHECK(event_ring_pop_payload(&r, out, 10) == 10);        // cabe exactamente uno
    CHECK(out[0] == 1);
    CHECK(event_ring_count(&r) == 1);
    CHECK(event_ring_pop_payload(nullptr, out, 512) == 0);
    CHECK(event_ring_pop_payload(&r, nullptr, 512) == 0);
}

static void test_full_and_drop()
{
    printf("búfer lleno\n");
    static EventRing r;
    event_ring_init(&r);
    for (size_t i = 0; i < EVENT_QUEUE_CAPACITY; ++i) {
        CHECK(event_ring_push(&r, rec(1, static_cast<uint32_t>(i), 1, 0)));
    }
    CHECK(!event_ring_push(&r, rec(1, 999, 1, 0)));
    CHECK(!event_ring_push(&r, rec(1, 998, 1, 0)));
    CHECK(r.dropped == 2);
    CHECK(event_ring_count(&r) == EVENT_QUEUE_CAPACITY);

    // Se conserva lo más antiguo (descarta el nuevo): el primero sigue siendo tick 0.
    uint8_t out[512];
    event_ring_pop_payload(&r, out, sizeof(out));
    CHECK(out[1] == 1 && out[2] == 0 && out[3] == 0);
    CHECK(event_ring_push(&r, rec(1, 5000, 1, 0)));          // ya hay espacio

    event_ring_init(&r);
    CHECK(r.dropped == 0 && event_ring_count(&r) == 0);
    CHECK(!event_ring_push(nullptr, rec(1, 1, 1, 0)));
}

static void test_wraparound_against_model()
{
    printf("vuelta del anillo contra un modelo\n");
    static EventRing r;
    event_ring_init(&r);
    std::deque<EventRecord> model;
    uint32_t next = 0;
    uint32_t model_dropped = 0;
    srand(12345);

    for (int step = 0; step < 20000; ++step) {
        if (rand() % 3 != 0) {
            const EventRecord e = rec(static_cast<uint8_t>(1 + next % 8), next,
                                      static_cast<uint16_t>(next * 7),
                                      static_cast<uint16_t>(next * 13));
            ++next;
            const bool ok = event_ring_push(&r, e);
            if (model.size() < EVENT_QUEUE_CAPACITY) {
                CHECK(ok);
                model.push_back(e);
            } else {
                CHECK(!ok);
                ++model_dropped;
            }
        } else {
            uint8_t out[512];
            const size_t cap = 10 + static_cast<size_t>(rand() % 500);
            const size_t len = event_ring_pop_payload(&r, out, cap);
            if (model.empty()) {
                CHECK(len == 0);
                continue;
            }
            const size_t n = out[0];
            CHECK(len == 1 + n * EVENT_RECORD_SIZE);
            CHECK(len <= cap);
            CHECK(n >= 1 && n <= EVENT_MAX_PER_FRAME);
            for (size_t i = 0; i < n; ++i) {
                const EventRecord &m = model.front();
                const uint8_t *p = out + 1 + i * EVENT_RECORD_SIZE;
                const uint32_t tick = p[1] | (p[2] << 8) | (p[3] << 16) |
                                      (static_cast<uint32_t>(p[4]) << 24);
                const uint16_t id = static_cast<uint16_t>(p[5] | (p[6] << 8));
                const uint16_t arg = static_cast<uint16_t>(p[7] | (p[8] << 8));
                if (p[0] != m.type || tick != m.tick || id != m.id || arg != m.arg) {
                    CHECK(false);
                    return;
                }
                model.pop_front();
            }
        }
        CHECK(event_ring_count(&r) == model.size());
    }
    CHECK(r.dropped == model_dropped);
}

int main()
{
    test_layout();
    test_batching_and_capacity();
    test_full_and_drop();
    test_wraparound_against_model();
    printf("\n%d comprobaciones, %d fallos\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}