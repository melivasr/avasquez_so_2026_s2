#pragma once

#include <stddef.h>
#include <stdint.h>

#define EVENT_QUEUE_CAPACITY 256U // registros 

// id 0 = ninguna tarea (ocioso). Por eso una tarea no puede tener id 0.
#define NO_TASK 0U

// Tipos de evento 
enum class EventType : uint8_t {
    Release = 0x01,  // se liberó un trabajo; arg = número de trabajo
    Switch = 0x02,   // cambió la tarea en ejecución; arg = la anterior
    Complete = 0x03, // terminó un trabajo; arg = número de trabajo
    Miss = 0x04,  // plazo vencido, trabajo abortado; arg = número de trabajo
    SporadicIgnored = 0x05, // pulsación antes del intervalo mínimo
    TaskAdded = 0x06,  // reservado 
    TickLost = 0x07,  // ticks no procesados; arg = cuántos
    BadFrame = 0x08,  // reservado (las tramas malas van en TICK_REPORT)
};

// Un registro de la trama EVENT
struct NodeEvent {
    uint32_t tick; // ticks desde START
    EventType type;
    uint16_t task_id; // NO_TASK si no aplica
    uint16_t value; // dato extra según el tipo ("arg")
};

class EventQueue {
public:
    EventQueue();

    bool push(const NodeEvent &item);
    bool pop(NodeEvent &item);

    bool empty() const;
    bool full() const;
    size_t size() const;
    size_t capacity() const;

private:
    NodeEvent buffer_[EVENT_QUEUE_CAPACITY];
    size_t head_; // siguiente posicion para escribir
    size_t tail_; // siguiente posicion para leer
    size_t count_;
};
