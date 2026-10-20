#include <stdint.h>

volatile uint32_t sink = 0;

void unidad_trabajo(void) {
    uint32_t x = sink;

    for (int i = 0; i < 1000; i++) {
        x = x * 1664525u + 1013904223u;
        x ^= x >> 16;
    }

    sink = x;
}
