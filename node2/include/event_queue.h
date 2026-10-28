// Eventos que el nodo le reporta al host y la cola donde esperan su envío.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "../../common/rt_protocol.h"

#define EVENT_QUEUE_CAPACITY 256U // registros 

// id 0 = ninguna tarea (ocioso). Por eso una tarea no puede tener id 0.
#define NO_TASK 0U

// Tipos de evento 
enum class EventType : uint8_t {
    Release = RT_RELEASE,  // se liberó un trabajo; arg = número de trabajo
    Switch = RT_SWITCH,   // cambió la tarea en ejecución; arg = la anterior
    Complete = RT_COMPLETE, // terminó un trabajo; arg = número de trabajo
    Miss = RT_MISS,  // plazo vencido, trabajo abortado; arg = número de trabajo
    SporadicIgnored = RT_SPORADIC_IGNORED, // pulsación antes del intervalo mínimo
    TaskAdded = RT_TASK_ADDED,  // arg = ranura 1..8
    TickLost = RT_TICK_LOST,  // ticks no procesados; arg = cuántos
    BadFrame = RT_BAD_FRAME,  // reservado (las tramas malas van en TICK_REPORT)
};

// Un registro de la trama EVENT
struct NodeEvent {
    uint32_t tick; // ticks desde START
    EventType type;
    uint16_t task_id; // NO_TASK si no aplica
    uint16_t value; // dato extra según el tipo ("arg")
};

// Cola circular de tamaño fijo (sin memoria dinámica). La llena el tick
// y la vacía usb_send_events en lotes.
class EventQueue {
public:
    EventQueue();

    // Agrega al final. Devuelve false si está llena (el evento no entra).
    bool push(const NodeEvent &item);
    // Saca el más antiguo. Devuelve false si está vacía.
    bool pop(NodeEvent &item);

    bool empty() const;
    bool full() const;
    size_t size() const;
    size_t capacity() const;

private:
    NodeEvent buffer_[EVENT_QUEUE_CAPACITY];
    size_t head_; // siguiente posicion para escribir
    size_t tail_; // siguiente posicion para leer
    size_t count_; // eventos guardados
};
