#ifndef NODE_RUNTIME_H
#define NODE_RUNTIME_H

// Ejecución del nodo: tarea calendarizadora (núcleo 1), temporizador de
// hardware, LEDs y orquestación de Calibrar / Start / Stop.
//
// Las funciones node_calibrate/node_start/node_stop las llama la tarea de
// recepción UART (un solo llamador a la vez). Bloquean hasta que la tarea
// calendarizadora termina la orden.

#include <stddef.h>
#include <stdint.h>

#include "sched_core.h"
#include "task.h"

// Códigos de estado; coinciden con los estados del ACK (README_NODO.md).
constexpr uint8_t NODE_OK = 0;
constexpr uint8_t NODE_REJECTED = 1;
constexpr uint8_t NODE_BAD_STATE = 3;

struct NodeStats {
    uint32_t ticks_lost;          // ticks que no se alcanzaron a procesar
    uint32_t max_busy_us;         // mayor tiempo de proceso dentro de un tick
    uint32_t max_wake_latency_us; // mayor retraso entre la ISR y el despertar
    uint32_t events_dropped;      // eventos descartados por búfer lleno
};

// Configura LEDs y crea la tarea calendarizadora. false si falla
// (en ese caso se encienden los 5 LEDs).
bool node_runtime_init(void);

// Calibra en el núcleo del calendarizador con el tick indicado (1 ms..1 s).
// NODE_BAD_STATE si el nodo está en ejecución.
uint8_t node_calibrate(uint32_t tick_us, uint32_t *ut_per_tick_out);

// Inicia la ejecución con las tareas dadas (se copian). Tick cero = ahora.
// NODE_BAD_STATE si ya está en ejecución o no hay calibración.
// report_n: periodo del TickReport en ticks (0 = desactivado).
uint8_t node_start(Algorithm algorithm, uint16_t report_n,
                   const Task *tasks, size_t count);

// Detiene la ejecución y transmite todos los eventos pendientes antes de
// volver (el ACK de Stop sale después). NODE_BAD_STATE si no estaba en ejecución.
uint8_t node_stop(void);

// Agrega una tarea con el nodo en ejecución. La aplica la tarea calendarizadora
// entre dos ticks y emite TASK_ADDED. Bloquea hasta ~un tick.
// NODE_OK = aplicada (slot_out = ranura 1..8); NODE_REJECTED = la tabla del
// calendarizador la rechazó; NODE_BAD_STATE = el nodo no está en ejecución.
uint8_t node_add_task(const Task *task, uint16_t *slot_out);

// Registra una pulsación de la tarea esporádica pedida por el host (equivale
// al botón). NODE_OK = registrada (el calendarizador decide en el siguiente
// tick si libera o ignora; se informa por Event); NODE_REJECTED = el nodo no
// tiene tarea esporádica; NODE_BAD_STATE = no está en ejecución.
uint8_t node_sporadic_release(void);

bool node_is_running(void);
NodeStats node_stats(void);

#endif