#include "event_queue.h"
//Implementacion de cola circular de eventos.

// Cola vacía: head y tail en la misma posición
EventQueue::EventQueue(){
    head_ = 0;
    tail_ = 0;
    count_ = 0;
}

// Agrega un evento al final; con la cola llena no lo guarda
bool EventQueue::push(const NodeEvent &item){
    if (full()) {
        return false;
    }

    // escribir en head y avanzar dando la vuelta al final del arreglo
    buffer_[head_] = item;
    head_ = (head_ + 1) % EVENT_QUEUE_CAPACITY;
    ++count_;

    return true;
}

// Saca el evento más antiguo; con la cola vacía no hace nada
bool EventQueue::pop(NodeEvent &item){
    if (empty()) {
        return false;
    }

    // leer en tail (el más antiguo) y avanzar
    item = buffer_[tail_];
    tail_ = (tail_ + 1) % EVENT_QUEUE_CAPACITY;
    --count_;

    return true;
}

// No hay eventos pendientes
bool EventQueue::empty() const{
    return count_ == 0;
}

// Ya no cabe otro evento (head alcanzó a tail dando la vuelta)
bool EventQueue::full() const{
    return count_ == EVENT_QUEUE_CAPACITY;
}

// Eventos guardados en este momento
size_t EventQueue::size() const{
    return count_;
}

// Máximo de eventos que caben (EVENT_QUEUE_CAPACITY = 256)
size_t EventQueue::capacity() const{
    return EVENT_QUEUE_CAPACITY;
}
