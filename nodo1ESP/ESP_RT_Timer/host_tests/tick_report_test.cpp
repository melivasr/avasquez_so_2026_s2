// Pruebas de la carga de TickReport y de su periodicidad (sin ESP32).
// Compilar y ejecutar (desde la raíz del proyecto):
//   g++ -std=c++17 -Wall -Wextra -Isrc host_tests/tick_report_test.cpp src/tick_report.cpp -o /tmp/tick_report_test && /tmp/tick_report_test

#include <stdio.h>

#include "lib/tick_report.h"

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

static void test_layout()
{
    printf("formato de la carga\n");
    TickReportData d = {};
    d.tick = 0x01020304u;
    d.deadline_misses = 0x0506;
    d.sporadic_ignored = 0x0708;
    d.ticks_lost = 0x090A;
    d.events_dropped = 0x0B0C;
    d.bad_frames = 0x0D0E;
    d.max_busy_us = 0x11121314u;
    d.max_wake_latency_us = 0x21222324u;

    uint8_t out[TICK_REPORT_SIZE];
    CHECK(tick_report_encode(d, out, sizeof(out)) == 22);
    const uint8_t want[22] = {
        0x04, 0x03, 0x02, 0x01,
        0x06, 0x05, 0x08, 0x07, 0x0A, 0x09, 0x0C, 0x0B, 0x0E, 0x0D,
        0x14, 0x13, 0x12, 0x11,
        0x24, 0x23, 0x22, 0x21};
    for (int i = 0; i < 22; ++i) {
        CHECK(out[i] == want[i]);
    }
}

static void test_limits()
{
    printf("saturación y capacidad\n");
    TickReportData d = {};
    d.deadline_misses = 70000;
    d.ticks_lost = 65535;
    d.events_dropped = 65536;
    d.bad_frames = 0xFFFFFFFFu;

    uint8_t out[TICK_REPORT_SIZE];
    CHECK(tick_report_encode(d, out, sizeof(out)) == 22);
    CHECK(out[4] == 0xFF && out[5] == 0xFF);   // 70000 satura
    CHECK(out[8] == 0xFF && out[9] == 0xFF);   // 65535 exacto
    CHECK(out[10] == 0xFF && out[11] == 0xFF); // 65536 satura
    CHECK(out[12] == 0xFF && out[13] == 0xFF);

    CHECK(tick_report_encode(d, out, 21) == 0);
    CHECK(tick_report_encode(d, nullptr, 22) == 0);
}

static void test_due()
{
    printf("periodicidad\n");
    uint32_t next = 20;
    for (uint32_t t = 0; t < 20; ++t) {
        CHECK(!tick_report_due(20, t, &next));
    }
    CHECK(tick_report_due(20, 20, &next));
    CHECK(next == 40);
    for (uint32_t t = 21; t < 40; ++t) {
        CHECK(!tick_report_due(20, t, &next));
    }
    CHECK(tick_report_due(20, 40, &next));
    CHECK(next == 60);

    // Salto por ticks perdidos: un solo reporte y vuelve al múltiplo siguiente.
    CHECK(tick_report_due(20, 95, &next));
    CHECK(next == 100);
    CHECK(!tick_report_due(20, 96, &next));

    // N = 1: en cada tick.
    uint32_t n1 = 1;
    for (uint32_t t = 1; t < 10; ++t) {
        CHECK(tick_report_due(1, t, &n1));
        CHECK(n1 == t + 1);
    }

    // N = 0 desactiva; parámetros nulos no rompen.
    uint32_t n0 = 0;
    CHECK(!tick_report_due(0, 1000, &n0));
    CHECK(!tick_report_due(20, 50, nullptr));
}

int main()
{
    test_layout();
    test_limits();
    test_due();
    printf("\n%d comprobaciones, %d fallos\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}