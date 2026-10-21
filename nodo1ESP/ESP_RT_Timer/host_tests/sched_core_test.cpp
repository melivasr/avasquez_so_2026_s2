// Pruebas del núcleo del calendarizador en el computador (sin ESP32).
// Compilar y ejecutar (desde la raíz del proyecto):
//   g++ -std=c++17 -Wall -Wextra -Isrc host_tests/sched_core_test.cpp src/sched_core.cpp -o /tmp/sched_core_test && /tmp/sched_core_test

#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "sched_core.h"

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

struct Recorder {
    std::vector<SchedEvent> events;
    std::vector<uint32_t> work_calls;
};

static void sink(const SchedEvent *e, void *ctx)
{
    static_cast<Recorder *>(ctx)->events.push_back(*e);
}

static void work(uint32_t ut, void *ctx)
{
    static_cast<Recorder *>(ctx)->work_calls.push_back(ut);
}

static Task mk(uint32_t id, char type, uint32_t W, uint32_t T, uint32_t D,
               uint32_t crit)
{
    Task t = {};
    t.id = id;
    t.type = (type == 'P') ? TaskType::Periodic : TaskType::Sporadic;
    t.W = W;
    t.T = T;
    t.D = D;
    t.criticality = crit;
    return t;
}

static void add(Scheduler *s, const Task &t)
{
    CHECK(sched_add_task(s, &t, nullptr));
}

static void run(Scheduler *s, int ticks)
{
    for (int i = 0; i < ticks; ++i) {
        sched_tick(s);
    }
}

static std::vector<SchedEvent> only(const Recorder &r, SchedEventType type)
{
    std::vector<SchedEvent> out;
    for (const SchedEvent &e : r.events) {
        if (e.type == type) {
            out.push_back(e);
        }
    }
    return out;
}

static void test_cost_and_slices()
{
    printf("costo y tramos\n");
    Recorder r;
    Scheduler s;
    CHECK(sched_init(&s, Algorithm::RMS, 70, sink, work, &r));

    uint16_t slot = 0;
    Task a = mk(1, 'P', 71, 100, 100, 1);
    CHECK(sched_add_task(&s, &a, &slot));
    CHECK(slot == 1);
    CHECK(s.slots[0].cost_ticks == 2);

    Task b = mk(2, 'P', 70, 100, 100, 1);
    Task c = mk(3, 'P', 1, 100, 100, 1);
    add(&s, b);
    add(&s, c);
    CHECK(s.slots[1].cost_ticks == 1);
    CHECK(s.slots[2].cost_ticks == 1);

    // W=25, sn=10 -> tramos de 10, 10, 5
    Recorder r2;
    Scheduler s2;
    CHECK(sched_init(&s2, Algorithm::EDF, 10, sink, work, &r2));
    add(&s2, mk(1, 'P', 25, 100, 100, 1));
    run(&s2, 5);
    CHECK(r2.work_calls.size() == 3);
    if (r2.work_calls.size() == 3) {
        CHECK(r2.work_calls[0] == 10);
        CHECK(r2.work_calls[1] == 10);
        CHECK(r2.work_calls[2] == 5);
    }
    std::vector<SchedEvent> done = only(r2, SchedEventType::Complete);
    CHECK(done.size() == 1);
    if (done.size() == 1) {
        CHECK(done[0].tick == 3);
    }
}

static void test_validation()
{
    printf("validación de tareas\n");
    Recorder r;
    Scheduler s;
    CHECK(!sched_init(&s, Algorithm::RMS, 0, sink, work, &r));
    CHECK(!sched_init(&s, static_cast<Algorithm>(7), 10, sink, work, &r));
    CHECK(sched_init(&s, Algorithm::RMS, 10, sink, work, &r));

    Task t = mk(1, 'P', 10, 10, 10, 1);
    CHECK(sched_add_task(&s, &t, nullptr));
    CHECK(!sched_add_task(&s, &t, nullptr));                 // id repetido
    Task z = mk(0, 'P', 10, 10, 10, 1);
    CHECK(!sched_add_task(&s, &z, nullptr));                 // id 0
    Task big = mk(65536, 'P', 10, 10, 10, 1);
    CHECK(!sched_add_task(&s, &big, nullptr));               // id > 65535
    Task ok = mk(65535, 'P', 10, 10, 10, 1);
    CHECK(sched_add_task(&s, &ok, nullptr));
    Task w0 = mk(9, 'P', 0, 10, 10, 1);
    CHECK(!sched_add_task(&s, &w0, nullptr));
    Task s1 = mk(10, 'S', 10, 10, 10, 1);
    Task s2 = mk(11, 'S', 10, 10, 10, 1);
    CHECK(sched_add_task(&s, &s1, nullptr));
    CHECK(!sched_add_task(&s, &s2, nullptr));                // segunda esporádica
    CHECK(!sched_add_task(&s, nullptr, nullptr));

    // Llenar hasta 8 y rechazar la novena.
    for (uint32_t id = 20; s.count < MAX_TASKS; ++id) {
        Task f = mk(id, 'P', 10, 10, 10, 1);
        CHECK(sched_add_task(&s, &f, nullptr));
    }
    CHECK(s.count == MAX_TASKS);
    Task extra = mk(99, 'P', 10, 10, 10, 1);
    CHECK(!sched_add_task(&s, &extra, nullptr));

    // Con nullptr no se rompe.
    sched_tick(nullptr);
}

static void test_rms_preemption()
{
    printf("RMS: apropiación a nivel de tick\n");
    Recorder r;
    Scheduler s;
    CHECK(sched_init(&s, Algorithm::RMS, 10, sink, work, &r));
    add(&s, mk(1, 'P', 10, 4, 4, 1));    // A: C=1, T=4
    add(&s, mk(2, 'P', 50, 10, 10, 1));  // B: C=5, T=10
    run(&s, 9);                          // ticks 0..8

    std::vector<SchedEvent> sw = only(r, SchedEventType::Switch);
    // (tick, id nuevo, id anterior)
    const uint32_t expect[][3] = {
        {0, 1, 0}, {1, 2, 1}, {4, 1, 2}, {5, 2, 1}, {7, 0, 2}, {8, 1, 0}};
    CHECK(sw.size() == 6);
    for (size_t i = 0; i < sw.size() && i < 6; ++i) {
        CHECK(sw[i].tick == expect[i][0]);
        CHECK(sw[i].id == expect[i][1]);
        CHECK(sw[i].arg == expect[i][2]);
    }

    bool b_done_at_7 = false;
    for (const SchedEvent &e : only(r, SchedEventType::Complete)) {
        if (e.id == 2 && e.tick == 7) {
            b_done_at_7 = true;
        }
    }
    CHECK(b_done_at_7);
    CHECK(s.miss_count == 0);
}

static void test_edf_only_workload()
{
    printf("carga que solo cumple EDF\n");
    // T1: C=2, T=D=5. T2: C=4, T=D=7. U = 0.971
    {
        Recorder r;
        Scheduler s;
        CHECK(sched_init(&s, Algorithm::RMS, 10, sink, work, &r));
        add(&s, mk(1, 'P', 20, 5, 5, 1));
        add(&s, mk(2, 'P', 40, 7, 7, 1));
        run(&s, 70);
        std::vector<SchedEvent> miss = only(r, SchedEventType::Miss);
        CHECK(!miss.empty());
        if (!miss.empty()) {
            CHECK(miss[0].tick == 7);
            CHECK(miss[0].id == 2);
        }
        CHECK(s.miss_count > 0);
    }
    {
        Recorder r;
        Scheduler s;
        CHECK(sched_init(&s, Algorithm::EDF, 10, sink, work, &r));
        add(&s, mk(1, 'P', 20, 5, 5, 1));
        add(&s, mk(2, 'P', 40, 7, 7, 1));
        run(&s, 70);
        CHECK(s.miss_count == 0);
        CHECK(only(r, SchedEventType::Miss).empty());
    }
}

static void test_tie_break()
{
    printf("desempate\n");
    // RMS: mismo T. Criticidad mayor primero; luego id menor.
    {
        Recorder r;
        Scheduler s;
        CHECK(sched_init(&s, Algorithm::RMS, 10, sink, work, &r));
        add(&s, mk(5, 'P', 10, 10, 10, 1));
        add(&s, mk(3, 'P', 10, 10, 10, 1));
        add(&s, mk(7, 'P', 10, 10, 10, 3));
        run(&s, 4);
        std::vector<SchedEvent> sw = only(r, SchedEventType::Switch);
        CHECK(sw.size() == 4);
        if (sw.size() == 4) {
            CHECK(sw[0].id == 7 && sw[0].tick == 0);
            CHECK(sw[1].id == 3 && sw[1].tick == 1);
            CHECK(sw[2].id == 5 && sw[2].tick == 2);
            CHECK(sw[3].id == 0 && sw[3].tick == 3);
        }
    }
    // EDF: mismo plazo absoluto. Igual regla.
    {
        Recorder r;
        Scheduler s;
        CHECK(sched_init(&s, Algorithm::EDF, 10, sink, work, &r));
        add(&s, mk(5, 'P', 10, 20, 10, 1));
        add(&s, mk(3, 'P', 10, 30, 10, 1));
        add(&s, mk(7, 'P', 10, 40, 10, 3));
        run(&s, 3);
        std::vector<SchedEvent> sw = only(r, SchedEventType::Switch);
        CHECK(sw.size() == 3);
        if (sw.size() == 3) {
            CHECK(sw[0].id == 7);
            CHECK(sw[1].id == 3);
            CHECK(sw[2].id == 5);
        }
    }
}

static void test_abort_and_continue()
{
    printf("aborto por plazo perdido\n");
    Recorder r;
    Scheduler s;
    CHECK(sched_init(&s, Algorithm::RMS, 10, sink, work, &r));
    add(&s, mk(1, 'P', 10, 5, 5, 1));     // B: siempre cumple
    add(&s, mk(2, 'P', 100, 10, 3, 1));   // A: C=10 > D=3
    run(&s, 25);

    std::vector<SchedEvent> miss = only(r, SchedEventType::Miss);
    CHECK(miss.size() == 3);
    if (miss.size() == 3) {
        CHECK(miss[0].id == 2 && miss[0].tick == 3);
        CHECK(miss[1].id == 2 && miss[1].tick == 13);
        CHECK(miss[2].id == 2 && miss[2].tick == 23);
    }

    int b_done = 0;
    int a_done = 0;
    for (const SchedEvent &e : only(r, SchedEventType::Complete)) {
        if (e.id == 1) ++b_done;
        if (e.id == 2) ++a_done;
    }
    CHECK(b_done == 5);
    CHECK(a_done == 0);
}

static void test_deadline_longer_than_period()
{
    printf("D > T: trabajo activo en la siguiente liberación\n");
    Recorder r;
    Scheduler s;
    CHECK(sched_init(&s, Algorithm::RMS, 10, sink, work, &r));
    add(&s, mk(1, 'P', 40, 2, 6, 1));   // C=4 > T=2
    run(&s, 3);
    std::vector<SchedEvent> miss = only(r, SchedEventType::Miss);
    CHECK(!miss.empty());
    if (!miss.empty()) {
        CHECK(miss[0].tick == 2);
        CHECK(miss[0].id == 1);
        CHECK(miss[0].arg == 1); // abortó el trabajo n.º 1
    }
}

static void test_sporadic_not_auto_released()
{
    printf("esporádica no se libera sola\n");
    Recorder r;
    Scheduler s;
    CHECK(sched_init(&s, Algorithm::EDF, 10, sink, work, &r));
    add(&s, mk(4, 'S', 10, 5, 5, 3));
    run(&s, 20);
    CHECK(only(r, SchedEventType::Release).empty());
    CHECK(r.work_calls.empty());
    CHECK(s.running_id == 0);
}

static void test_add_while_running()
{
    printf("agregar con el sistema en marcha\n");
    Recorder r;
    Scheduler s;
    CHECK(sched_init(&s, Algorithm::EDF, 10, sink, work, &r));
    add(&s, mk(1, 'P', 10, 10, 10, 1));
    run(&s, 4);                              // ticks 0..3
    add(&s, mk(2, 'P', 10, 10, 10, 1));      // se libera en el tick 4
    run(&s, 2);
    bool seen = false;
    for (const SchedEvent &e : only(r, SchedEventType::Release)) {
        if (e.id == 2) {
            CHECK(e.tick == 4);
            seen = true;
        }
    }
    CHECK(seen);
}

int main()
{
    test_cost_and_slices();
    test_validation();
    test_rms_preemption();
    test_edf_only_workload();
    test_tie_break();
    test_abort_and_continue();
    test_deadline_longer_than_period();
    test_sporadic_not_auto_released();
    test_add_while_running();

    printf("\n%d comprobaciones, %d fallos\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}