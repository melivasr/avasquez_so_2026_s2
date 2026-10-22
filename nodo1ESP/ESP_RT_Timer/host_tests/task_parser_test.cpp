// Pruebas del parser de líneas de tarea (sin ESP32).
// Compilar y ejecutar (desde la raíz del proyecto):
//   g++ -std=c++17 -Wall -Wextra -Isrc host_tests/task_parser_test.cpp src/task_parser.cpp -o /tmp/task_parser_test && /tmp/task_parser_test

#include <stdio.h>
#include <string.h>
#include <string>

#include "lib/task_parser.h"

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

static bool parses(const std::string &line)
{
    Task t = {};
    return parse_task_line(line.c_str(), &t);
}

static void test_valid()
{
    printf("líneas válidas\n");
    Task t = {};
    CHECK(parse_task_line("3,P,40,20,20,2", &t));
    CHECK(t.id == 3 && t.type == TaskType::Periodic && t.W == 40 &&
          t.T == 20 && t.D == 20 && t.criticality == 2);
    CHECK(!t.job_active && t.remaining == 0);

    CHECK(parse_task_line("5,S,6,15,10,3", &t));
    CHECK(t.id == 5 && t.type == TaskType::Sporadic && t.W == 6 &&
          t.T == 15 && t.D == 10 && t.criticality == 3);

    // W máximo de 32 bits y criticidad 0 se aceptan.
    CHECK(parse_task_line("3,P,4294967295,20,20,0", &t));
    CHECK(t.W == 4294967295u && t.criticality == 0);

    // Límite de la línea: 95 caracteres se aceptan, 96 no (W rellena con ceros).
    const std::string s95 = "9,P," + std::string(82, '0') + "1,10,10,1";
    const std::string s96 = "9,P," + std::string(83, '0') + "1,10,10,1";
    CHECK(s95.size() == 95 && s96.size() == 96);
    CHECK(parses(s95));
    CHECK(!parses(s96));
}

static void test_rejected()
{
    printf("líneas que deben rechazarse\n");
    const char *bad[] = {
        "",
        "3,P,40,20,20",          // faltan campos
        "3,P,40,20,20,2,9",      // campo extra
        ",3,P,40,20,20,2",       // coma inicial
        "3,P,40,20,20,2,",       // coma final
        "3,P,,20,20,2",          // campo vacío (coma doble)
        "3,,40,20,20,2",         // tipo vacío
        "3,P,40,20,20,",         // último campo vacío
        ",,,,,",                 // todos vacíos
        " 3,P,40,20,20,2",       // espacio inicial
        "3,P,40,20,20,2 ",       // espacio final
        "3,P,40,20,20,2\r",      // CR final
        "+3,P,40,20,20,2",       // signo
        "-3,P,40,20,20,2",
        "3,p,40,20,20,2",        // tipo en minúscula
        "3,PP,40,20,20,2",       // tipo de dos letras
        "3,X,40,20,20,2",        // tipo inválido
        "0,P,40,20,20,2",        // id 0
        "3,P,0,20,20,2",         // W = 0
        "3,P,40,0,20,2",         // T = 0
        "3,P,40,20,0,2",         // D = 0
        "3,P,4294967296,20,20,2",   // desborda 32 bits
        "99999999999,P,1,1,1,1",    // 11 dígitos
        "a,P,40,20,20,2",        // id no numérico
    };
    for (const char *line : bad) {
        ++g_checks;
        if (parses(line)) {
            ++g_failed;
            printf("  FALLO: se aceptó \"%s\"\n", line);
        }
    }
}

static void test_arguments()
{
    printf("argumentos nulos y salida intacta ante error\n");
    Task t;
    memset(&t, 0xAB, sizeof(t));
    Task ref;
    memset(&ref, 0xAB, sizeof(ref));
    CHECK(!parse_task_line("3,P,,20,20,2", &t));
    CHECK(memcmp(&t, &ref, sizeof(t)) == 0);   // no se toca 'out' si falla

    CHECK(!parse_task_line(nullptr, &t));
    CHECK(!parse_task_line("3,P,40,20,20,2", nullptr));
}

int main()
{
    test_valid();
    test_rejected();
    test_arguments();
    printf("\n%d comprobaciones, %d fallos\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}