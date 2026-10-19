#pragma once

#include <stddef.h>
#include <stdint.h>

#define EVENT_QUEUE_CAPACITY 64U
#define NO_TASK 0xFFFFFFFFU

// Tipos de evento 
enum class EventType : uint8_t {
    Release = 0x01,
    Run = 0x02,
    Complete = 0x03,
    DeadlineMiss = 0x04,
    Idle = 0x05,
    SporadicIgnored = 0x06,
    FrameError = 0x07,
    EventsLost = 0x08,
};

struct NodeEvent {
    uint32_t tick; // ticks desde START
    EventType type;
    uint32_t task_id; // NO_TASK si no aplica
    uint32_t value; // dato extra segun el tipo
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
